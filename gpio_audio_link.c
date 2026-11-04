#include "gpio_audio_link.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/*
 * 1:1 connection between the two boards. GPIO numbers are used here, not
 * physical header positions.
 *
 * Master owns GP17/GP18/GP19 as outputs and reads GP16.
 * Slave owns GP16 as its only output and reads GP17/GP18/GP19.
 *
 * These directions never change after initialization. There is no point in
 * this protocol where both MCUs drive the same connected net.
 */
#define LINK_GPIO_MISO 16u
#define LINK_GPIO_CS   17u
#define LINK_GPIO_SCK  18u
#define LINK_GPIO_MOSI 19u

#define LINK_MISO_MASK (1u << LINK_GPIO_MISO)
#define LINK_CS_MASK   (1u << LINK_GPIO_CS)
#define LINK_SCK_MASK  (1u << LINK_GPIO_SCK)
#define LINK_MOSI_MASK (1u << LINK_GPIO_MOSI)
#define LINK_MASTER_INPUT_MASK (LINK_CS_MASK | LINK_SCK_MASK | LINK_MOSI_MASK)

/* Startup preflight */
#define LINK_PROBE_PULSE_US             10000u
#define LINK_PROBE_ACK_HOLD_US          12000u
#define LINK_PROBE_ACK_TIMEOUT_US       30000u
#define LINK_PROBE_RELEASE_TIMEOUT_US   50000u
#define LINK_PROBE_RETRY_INTERVAL_US   250000u
#define LINK_PROBE_LOG_INTERVAL_US    2000000u
#define LINK_PROBE_COMMIT_SETTLE_US     20000u

/* Slow, intentionally unoptimized Stage-1 C bitbang diagnostic */
#define DIAG_ROUNDS                    64u
#define DIAG_HALF_PERIOD_US            12u
#define DIAG_CS_SETUP_US              100u
#define DIAG_EDGE_TIMEOUT_US       100000u
#define DIAG_RETRY_INTERVAL_US     250000u
#define DIAG_LOG_INTERVAL_US      2000000u

#define DIAG_SLAVE_MARKER             0xA5u
#define DIAG_COMMIT_TOKEN             0xC7u
#define DIAG_COMMIT_ACK               0xD4u
#define DIAG_FINAL_TOKEN              0x96u
#define DIAG_FINAL_ACK                0xE1u

/* This build only exposes PCM compatibility stubs; it does not stream PCM. */
#define DIAG_COMPAT_TX_CAPACITY_FRAMES 4096u

static volatile uint32_t diagnostic_passed;
static volatile uint32_t diagnostic_sessions;
static volatile uint32_t diagnostic_failed_sessions;
static volatile uint32_t local_sample_rate = 48000u;
static volatile uint32_t remote_sample_rate = 48000u;

static inline uint32_t atomic_load_u32(const volatile uint32_t *value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline void atomic_store_u32(volatile uint32_t *value, uint32_t next) {
  __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

static inline void atomic_add_u32(volatile uint32_t *value, uint32_t amount) {
  (void)__atomic_fetch_add(value, amount, __ATOMIC_RELAXED);
}

static void gpio_link_pins_init(void) {
  gpio_init(LINK_GPIO_MISO);
  gpio_init(LINK_GPIO_CS);
  gpio_init(LINK_GPIO_SCK);
  gpio_init(LINK_GPIO_MOSI);

#if defined(PICODAC_GPIO_LINK_ROLE_MASTER)
  /* Master-owned outputs are preloaded LOW before enabling their drivers. */
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
  /* Slave has exactly one output: GP16. All master-owned nets are inputs. */
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

static uint32_t gpio_probe_input_mask(void) {
  return sio_hw->gpio_in & LINK_MASTER_INPUT_MASK;
}

static bool wait_miso_level(bool high, uint32_t timeout_us) {
  const uint32_t started = time_us_32();
  do {
    if (gpio_get(LINK_GPIO_MISO) == high) return true;
    tight_loop_contents();
  } while ((uint32_t)(time_us_32() - started) < timeout_us);
  return false;
}

#if defined(PICODAC_GPIO_LINK_ROLE_MASTER)
static bool master_probe_output(uint32_t mask) {
  if (!wait_miso_level(false, LINK_PROBE_RELEASE_TIMEOUT_US)) return false;

  sio_hw->gpio_set = mask;
  busy_wait_us_32(LINK_PROBE_PULSE_US);
  sio_hw->gpio_clr = mask;

  if (!wait_miso_level(true, LINK_PROBE_ACK_TIMEOUT_US)) return false;
  return wait_miso_level(false, LINK_PROBE_RELEASE_TIMEOUT_US);
}

static void gpio_link_preflight(void) {
  static const uint32_t probes[] = {
      LINK_CS_MASK, LINK_SCK_MASK, LINK_MOSI_MASK};
  static const uint8_t probe_pins[] = {
      LINK_GPIO_CS, LINK_GPIO_SCK, LINK_GPIO_MOSI};
  uint32_t last_log = time_us_32();

  LOG_INFO("GPIO preflight: master owns GP17/GP18/GP19; slave ACKs on GP16");

  for (;;) {
    bool all_acknowledged = true;
    for (unsigned int i = 0; i < 3u; ++i) {
      if (master_probe_output(probes[i])) {
        LOG_INFO("GPIO preflight: GP%u acknowledged", (unsigned)probe_pins[i]);
      } else {
        all_acknowledged = false;
        break;
      }
    }

    if (all_acknowledged &&
        master_probe_output(LINK_CS_MASK | LINK_SCK_MASK)) {
      sio_hw->gpio_set = LINK_CS_MASK; /* Runtime CS idle is HIGH. */
      sio_hw->gpio_clr = LINK_SCK_MASK | LINK_MOSI_MASK;
      busy_wait_us_32(LINK_PROBE_COMMIT_SETTLE_US);
      LOG_INFO("GPIO preflight PASSED: all master outputs acknowledged");
      return;
    }

    const uint32_t now = time_us_32();
    if ((uint32_t)(now - last_log) >= LINK_PROBE_LOG_INTERVAL_US) {
      LOG_INFO("GPIO preflight: peer not ready; retrying");
      last_log = now;
    }
    busy_wait_us_32(LINK_PROBE_RETRY_INTERVAL_US);
  }
}

#else
static void slave_acknowledge_probe(void) {
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
  const uint32_t commit_mask = LINK_CS_MASK | LINK_SCK_MASK;
  uint32_t seen = 0;
  uint32_t last_log = time_us_32();

  LOG_INFO("GPIO preflight: slave listens on GP17/GP18/GP19; ACK output GP16");

  for (;;) {
    const uint32_t active = gpio_probe_input_mask();

    if (seen == LINK_MASTER_INPUT_MASK && active == commit_mask) {
      LOG_INFO("GPIO preflight: received master commit signature");
      slave_acknowledge_probe();
      gpio_pull_up(LINK_GPIO_CS); /* Runtime CS idle is HIGH. */
      LOG_INFO("GPIO preflight PASSED: fixed GPIO ownership established");
      return;
    }

    if (active != 0u) {
      const uint32_t newly_seen = active & ~seen;
      seen |= active;
      if (newly_seen & LINK_CS_MASK) LOG_INFO("GPIO probe detected GP17");
      if (newly_seen & LINK_SCK_MASK) LOG_INFO("GPIO probe detected GP18");
      if (newly_seen & LINK_MOSI_MASK) LOG_INFO("GPIO probe detected GP19");
      slave_acknowledge_probe();
      continue;
    }

    const uint32_t now = time_us_32();
    if ((uint32_t)(now - last_log) >= LINK_PROBE_LOG_INTERVAL_US) {
      LOG_INFO("GPIO preflight: waiting for master (seen mask 0x%08lx)",
               (unsigned long)seen);
      last_log = now;
    }
    tight_loop_contents();
  }
}
#endif

static uint8_t diagnostic_challenge(uint32_t index) {
  /* Nontrivial transitions exercise both logic levels and bit positions. */
  uint32_t x = index * 73u + 0x39u;
  x ^= x >> 3;
  x *= 0x45d9f3bu;
  x ^= x >> 11;
  return (uint8_t)x;
}

static uint8_t diagnostic_token(uint8_t challenge, uint32_t index) {
  return (uint8_t)(challenge ^ 0x5au ^ (uint8_t)(index * 13u));
}

static uint8_t diagnostic_reply(uint8_t challenge) {
  return (uint8_t)((challenge << 3) | (challenge >> 5)) ^ 0xC3u;
}

#if defined(PICODAC_GPIO_LINK_ROLE_MASTER)
/*
 * Mode-0 byte transfer in ordinary C. MOSI is stable before SCK rises; MISO
 * is sampled while SCK is HIGH. The generous delays are deliberate: this is
 * a correctness diagnostic, not the eventual audio-rate transport.
 */
static uint8_t master_transfer_byte(uint8_t transmit) {
  uint8_t receive = 0;

  for (int bit = 7; bit >= 0; --bit) {
    gpio_put(LINK_GPIO_SCK, 0);
    gpio_put(LINK_GPIO_MOSI, (transmit & (1u << bit)) != 0u);
    busy_wait_us_32(DIAG_HALF_PERIOD_US);

    gpio_put(LINK_GPIO_SCK, 1);
    busy_wait_us_32(DIAG_HALF_PERIOD_US);
    receive = (uint8_t)((receive << 1) |
                        (gpio_get(LINK_GPIO_MISO) ? 1u : 0u));

    gpio_put(LINK_GPIO_SCK, 0);
    busy_wait_us_32(DIAG_HALF_PERIOD_US);
  }

  return receive;
}

static bool master_diagnostic_session(void) {
  bool data_ok = true;
  gpio_put(LINK_GPIO_CS, 0);
  gpio_put(LINK_GPIO_SCK, 0);
  gpio_put(LINK_GPIO_MOSI, 0);
  busy_wait_us_32(DIAG_CS_SETUP_US);

  const uint32_t irq_state = save_and_disable_interrupts();
  for (uint32_t i = 0; i < DIAG_ROUNDS; ++i) {
    const uint8_t challenge = diagnostic_challenge(i);
    const uint8_t token = diagnostic_token(challenge, i);

    const uint8_t marker = master_transfer_byte(challenge);
    const uint8_t reply = master_transfer_byte(token);
    const uint8_t status = master_transfer_byte(0x00u);

    if (marker != DIAG_SLAVE_MARKER ||
        reply != diagnostic_reply(challenge) || status != 0xC3u) {
      data_ok = false;
    }
  }

  /* Three-way commit: the slave cannot pass until it sees the final token. */
  const uint8_t commit_reply =
      master_transfer_byte(data_ok ? DIAG_COMMIT_TOKEN : 0x00u);
  const bool slave_saw_all_data = data_ok && commit_reply == DIAG_COMMIT_ACK;
  const uint8_t final_reply =
      master_transfer_byte(slave_saw_all_data ? DIAG_FINAL_TOKEN : 0x00u);
  const bool passed = slave_saw_all_data && final_reply == DIAG_FINAL_ACK;

  gpio_put(LINK_GPIO_MOSI, 0);
  gpio_put(LINK_GPIO_SCK, 0);
  gpio_put(LINK_GPIO_CS, 1);
  restore_interrupts(irq_state);

  atomic_add_u32(&diagnostic_sessions, 1u);
  if (!passed) atomic_add_u32(&diagnostic_failed_sessions, 1u);
  return passed;
}

static void run_master_diagnostic(void) {
  uint32_t last_log = time_us_32();
  uint32_t attempt = 0;

  /* Let the slave finish launching its C receiver after the commit ACK. */
  busy_wait_us_32(30000u);
  LOG_INFO("GPIO C diagnostic: sending %u full-duplex challenge rounds at slow clock",
           DIAG_ROUNDS);

  for (;;) {
    ++attempt;
    if (master_diagnostic_session()) {
      atomic_store_u32(&diagnostic_passed, 1u);
      LOG_INFO("GPIO C diagnostic PASSED: %u rounds, master->slave and slave->master",
               DIAG_ROUNDS);
      return;
    }

    const uint32_t now = time_us_32();
    if ((uint32_t)(now - last_log) >= DIAG_LOG_INTERVAL_US) {
      LOG_WARN("GPIO C diagnostic: attempt %lu failed; retrying (sessions=%lu failed=%lu)",
               (unsigned long)attempt,
               (unsigned long)atomic_load_u32(&diagnostic_sessions),
               (unsigned long)atomic_load_u32(&diagnostic_failed_sessions));
      last_log = now;
    }
    busy_wait_us_32(DIAG_RETRY_INTERVAL_US);
  }
}

#else /* PICODAC_GPIO_LINK_ROLE_SLAVE */
static bool wait_clock_level(bool high) {
  const uint32_t started = time_us_32();
  do {
    if (gpio_get(LINK_GPIO_SCK) == high) return true;
    tight_loop_contents();
  } while ((uint32_t)(time_us_32() - started) < DIAG_EDGE_TIMEOUT_US);
  return false;
}

/*
 * Slave-side Mode-0 transfer. The slave preloads MISO before the first rising
 * edge, samples MOSI on rising edges, and advances MISO after falling edges.
 */
static bool slave_transfer_byte(uint8_t transmit, uint8_t *received) {
  uint8_t value = 0;
  gpio_put(LINK_GPIO_MISO, (transmit & 0x80u) != 0u);

  for (int bit = 7; bit >= 0; --bit) {
    if (!wait_clock_level(true)) return false;
    value = (uint8_t)((value << 1) |
                      (gpio_get(LINK_GPIO_MOSI) ? 1u : 0u));

    if (!wait_clock_level(false)) return false;
    if (bit != 0) {
      gpio_put(LINK_GPIO_MISO,
               (transmit & (1u << (bit - 1))) != 0u);
    }
  }

  if (received) *received = value;
  return true;
}

static void wait_for_cs_release(void) {
  const uint32_t started = time_us_32();
  while (!gpio_get(LINK_GPIO_CS)) {
    if ((uint32_t)(time_us_32() - started) >= DIAG_EDGE_TIMEOUT_US) break;
    tight_loop_contents();
  }
  gpio_put(LINK_GPIO_MISO, 0);
}

static void slave_diagnostic_worker(void) {
  while (atomic_load_u32(&diagnostic_passed) == 0u) {
    while (gpio_get(LINK_GPIO_CS)) tight_loop_contents();
    busy_wait_us_32(DIAG_CS_SETUP_US / 2u);

    bool session_ok = true;
    const uint32_t irq_state = save_and_disable_interrupts();

    for (uint32_t i = 0; i < DIAG_ROUNDS; ++i) {
      uint8_t challenge = 0;
      uint8_t token = 0;
      uint8_t ignored = 0;

      if (!slave_transfer_byte(DIAG_SLAVE_MARKER, &challenge)) {
        session_ok = false;
        break;
      }
      if (!slave_transfer_byte(diagnostic_reply(challenge), &token)) {
        session_ok = false;
        break;
      }

      const bool token_ok = token == diagnostic_token(challenge, i);
      if (!token_ok) session_ok = false;
      if (!slave_transfer_byte(token_ok ? 0xC3u : 0x00u, &ignored)) {
        session_ok = false;
        break;
      }
    }

    uint8_t commit_token = 0;
    uint8_t final_token = 0;

    if (session_ok) {
      if (!slave_transfer_byte(DIAG_COMMIT_ACK, &commit_token)) {
        session_ok = false;
      }
    }

    const bool commit_ok = session_ok && commit_token == DIAG_COMMIT_TOKEN;
    if (session_ok) {
      if (!slave_transfer_byte(commit_ok ? DIAG_FINAL_ACK : 0x00u,
                               &final_token)) {
        session_ok = false;
      }
    }

    const bool passed = session_ok && commit_ok &&
                        final_token == DIAG_FINAL_TOKEN;
    restore_interrupts(irq_state);
    wait_for_cs_release();

    atomic_add_u32(&diagnostic_sessions, 1u);
    if (passed) {
      atomic_store_u32(&diagnostic_passed, 1u);
      break;
    }
    atomic_add_u32(&diagnostic_failed_sessions, 1u);
  }

  gpio_put(LINK_GPIO_MISO, 0);
  for (;;) tight_loop_contents();
}

static void wait_for_master_diagnostic(void) {
  uint32_t last_log = time_us_32();
  LOG_INFO("GPIO C diagnostic: slave receiver running; waiting for master test");
  while (atomic_load_u32(&diagnostic_passed) == 0u) {
    const uint32_t now = time_us_32();
    if ((uint32_t)(now - last_log) >= DIAG_LOG_INTERVAL_US) {
      LOG_INFO("GPIO C diagnostic: still waiting (sessions=%lu failed=%lu)",
               (unsigned long)atomic_load_u32(&diagnostic_sessions),
               (unsigned long)atomic_load_u32(&diagnostic_failed_sessions));
      last_log = now;
    }
    sleep_ms(10u);
  }
  LOG_INFO("GPIO C diagnostic PASSED: slave received all challenge tokens and final commit");
}
#endif

void gpio_audio_link_init(void) {
  atomic_store_u32(&diagnostic_passed, 0u);
  atomic_store_u32(&diagnostic_sessions, 0u);
  atomic_store_u32(&diagnostic_failed_sessions, 0u);
  gpio_link_pins_init();
  gpio_link_preflight();

#if defined(PICODAC_GPIO_LINK_ROLE_MASTER)
  run_master_diagnostic();
#else
  multicore_launch_core1(slave_diagnostic_worker);
  wait_for_master_diagnostic();
#endif

  LOG_INFO("GPIO link READY: fixed ownership GP17/GP18/GP19 -> GP16; C diagnostic only");
}

void gpio_audio_link_task(void) {
  /* Deliberately no audio transport worker in this correctness-first stage. */
}

void gpio_audio_link_set_local_sample_rate(uint32_t sample_rate) {
  if (sample_rate == 44100u || sample_rate == 48000u) {
    atomic_store_u32(&local_sample_rate, sample_rate);
  }
}

uint32_t gpio_audio_link_get_local_sample_rate(void) {
  return atomic_load_u32(&local_sample_rate);
}

uint32_t gpio_audio_link_get_remote_sample_rate(void) {
  return atomic_load_u32(&remote_sample_rate);
}

uint32_t gpio_audio_link_write_tx(const int32_t *samples, uint32_t frames) {
  /* PCM is intentionally discarded until the basic GPIO link is proven. */
  (void)samples;
  return frames;
}

uint32_t gpio_audio_link_get_tx_queued_frames(void) {
  return 0u;
}

uint32_t gpio_audio_link_get_tx_free_frames(void) {
  return DIAG_COMPAT_TX_CAPACITY_FRAMES;
}

void gpio_audio_link_clear_tx(void) {
  /* No PCM queue exists in the Stage-1 diagnostic build. */
}

uint32_t gpio_audio_link_read_rx(int32_t *samples, uint32_t max_frames,
                                 uint32_t *source_sample_rate) {
  (void)samples;
  (void)max_frames;
  if (source_sample_rate) {
    *source_sample_rate = atomic_load_u32(&remote_sample_rate);
  }
  return 0u;
}
