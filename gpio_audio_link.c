#include "gpio_audio_link.h"
#include "gpio_audio_link_asm.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/sio.h"
#include "log.h"
#include "pico/multicore.h"
#include "pico/platform.h"
#include "pico/stdlib.h"

#if !defined(PICODAC_GPIO_LINK_ROLE_MASTER) && \
    !defined(PICODAC_GPIO_LINK_ROLE_SLAVE)
#define PICODAC_GPIO_LINK_ROLE_MASTER 1
#endif

#if defined(PICODAC_GPIO_LINK_ROLE_MASTER) && \
    defined(PICODAC_GPIO_LINK_ROLE_SLAVE)
#error "Select exactly one GPIO link role"
#endif

/* GPIO assignments are GPIO numbers, not physical header-pin numbers.
 *
 * This revision is for a straight-through stacked/interposer connection:
 * both boards use the same GPIO numbers on each side. The role controls the
 * direction, not the GPIO number:
 *   master: GP19 MOSI output, GP16 MISO input
 *   slave:  GP19 MOSI input,  GP16 MISO output
 *
 * GP17 is active-low chip select and GP18 is the master-generated clock.
 */
#define LINK_GPIO_MOSI 19u
#define LINK_GPIO_MISO 16u
#define LINK_GPIO_CS   17u
#define LINK_GPIO_SCK  18u

#define LINK_MISO_MASK (1u << LINK_GPIO_MISO)
#define LINK_CS_MASK   (1u << LINK_GPIO_CS)
#define LINK_SCK_MASK  (1u << LINK_GPIO_SCK)
#define LINK_MOSI_MASK (1u << LINK_GPIO_MOSI)

#define LINK_PACKET_INTERVAL_US 4000u
#define LINK_CS_SETUP_US 200u
#define LINK_MAX_PACKET_FRAMES 192u
#define LINK_AUDIO_FIFO_FRAMES 4096u
#define LINK_AUDIO_FIFO_MASK (LINK_AUDIO_FIFO_FRAMES - 1u)
#define LINK_FRAME_MAGIC 0x50434155u
#define LINK_STATS_INTERVAL_US 2000000u

/* Startup probe timings. The master emits 10 ms pulses; the slave holds its
 * MISO acknowledgement long enough for the master to sample it after the
 * pulse has ended. */
#define LINK_PROBE_PULSE_US 10000u
#define LINK_PROBE_ACK_HOLD_US 12000u
#define LINK_PROBE_ACK_TIMEOUT_US 20000u
#define LINK_PROBE_RELEASE_TIMEOUT_US 50000u
#define LINK_PROBE_RETRY_INTERVAL_US 250000u
#define LINK_PROBE_COMMIT_SETTLE_US 20000u

/*
 * One fixed 4 ms exchange. Both directions carry their own source rate and
 * frame count because the two USB hosts may select different nominal rates.
 * 16-byte header + 192 * 2 * 4-byte samples + 4-byte CRC = 1556 bytes.
 */
typedef struct __attribute__((packed, aligned(4))) {
  uint32_t magic;
  uint32_t sequence;
  uint32_t sample_rate;
  uint16_t frames;
  uint16_t flags;
  int32_t samples[LINK_MAX_PACKET_FRAMES * 2u];
  uint32_t crc32;
} link_audio_frame_t;

_Static_assert(sizeof(link_audio_frame_t) == 1556u,
               "GPIO link frame size changed unexpectedly");
_Static_assert((LINK_AUDIO_FIFO_FRAMES & LINK_AUDIO_FIFO_MASK) == 0,
               "GPIO link FIFO size must be a power of two");

/* Single-producer/single-consumer queues shared between core 0 and core 1. */
static int32_t tx_ring[LINK_AUDIO_FIFO_FRAMES * 2u];
static volatile uint32_t tx_head;
static volatile uint32_t tx_tail;
static volatile uint32_t tx_clear_requested;
static int32_t rx_ring[LINK_AUDIO_FIFO_FRAMES * 2u];
static volatile uint32_t rx_head;
static volatile uint32_t rx_tail;

static volatile uint32_t local_sample_rate = 48000u;
static volatile uint32_t remote_sample_rate = 48000u;
static volatile uint32_t link_initialized;

static volatile uint32_t stat_tx_packets;
static volatile uint32_t stat_rx_packets;
static volatile uint32_t stat_bad_header;
static volatile uint32_t stat_bad_crc;
static volatile uint32_t stat_tx_silence_frames;
static volatile uint32_t stat_rx_dropped_frames;
static volatile uint32_t stat_packet_overruns;
static volatile uint32_t stat_last_transfer_us __attribute__((unused));

static uint32_t tx_sequence;
static uint32_t packet_frame_phase;

static const uint32_t crc32_table[16] = {
    0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
    0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
    0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
    0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
};

static inline uint32_t atomic_load_u32(const volatile uint32_t *value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline void atomic_store_u32(volatile uint32_t *value,
                                    uint32_t next) {
  __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

static inline void atomic_add_u32(volatile uint32_t *value, uint32_t amount) {
  (void)__atomic_fetch_add(value, amount, __ATOMIC_RELAXED);
}

static uint32_t crc32_compute(const void *data, size_t length) {
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t crc = 0xffffffffu;
  while (length--) {
    crc ^= *bytes++;
    crc = (crc >> 4) ^ crc32_table[crc & 0x0fu];
    crc = (crc >> 4) ^ crc32_table[crc & 0x0fu];
  }
  return ~crc;
}

static bool supported_rate(uint32_t rate) {
  return rate == 44100u || rate == 48000u;
}

static uint16_t next_packet_frame_count(void) {
  const uint32_t rate = atomic_load_u32(&local_sample_rate);
  uint32_t frames = rate / 250u;
  packet_frame_phase += rate % 250u;
  if (packet_frame_phase >= 250u) {
    packet_frame_phase -= 250u;
    ++frames;
  }
  if (frames > LINK_MAX_PACKET_FRAMES) frames = LINK_MAX_PACKET_FRAMES;
  return (uint16_t)frames;
}

/*
 * Core 0 writes tx_ring; core 1 reads it. Publish the updated tail only after
 * every sample in the block has been stored.
 */
static void tx_pop_or_silence(int32_t *destination, uint32_t frames) {
  if (atomic_load_u32(&tx_clear_requested) != 0) {
    const uint32_t discard_to = atomic_load_u32(&tx_tail);
    atomic_store_u32(&tx_head, discard_to);
    atomic_store_u32(&tx_clear_requested, 0u);
  }
  uint32_t head = atomic_load_u32(&tx_head);
  const uint32_t tail = atomic_load_u32(&tx_tail);
  uint32_t available = tail - head;
  uint32_t copied = 0;
  if (available > LINK_AUDIO_FIFO_FRAMES) available = LINK_AUDIO_FIFO_FRAMES;
  if (available > frames) available = frames;

  while (copied < available) {
    const uint32_t source = (head & LINK_AUDIO_FIFO_MASK) * 2u;
    destination[copied * 2u] = tx_ring[source];
    destination[copied * 2u + 1u] = tx_ring[source + 1u];
    ++head;
    ++copied;
  }

  if (copied != 0) atomic_store_u32(&tx_head, head);
  if (copied < frames) {
    memset(destination + copied * 2u, 0,
           (size_t)(frames - copied) * 2u * sizeof(int32_t));
    atomic_add_u32(&stat_tx_silence_frames, frames - copied);
  }
}

static void build_tx_frame(link_audio_frame_t *frame) {
  memset(frame, 0, sizeof(*frame));
  frame->magic = LINK_FRAME_MAGIC;
  frame->sequence = tx_sequence++;
  frame->sample_rate = atomic_load_u32(&local_sample_rate);
  frame->frames = next_packet_frame_count();
  tx_pop_or_silence(frame->samples, frame->frames);
  frame->crc32 = crc32_compute(frame, offsetof(link_audio_frame_t, crc32));
}

/* Core 1 writes rx_ring; core 0 reads it. Full frames are dropped on overflow. */
static void rx_push_frame(const link_audio_frame_t *frame) {
  const uint32_t head = atomic_load_u32(&rx_head);
  uint32_t tail = atomic_load_u32(&rx_tail);
  const uint32_t used = tail - head;
  const uint32_t free_frames =
      used >= LINK_AUDIO_FIFO_FRAMES ? 0u : LINK_AUDIO_FIFO_FRAMES - used;

  if (free_frames < frame->frames) {
    atomic_add_u32(&stat_rx_dropped_frames, frame->frames);
    return;
  }

  for (uint32_t i = 0; i < frame->frames; ++i) {
    const uint32_t destination = (tail & LINK_AUDIO_FIFO_MASK) * 2u;
    rx_ring[destination] = frame->samples[i * 2u];
    rx_ring[destination + 1u] = frame->samples[i * 2u + 1u];
    ++tail;
  }

  atomic_store_u32(&remote_sample_rate, frame->sample_rate);
  atomic_store_u32(&rx_tail, tail);
  atomic_add_u32(&stat_rx_packets, 1u);
}

static void process_rx_frame(const link_audio_frame_t *frame) {
  if (frame->magic != LINK_FRAME_MAGIC ||
      !supported_rate(frame->sample_rate) ||
      frame->frames > LINK_MAX_PACKET_FRAMES) {
    atomic_add_u32(&stat_bad_header, 1u);
    return;
  }

  const uint32_t expected =
      crc32_compute(frame, offsetof(link_audio_frame_t, crc32));
  if (expected != frame->crc32) {
    atomic_add_u32(&stat_bad_crc, 1u);
    return;
  }

  rx_push_frame(frame);
}

static void gpio_link_pins_init(void) {
  /* Initialize every link pad to SIO before assigning any output direction. */
  gpio_init(LINK_GPIO_MOSI);
  gpio_init(LINK_GPIO_MISO);
  gpio_init(LINK_GPIO_CS);
  gpio_init(LINK_GPIO_SCK);

#if defined(PICODAC_GPIO_LINK_ROLE_MASTER)
  /*
   * Fixed ownership for the lifetime of this firmware:
   *   master outputs: GP17/GP18/GP19
   *   master input:   GP16
   * Preload LOW before enabling output drivers. GP17 is temporarily LOW
   * during startup probing; it becomes the normal inactive-HIGH CS only
   * after the peer has acknowledged the complete probe sequence.
   */
  gpio_pull_down(LINK_GPIO_MISO);
  gpio_set_dir(LINK_GPIO_MISO, GPIO_IN);

  gpio_put(LINK_GPIO_CS, 0);
  gpio_put(LINK_GPIO_SCK, 0);
  gpio_put(LINK_GPIO_MOSI, 0);
  gpio_set_drive_strength(LINK_GPIO_CS, GPIO_DRIVE_STRENGTH_4MA);
  gpio_set_drive_strength(LINK_GPIO_SCK, GPIO_DRIVE_STRENGTH_4MA);
  gpio_set_drive_strength(LINK_GPIO_MOSI, GPIO_DRIVE_STRENGTH_4MA);
  gpio_set_dir(LINK_GPIO_CS, GPIO_OUT);
  gpio_set_dir(LINK_GPIO_SCK, GPIO_OUT);
  gpio_set_dir(LINK_GPIO_MOSI, GPIO_OUT);
#else
  /*
   * Fixed ownership for the lifetime of this firmware:
   *   slave outputs: GP16 only (MISO)
   *   slave inputs:  GP17/GP18/GP19
   * Keep all master-owned inputs pulled LOW during preflight. In particular,
   * GP17 is not pulled HIGH until the master has completed its commit pulse.
   */
  gpio_pull_down(LINK_GPIO_CS);
  gpio_pull_down(LINK_GPIO_SCK);
  gpio_pull_down(LINK_GPIO_MOSI);
  gpio_set_dir(LINK_GPIO_CS, GPIO_IN);
  gpio_set_dir(LINK_GPIO_SCK, GPIO_IN);
  gpio_set_dir(LINK_GPIO_MOSI, GPIO_IN);

  gpio_put(LINK_GPIO_MISO, 0);
  gpio_set_drive_strength(LINK_GPIO_MISO, GPIO_DRIVE_STRENGTH_4MA);
  gpio_set_dir(LINK_GPIO_MISO, GPIO_OUT);
#endif
}

/*
 * The register layout is consumed directly by gpio_audio_link_bitbang.S.
 * Both assembly transfer functions execute from SRAM and are called with
 * core-1 interrupts disabled, so software edge detection has bounded latency.
 */
static const gpio_bitbang_context_t bitbang_context = {
    .gpio_in = &sio_hw->gpio_in,
    .gpio_set = &sio_hw->gpio_set,
    .gpio_clr = &sio_hw->gpio_clr,
    .cs_mask = LINK_CS_MASK,
    .sck_mask = LINK_SCK_MASK,
    .mosi_mask = LINK_MOSI_MASK,
    .miso_mask = LINK_MISO_MASK,
};
#if UINTPTR_MAX == 0xffffffffu
_Static_assert(offsetof(gpio_bitbang_context_t, gpio_in) == 0u &&
                   offsetof(gpio_bitbang_context_t, gpio_set) == 4u &&
                   offsetof(gpio_bitbang_context_t, gpio_clr) == 8u &&
                   offsetof(gpio_bitbang_context_t, cs_mask) == 12u &&
                   offsetof(gpio_bitbang_context_t, sck_mask) == 16u &&
                   offsetof(gpio_bitbang_context_t, mosi_mask) == 20u &&
                   offsetof(gpio_bitbang_context_t, miso_mask) == 24u,
               "GPIO bitbang context offsets must match assembly");
#endif

#if defined(PICODAC_GPIO_LINK_ROLE_MASTER)
static void master_bitbang_frame(const uint8_t *transmit, uint8_t *receive) {
  gpio_bitbang_master_transfer_asm(&bitbang_context, transmit, receive,
                                   sizeof(link_audio_frame_t));
}

static void master_worker(void) {
  static link_audio_frame_t tx_frame;
  static link_audio_frame_t rx_frame;
  uint64_t next_packet_us = time_us_64() + 1000u;

  while (true) {
    /* Prepare PCM and CRC before the scheduled chip-select edge. */
    build_tx_frame(&tx_frame);
    memset(&rx_frame, 0, sizeof(rx_frame));

    while (time_us_64() < next_packet_us) {
      tight_loop_contents();
    }

    /*
     * Keep core-1 interrupt latency out of the software clock. The 200 us
     * CS setup interval lets the slave arm its assembly receiver and place
     * the first MISO bit before SCK starts.
     */
    const uint32_t irq_state = save_and_disable_interrupts();
    sio_hw->gpio_clr = LINK_CS_MASK;
    busy_wait_us_32(LINK_CS_SETUP_US);
    const uint32_t transfer_start = time_us_32();
    master_bitbang_frame((const uint8_t *)&tx_frame,
                         (uint8_t *)&rx_frame);
    const uint32_t transfer_us = time_us_32() - transfer_start;
    sio_hw->gpio_set = LINK_CS_MASK;
    sio_hw->gpio_clr = LINK_MOSI_MASK | LINK_SCK_MASK;
    restore_interrupts(irq_state);

    atomic_store_u32(&stat_last_transfer_us, transfer_us);
    atomic_add_u32(&stat_tx_packets, 1u);
    process_rx_frame(&rx_frame);

    next_packet_us += LINK_PACKET_INTERVAL_US;
    const uint64_t now = time_us_64();
    if (now >= next_packet_us) {
      atomic_add_u32(&stat_packet_overruns, 1u);
      next_packet_us = now + 100u;
    }
  }
}

#else /* PICODAC_GPIO_LINK_ROLE_SLAVE */
static bool slave_bitbang_frame(const uint8_t *transmit, uint8_t *receive) {
  return gpio_bitbang_slave_transfer_asm(&bitbang_context, transmit, receive,
                                         sizeof(link_audio_frame_t)) != 0;
}

static void slave_worker(void) {
  static link_audio_frame_t tx_frame;
  static link_audio_frame_t rx_frame;
  bool tx_prepared = false;

  while (true) {
    /* Prepare the reply before listening for the next chip-select edge. */
    if (!tx_prepared) build_tx_frame(&tx_frame);
    tx_prepared = false;
    memset(&rx_frame, 0, sizeof(rx_frame));

    while ((sio_hw->gpio_in & LINK_CS_MASK) != 0) {
      tight_loop_contents();
    }

    /* Do not let an interrupt stretch either software-polled clock phase. */
    const uint32_t irq_state = save_and_disable_interrupts();
    const bool complete = slave_bitbang_frame((const uint8_t *)&tx_frame,
                                               (uint8_t *)&rx_frame);
    restore_interrupts(irq_state);

    if (!complete) {
      atomic_add_u32(&stat_packet_overruns, 1u);
      continue;
    }

    /* Wait for transaction termination before preparing the next response. */
    while ((sio_hw->gpio_in & LINK_CS_MASK) == 0) {
      tight_loop_contents();
    }

    /* Give next reply priority; prepare it before running CRC on this frame. */
    build_tx_frame(&tx_frame);
    tx_prepared = true;
    atomic_add_u32(&stat_tx_packets, 1u);
    process_rx_frame(&rx_frame);
  }
}

#endif

/* Read the three master-owned lines during the pre-UAC startup handshake. */
static uint32_t gpio_probe_input_mask(void) {
  return sio_hw->gpio_in & (LINK_CS_MASK | LINK_SCK_MASK | LINK_MOSI_MASK);
}

static bool gpio_wait_miso_level(bool high, uint32_t timeout_us) {
  const uint32_t started = time_us_32();
  do {
    if (gpio_get(LINK_GPIO_MISO) == high) return true;
    tight_loop_contents();
  } while ((uint32_t)(time_us_32() - started) < timeout_us);
  return false;
}

#if defined(PICODAC_GPIO_LINK_ROLE_MASTER)
/*
 * Exercise one master-owned output without ever changing the peer's pin
 * direction. The peer must acknowledge on its exclusively-owned MISO pin.
 */
static bool gpio_master_probe_mask(uint32_t output_mask) {
  if (!gpio_wait_miso_level(false, LINK_PROBE_RELEASE_TIMEOUT_US))
    return false;

  sio_hw->gpio_set = output_mask;
  busy_wait_us_32(LINK_PROBE_PULSE_US);
  sio_hw->gpio_clr = output_mask;

  if (!gpio_wait_miso_level(true, LINK_PROBE_ACK_TIMEOUT_US)) return false;
  return gpio_wait_miso_level(false, LINK_PROBE_RELEASE_TIMEOUT_US);
}

static void gpio_link_preflight(void) {
  static const uint32_t probe_masks[] = {
      LINK_CS_MASK, LINK_SCK_MASK, LINK_MOSI_MASK};
  static const uint8_t probe_gpio_numbers[] = {
      LINK_GPIO_CS, LINK_GPIO_SCK, LINK_GPIO_MOSI};
  uint32_t last_heartbeat_log = time_us_32();

  LOG_INFO("GPIO startup preflight: master heartbeat on GP17/GP18/GP19; waiting for slave on GP16");

  for (;;) {
    bool all_lines_acknowledged = true;

    /* Each output is tested alone, with a 10 ms HIGH pulse. */
    for (unsigned int i = 0; i < 3u; ++i) {
      if (gpio_master_probe_mask(probe_masks[i])) {
        LOG_INFO("GPIO startup probe: GP%u acknowledged by slave", (unsigned)probe_gpio_numbers[i]);
      } else {
        all_lines_acknowledged = false;
      }
    }

    if (all_lines_acknowledged) {
      /*
       * Commit signature: GP17 and GP18 HIGH simultaneously, GP19 LOW.
       * It cannot be confused with any of the three single-pin probes.
       */
      if (gpio_master_probe_mask(LINK_CS_MASK | LINK_SCK_MASK)) {
        sio_hw->gpio_set = LINK_CS_MASK;  /* normal inactive CS level */
        busy_wait_us_32(LINK_PROBE_COMMIT_SETTLE_US);
        LOG_INFO("GPIO startup preflight PASSED: all four signal directions verified");
        return;
      }
    }

    const uint32_t now = time_us_32();
    if ((uint32_t)(now - last_heartbeat_log) >= LINK_STATS_INTERVAL_US) {
      LOG_INFO("GPIO startup heartbeat: peer not ready; retrying GP17/GP18/GP19 probes");
      last_heartbeat_log = now;
    }
    busy_wait_us_32(LINK_PROBE_RETRY_INTERVAL_US);
  }
}
#else
/*
 * The slave owns GP16 and acknowledges every probe only on that output.
 * Wait for the master to finish (release) its probe before raising GP16.
 * This is deliberate: the master starts looking for the ACK only after its
 * 10 ms output pulse has ended, so the ACK cannot be missed at the edge.
 */
static void gpio_slave_acknowledge_probe(void) {
  const uint32_t started = time_us_32();
  while (gpio_probe_input_mask() != 0u) {
    if ((uint32_t)(time_us_32() - started) >=
        LINK_PROBE_RELEASE_TIMEOUT_US) {
      return;
    }
    tight_loop_contents();
  }

  sio_hw->gpio_set = LINK_MISO_MASK;
  busy_wait_us_32(LINK_PROBE_ACK_HOLD_US);
  sio_hw->gpio_clr = LINK_MISO_MASK;
}

static void gpio_link_preflight(void) {
  const uint32_t all_probes =
      LINK_CS_MASK | LINK_SCK_MASK | LINK_MOSI_MASK;
  const uint32_t commit_mask = LINK_CS_MASK | LINK_SCK_MASK;
  uint32_t seen_probes = 0;
  uint32_t last_heartbeat_log = time_us_32();

  LOG_INFO("GPIO startup preflight: slave listening on GP17/GP18/GP19; ACK output GP16");

  for (;;) {
    const uint32_t active = gpio_probe_input_mask();

    /* Only accept the two-line commit signature after all three single-line
     * master outputs have been observed and acknowledged. */
    if (seen_probes == all_probes && active == commit_mask) {
      LOG_INFO("GPIO startup preflight: received master's commit signature");
      gpio_slave_acknowledge_probe();
      gpio_pull_up(LINK_GPIO_CS);  /* runtime CS is active-low, idle HIGH */
      LOG_INFO("GPIO startup preflight PASSED: switching GP17 to runtime CS idle");
      return;
    }

    if (active != 0u) {
      const uint32_t newly_seen = active & ~seen_probes;
      seen_probes |= active;
      if (newly_seen & LINK_CS_MASK)
        LOG_INFO("GPIO startup probe detected: GP17");
      if (newly_seen & LINK_SCK_MASK)
        LOG_INFO("GPIO startup probe detected: GP18");
      if (newly_seen & LINK_MOSI_MASK)
        LOG_INFO("GPIO startup probe detected: GP19");
      gpio_slave_acknowledge_probe();
      continue;
    }

    const uint32_t now = time_us_32();
    if ((uint32_t)(now - last_heartbeat_log) >= LINK_STATS_INTERVAL_US) {
      LOG_INFO("GPIO startup heartbeat: waiting for master probes (seen mask 0x%08lx)",
               (unsigned long)seen_probes);
      last_heartbeat_log = now;
    }
    tight_loop_contents();
  }
}
#endif

void gpio_audio_link_init(void) {
  if (atomic_load_u32(&link_initialized)) return;

  tx_head = tx_tail = 0;
  tx_clear_requested = 0;
  rx_head = rx_tail = 0;
  tx_sequence = 0;
  packet_frame_phase = 0;
  atomic_store_u32(&remote_sample_rate, 48000u);
  gpio_link_pins_init();

  /*
   * This blocks until the counterpart has acknowledged every link output.
   * main.c calls gpio_audio_link_init() before audio and USB initialization,
   * so neither UAC2 interface is exposed until this handshake succeeds.
   */
  gpio_link_preflight();

  atomic_store_u32(&link_initialized, 1u);
  multicore_launch_core1(
#if defined(PICODAC_GPIO_LINK_ROLE_MASTER)
      master_worker
#else
      slave_worker
#endif
  );

#if defined(PICODAC_GPIO_LINK_ROLE_MASTER)
  LOG_INFO("GPIO bitbang audio link: master on GP17/GP18/GP19/GP16");
#else
  LOG_INFO("GPIO bitbang audio link: slave on GP17/GP18/GP16/GP19");
#endif
}

void gpio_audio_link_task(void) {
  static uint32_t last_log_us;
  const uint32_t now = time_us_32();
  if ((uint32_t)(now - last_log_us) < LINK_STATS_INTERVAL_US) return;
  last_log_us = now;

  LOG_INFO("GPIO link stats: TX=%lu RX-valid=%lu bad-header=%lu bad-CRC=%lu "
           "TX-silence-frames=%lu RX-dropped=%lu packet-overruns=%lu "
           "last-transfer=%lu us",
           (unsigned long)atomic_load_u32(&stat_tx_packets),
           (unsigned long)atomic_load_u32(&stat_rx_packets),
           (unsigned long)atomic_load_u32(&stat_bad_header),
           (unsigned long)atomic_load_u32(&stat_bad_crc),
           (unsigned long)atomic_load_u32(&stat_tx_silence_frames),
           (unsigned long)atomic_load_u32(&stat_rx_dropped_frames),
           (unsigned long)atomic_load_u32(&stat_packet_overruns),
           (unsigned long)atomic_load_u32(&stat_last_transfer_us));
}

void gpio_audio_link_set_local_sample_rate(uint32_t sample_rate) {
  if (!supported_rate(sample_rate)) return;
  atomic_store_u32(&local_sample_rate, sample_rate);
}

uint32_t gpio_audio_link_get_local_sample_rate(void) {
  return atomic_load_u32(&local_sample_rate);
}

uint32_t gpio_audio_link_get_remote_sample_rate(void) {
  return atomic_load_u32(&remote_sample_rate);
}

uint32_t gpio_audio_link_write_tx(const int32_t *samples, uint32_t frames) {
  if (!samples || frames == 0) return 0;

  uint32_t tail = atomic_load_u32(&tx_tail);
  const uint32_t head = atomic_load_u32(&tx_head);
  uint32_t used = tail - head;
  if (used > LINK_AUDIO_FIFO_FRAMES) used = LINK_AUDIO_FIFO_FRAMES;
  uint32_t free_frames = LINK_AUDIO_FIFO_FRAMES - used;
  if (frames > free_frames) frames = free_frames;

  for (uint32_t i = 0; i < frames; ++i) {
    const uint32_t destination = (tail & LINK_AUDIO_FIFO_MASK) * 2u;
    tx_ring[destination] = samples[i * 2u];
    tx_ring[destination + 1u] = samples[i * 2u + 1u];
    ++tail;
  }

  if (frames != 0) atomic_store_u32(&tx_tail, tail);
  return frames;
}

uint32_t gpio_audio_link_get_tx_queued_frames(void) {
  const uint32_t tail = atomic_load_u32(&tx_tail);
  const uint32_t head = atomic_load_u32(&tx_head);
  const uint32_t used = tail - head;
  return used > LINK_AUDIO_FIFO_FRAMES ? LINK_AUDIO_FIFO_FRAMES : used;
}

uint32_t gpio_audio_link_get_tx_free_frames(void) {
  return LINK_AUDIO_FIFO_FRAMES - gpio_audio_link_get_tx_queued_frames();
}

void gpio_audio_link_clear_tx(void) {
  /* Ask the core-1 consumer to discard queued samples at its next frame edge. */
  atomic_store_u32(&tx_clear_requested, 1u);
}

uint32_t gpio_audio_link_read_rx(int32_t *samples, uint32_t max_frames,
                                 uint32_t *source_sample_rate) {
  if (source_sample_rate)
    *source_sample_rate = atomic_load_u32(&remote_sample_rate);
  if (!samples || max_frames == 0) return 0;

  uint32_t head = atomic_load_u32(&rx_head);
  const uint32_t tail = atomic_load_u32(&rx_tail);
  uint32_t available = tail - head;
  if (available > LINK_AUDIO_FIFO_FRAMES) available = LINK_AUDIO_FIFO_FRAMES;
  if (max_frames > available) max_frames = available;

  for (uint32_t i = 0; i < max_frames; ++i) {
    const uint32_t source = (head & LINK_AUDIO_FIFO_MASK) * 2u;
    samples[i * 2u] = rx_ring[source];
    samples[i * 2u + 1u] = rx_ring[source + 1u];
    ++head;
  }

  if (max_frames != 0) atomic_store_u32(&rx_head, head);
  return max_frames;
}
