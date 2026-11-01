#include "spi_link.h"

#include <stddef.h>
#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "hardware/structs/dma.h"
#include "pico/platform.h"
#include "pico/stdlib.h"

#include "log.h"

#if !defined(PICODAC_SPI_ROLE_MASTER) && !defined(PICODAC_SPI_ROLE_SLAVE)
#define PICODAC_SPI_ROLE_MASTER 1
#endif

#define SPI_PORT spi0
#define SPI_PIN_RX 16u
#define SPI_PIN_CS 17u
#define SPI_PIN_SCK 18u
#define SPI_PIN_TX 19u

#define SPI_LINK_BAUD_HZ 7500000u
#define SPI_PACKET_INTERVAL_US 4000u
#define SPI_PACKET_TIMEOUT_US 12000u
#define SPI_MAX_PACKET_FRAMES 192u
#define SPI_AUDIO_FIFO_FRAMES 4096u
#define SPI_SLAVE_RX_SLOTS 3u
#define SPI_SLAVE_TX_SLOTS 2u
#define SPI_FRAME_MAGIC 0x50434155u

typedef struct __attribute__((packed, aligned(4))) {
  uint32_t magic;
  uint32_t sequence;
  uint32_t sample_rate;
  uint16_t frames;
  uint16_t flags;
  int32_t samples[SPI_MAX_PACKET_FRAMES * 2u];
  uint32_t crc32;
} spi_audio_frame_t;

_Static_assert(sizeof(spi_audio_frame_t) == 1556u,
               "SPI audio frame size changed; update the transport timing");

static int32_t tx_ring[SPI_AUDIO_FIFO_FRAMES * 2u];
static uint32_t tx_read_frame;
static uint32_t tx_write_frame;
static uint32_t tx_fill_frames;

static int32_t rx_ring[SPI_AUDIO_FIFO_FRAMES * 2u];
static uint32_t rx_read_frame;
static uint32_t rx_write_frame;
static uint32_t rx_fill_frames;

static uint32_t local_sample_rate = 48000u;
static uint32_t remote_sample_rate = 48000u;
static uint32_t tx_sequence;
static uint32_t packet_frame_phase;
static bool initialized;

static const uint32_t crc32_table[16] = {
    0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
    0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
    0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
    0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
};

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
  uint32_t frames = local_sample_rate / 250u;
  packet_frame_phase += local_sample_rate % 250u;
  if (packet_frame_phase >= 250u) {
    packet_frame_phase -= 250u;
    ++frames;
  }
  if (frames > SPI_MAX_PACKET_FRAMES) frames = SPI_MAX_PACKET_FRAMES;
  return (uint16_t)frames;
}

static void tx_pop_or_silence(int32_t *destination, uint32_t frames) {
  for (uint32_t i = 0; i < frames; ++i) {
    if (tx_fill_frames != 0) {
      const uint32_t source = tx_read_frame * 2u;
      destination[i * 2u] = tx_ring[source];
      destination[i * 2u + 1u] = tx_ring[source + 1u];
      tx_read_frame = (tx_read_frame + 1u) % SPI_AUDIO_FIFO_FRAMES;
      --tx_fill_frames;
    } else {
      destination[i * 2u] = 0;
      destination[i * 2u + 1u] = 0;
    }
  }
}

static void build_tx_frame(spi_audio_frame_t *frame) {
  memset(frame, 0, sizeof(*frame));
  frame->magic = SPI_FRAME_MAGIC;
  frame->sequence = tx_sequence++;
  frame->sample_rate = local_sample_rate;
  frame->frames = next_packet_frame_count();
  frame->flags = 0;
  tx_pop_or_silence(frame->samples, frame->frames);
  frame->crc32 = crc32_compute(frame, offsetof(spi_audio_frame_t, crc32));
}

static void rx_ring_push(const int32_t *samples, uint32_t frames,
                         uint32_t sample_rate) {
  if (sample_rate != remote_sample_rate) {
    rx_read_frame = 0;
    rx_write_frame = 0;
    rx_fill_frames = 0;
    remote_sample_rate = sample_rate;
  }

  if (frames > SPI_AUDIO_FIFO_FRAMES) {
    samples += (frames - SPI_AUDIO_FIFO_FRAMES) * 2u;
    frames = SPI_AUDIO_FIFO_FRAMES;
  }

  while (rx_fill_frames + frames > SPI_AUDIO_FIFO_FRAMES) {
    rx_read_frame = (rx_read_frame + 1u) % SPI_AUDIO_FIFO_FRAMES;
    --rx_fill_frames;
  }

  for (uint32_t i = 0; i < frames; ++i) {
    const uint32_t destination = rx_write_frame * 2u;
    rx_ring[destination] = samples[i * 2u];
    rx_ring[destination + 1u] = samples[i * 2u + 1u];
    rx_write_frame = (rx_write_frame + 1u) % SPI_AUDIO_FIFO_FRAMES;
    ++rx_fill_frames;
  }
}

static bool process_rx_frame(const spi_audio_frame_t *frame) {
  if (frame->magic != SPI_FRAME_MAGIC ||
      !supported_rate(frame->sample_rate) ||
      frame->frames > SPI_MAX_PACKET_FRAMES) {
    return false;
  }

  const uint32_t expected =
      crc32_compute(frame, offsetof(spi_audio_frame_t, crc32));
  if (expected != frame->crc32) return false;

  rx_ring_push(frame->samples, frame->frames, frame->sample_rate);
  return true;
}

void spi_link_set_local_sample_rate(uint32_t sample_rate) {
  if (!supported_rate(sample_rate)) return;
  if (local_sample_rate != sample_rate) {
    local_sample_rate = sample_rate;
    packet_frame_phase = 0;
  }
}

uint32_t spi_link_get_local_sample_rate(void) { return local_sample_rate; }
uint32_t spi_link_get_remote_sample_rate(void) { return remote_sample_rate; }

uint32_t spi_link_write_tx(const int32_t *samples, uint32_t frames) {
  if (!samples || frames == 0) return 0;
  uint32_t free_frames = SPI_AUDIO_FIFO_FRAMES - tx_fill_frames;
  if (frames > free_frames) frames = free_frames;

  for (uint32_t i = 0; i < frames; ++i) {
    const uint32_t destination = tx_write_frame * 2u;
    tx_ring[destination] = samples[i * 2u];
    tx_ring[destination + 1u] = samples[i * 2u + 1u];
    tx_write_frame = (tx_write_frame + 1u) % SPI_AUDIO_FIFO_FRAMES;
    ++tx_fill_frames;
  }
  return frames;
}

uint32_t spi_link_get_tx_queued_frames(void) { return tx_fill_frames; }

uint32_t spi_link_get_tx_free_frames(void) {
  return SPI_AUDIO_FIFO_FRAMES - tx_fill_frames;
}

void spi_link_clear_tx(void) {
  tx_read_frame = 0;
  tx_write_frame = 0;
  tx_fill_frames = 0;
}

uint32_t spi_link_read_rx(int32_t *samples, uint32_t max_frames,
                          uint32_t *source_sample_rate) {
  if (source_sample_rate) *source_sample_rate = remote_sample_rate;
  if (!samples || max_frames == 0) return 0;
  if (max_frames > rx_fill_frames) max_frames = rx_fill_frames;

  for (uint32_t i = 0; i < max_frames; ++i) {
    const uint32_t source = rx_read_frame * 2u;
    samples[i * 2u] = rx_ring[source];
    samples[i * 2u + 1u] = rx_ring[source + 1u];
    rx_read_frame = (rx_read_frame + 1u) % SPI_AUDIO_FIFO_FRAMES;
    --rx_fill_frames;
  }
  return max_frames;
}

static void configure_dma_channels(int *tx_channel, int *rx_channel) {
  *tx_channel = dma_claim_unused_channel(true);
  *rx_channel = dma_claim_unused_channel(true);
}

static void configure_tx_dma(int channel) {
  dma_channel_config config = dma_channel_get_default_config(channel);
  channel_config_set_transfer_data_size(&config, DMA_SIZE_8);
  channel_config_set_read_increment(&config, true);
  channel_config_set_write_increment(&config, false);
  channel_config_set_dreq(&config, spi_get_dreq(SPI_PORT, true));
  dma_channel_configure(channel, &config, &spi_get_hw(SPI_PORT)->dr,
                        NULL, sizeof(spi_audio_frame_t), false);
}

static void configure_rx_dma(int channel) {
  dma_channel_config config = dma_channel_get_default_config(channel);
  channel_config_set_transfer_data_size(&config, DMA_SIZE_8);
  channel_config_set_read_increment(&config, false);
  channel_config_set_write_increment(&config, true);
  channel_config_set_dreq(&config, spi_get_dreq(SPI_PORT, false));
  dma_channel_configure(channel, &config, NULL, &spi_get_hw(SPI_PORT)->dr,
                        sizeof(spi_audio_frame_t), false);
}

static void flush_spi_rx_fifo(void) {
  while (spi_is_readable(SPI_PORT)) {
    (void)spi_get_hw(SPI_PORT)->dr;
  }
  /* Clear receive-overrun and receive-timeout sticky flags. */
  spi_get_hw(SPI_PORT)->icr = 3u;
}

#if defined(PICODAC_SPI_ROLE_MASTER)

static int master_tx_dma = -1;
static int master_rx_dma = -1;
static spi_audio_frame_t master_tx_frame;
static spi_audio_frame_t master_rx_frame;
static volatile bool master_rx_done;
static bool master_busy;
static uint64_t master_started_us;
static uint64_t next_transfer_us;

static void spi_link_dma_irq(void) {
  const uint32_t mask = 1u << master_rx_dma;
  if (dma_hw->ints0 & mask) {
    dma_hw->ints0 = mask;
    master_rx_done = true;
  }
}

static void start_master_transfer(void) {
  build_tx_frame(&master_tx_frame);
  memset(&master_rx_frame, 0, sizeof(master_rx_frame));

  dma_channel_set_write_addr(master_rx_dma, &master_rx_frame, false);
  dma_channel_set_transfer_count(master_rx_dma, sizeof(master_rx_frame), false);
  dma_channel_set_read_addr(master_tx_dma, &master_tx_frame, false);
  dma_channel_set_transfer_count(master_tx_dma, sizeof(master_tx_frame), false);

  master_rx_done = false;
  master_started_us = time_us_64();
  next_transfer_us = master_started_us + SPI_PACKET_INTERVAL_US;
  gpio_put(SPI_PIN_CS, 0);
  dma_start_channel_mask((1u << master_rx_dma) | (1u << master_tx_dma));
  master_busy = true;
}

static void master_abort_transfer(void) {
  dma_channel_abort(master_tx_dma);
  dma_channel_abort(master_rx_dma);
  dma_hw->ints0 = 1u << master_rx_dma;
  master_rx_done = false;
  gpio_put(SPI_PIN_CS, 1);
  flush_spi_rx_fifo();
  master_busy = false;
  next_transfer_us = time_us_64() + SPI_PACKET_INTERVAL_US;
}

static void master_link_init(void) {
  spi_init(SPI_PORT, SPI_LINK_BAUD_HZ);
  spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  gpio_set_function(SPI_PIN_RX, GPIO_FUNC_SPI);
  gpio_set_function(SPI_PIN_SCK, GPIO_FUNC_SPI);
  gpio_set_function(SPI_PIN_TX, GPIO_FUNC_SPI);

  gpio_init(SPI_PIN_CS);
  gpio_set_dir(SPI_PIN_CS, GPIO_OUT);
  gpio_put(SPI_PIN_CS, 1);

  configure_dma_channels(&master_tx_dma, &master_rx_dma);
  configure_tx_dma(master_tx_dma);
  configure_rx_dma(master_rx_dma);
  dma_channel_set_irq0_enabled(master_rx_dma, true);
  irq_set_exclusive_handler(DMA_IRQ_0, spi_link_dma_irq);
  irq_set_enabled(DMA_IRQ_0, true);
  next_transfer_us = time_us_64() + SPI_PACKET_INTERVAL_US;
}

static void master_link_task(void) {
  const uint64_t now = time_us_64();

  if (master_busy && master_rx_done && !spi_is_busy(SPI_PORT)) {
    gpio_put(SPI_PIN_CS, 1);
    master_busy = false;
    (void)process_rx_frame(&master_rx_frame);
  }

  if (master_busy && now - master_started_us > SPI_PACKET_TIMEOUT_US) {
    master_abort_transfer();
  }

  if (!master_busy && now >= next_transfer_us) {
    flush_spi_rx_fifo();
    start_master_transfer();
  }
}

#else /* PICODAC_SPI_ROLE_SLAVE */

static int slave_tx_dma = -1;
static int slave_rx_dma = -1;
static spi_audio_frame_t slave_rx_frames[SPI_SLAVE_RX_SLOTS];
static spi_audio_frame_t slave_rx_discard;
static spi_audio_frame_t slave_tx_frames[SPI_SLAVE_TX_SLOTS];
static spi_audio_frame_t slave_tx_silence;
static volatile bool slave_rx_ready[SPI_SLAVE_RX_SLOTS];
static volatile bool slave_tx_ready[SPI_SLAVE_TX_SLOTS];
static volatile uint8_t slave_rx_queue[SPI_SLAVE_RX_SLOTS];
static volatile uint8_t slave_rx_head;
static volatile uint8_t slave_rx_tail;
static volatile uint8_t slave_rx_count;
static volatile int8_t slave_rx_active = -1;
static volatile int8_t slave_tx_active = -1;
static volatile bool slave_rx_discard_active;

static int find_free_rx_slot(void) {
  for (uint32_t i = 0; i < SPI_SLAVE_RX_SLOTS; ++i) {
    if (!slave_rx_ready[i] && (int8_t)i != slave_rx_active) return (int)i;
  }
  return -1;
}

static int find_ready_tx_slot(void) {
  for (uint32_t i = 0; i < SPI_SLAVE_TX_SLOTS; ++i) {
    if (slave_tx_ready[i]) return (int)i;
  }
  return -1;
}

static void arm_slave_dma(void) {
  uint8_t *rx_destination = slave_rx_discard_active
                                ? (uint8_t *)&slave_rx_discard
                                : (uint8_t *)&slave_rx_frames[slave_rx_active];
  const uint8_t *tx_source = (slave_tx_active < 0)
                                 ? (const uint8_t *)&slave_tx_silence
                                 : (const uint8_t *)&slave_tx_frames[slave_tx_active];

  dma_channel_set_read_addr(slave_rx_dma, &spi_get_hw(SPI_PORT)->dr, false);
  dma_channel_set_write_addr(slave_rx_dma, rx_destination, false);
  dma_channel_set_transfer_count(slave_rx_dma, sizeof(spi_audio_frame_t), false);
  dma_channel_set_read_addr(slave_tx_dma, tx_source, false);
  dma_channel_set_write_addr(slave_tx_dma, &spi_get_hw(SPI_PORT)->dr, false);
  dma_channel_set_transfer_count(slave_tx_dma, sizeof(spi_audio_frame_t), false);
  dma_start_channel_mask((1u << slave_rx_dma) | (1u << slave_tx_dma));
}

static void slave_cs_irq(uint gpio, uint32_t events) {
  if (gpio != SPI_PIN_CS || !(events & GPIO_IRQ_EDGE_RISE)) return;

  const bool rx_complete = !dma_channel_is_busy(slave_rx_dma);
  const bool tx_complete = !dma_channel_is_busy(slave_tx_dma);

  if (rx_complete && tx_complete && !slave_rx_discard_active &&
      slave_rx_active >= 0 && slave_rx_count < SPI_SLAVE_RX_SLOTS) {
    const uint8_t slot = (uint8_t)slave_rx_active;
    slave_rx_ready[slot] = true;
    slave_rx_queue[slave_rx_tail] = slot;
    slave_rx_tail = (uint8_t)((slave_rx_tail + 1u) % SPI_SLAVE_RX_SLOTS);
    ++slave_rx_count;
  }

  if (!rx_complete || !tx_complete) {
    dma_channel_abort(slave_rx_dma);
    dma_channel_abort(slave_tx_dma);
    /* A partial SPI transaction can leave bytes queued in the TX FIFO.
       Reset the peripheral so the next CS assertion starts on byte zero. */
    spi_deinit(SPI_PORT);
    spi_init(SPI_PORT, SPI_LINK_BAUD_HZ);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    spi_set_slave(SPI_PORT, true);
  }

  slave_rx_active = -1;
  slave_rx_active = (int8_t)find_free_rx_slot();
  slave_rx_discard_active = slave_rx_active < 0;

  slave_tx_active = (int8_t)find_ready_tx_slot();
  if (slave_tx_active >= 0) {
    slave_tx_ready[slave_tx_active] = false;
  }

  flush_spi_rx_fifo();
  arm_slave_dma();
}

static void slave_link_init(void) {
  spi_init(SPI_PORT, SPI_LINK_BAUD_HZ);
  spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  gpio_set_function(SPI_PIN_RX, GPIO_FUNC_SPI);
  gpio_set_function(SPI_PIN_SCK, GPIO_FUNC_SPI);
  gpio_set_function(SPI_PIN_TX, GPIO_FUNC_SPI);
  gpio_set_function(SPI_PIN_CS, GPIO_FUNC_SPI);
  gpio_pull_up(SPI_PIN_CS);
  spi_set_slave(SPI_PORT, true);

  configure_dma_channels(&slave_tx_dma, &slave_rx_dma);
  configure_tx_dma(slave_tx_dma);
  configure_rx_dma(slave_rx_dma);

  memset((void *)slave_rx_ready, 0, sizeof(slave_rx_ready));
  memset((void *)slave_tx_ready, 0, sizeof(slave_tx_ready));
  memset(&slave_rx_discard, 0, sizeof(slave_rx_discard));

  build_tx_frame(&slave_tx_frames[0]);
  build_tx_frame(&slave_tx_frames[1]);
  memset(&slave_tx_silence, 0, sizeof(slave_tx_silence));
  slave_tx_silence.magic = SPI_FRAME_MAGIC;
  slave_tx_silence.sample_rate = local_sample_rate;
  slave_tx_silence.frames = next_packet_frame_count();
  slave_tx_silence.crc32 =
      crc32_compute(&slave_tx_silence, offsetof(spi_audio_frame_t, crc32));

  slave_tx_active = 0;
  slave_tx_ready[1] = true;
  slave_rx_active = 0;
  slave_rx_discard_active = false;

  gpio_set_irq_enabled_with_callback(SPI_PIN_CS, GPIO_IRQ_EDGE_RISE, true,
                                     &slave_cs_irq);
  flush_spi_rx_fifo();
  arm_slave_dma();
}

static void slave_process_completed_frames(void) {
  while (true) {
    int slot = -1;
    const uint32_t irq_state = save_and_disable_interrupts();
    if (slave_rx_count != 0) {
      slot = slave_rx_queue[slave_rx_head];
      slave_rx_head = (uint8_t)((slave_rx_head + 1u) % SPI_SLAVE_RX_SLOTS);
      --slave_rx_count;
    }
    restore_interrupts(irq_state);

    if (slot < 0) break;
    (void)process_rx_frame(&slave_rx_frames[slot]);

    const uint32_t finish_irq_state = save_and_disable_interrupts();
    slave_rx_ready[slot] = false;
    restore_interrupts(finish_irq_state);
  }
}

static void slave_prepare_tx_frame(void) {
  int slot = -1;
  const uint32_t irq_state = save_and_disable_interrupts();
  for (uint32_t i = 0; i < SPI_SLAVE_TX_SLOTS; ++i) {
    if ((int8_t)i != slave_tx_active && !slave_tx_ready[i]) {
      slot = (int)i;
      break;
    }
  }
  restore_interrupts(irq_state);

  if (slot < 0) return;
  build_tx_frame(&slave_tx_frames[slot]);

  const uint32_t ready_irq_state = save_and_disable_interrupts();
  slave_tx_ready[slot] = true;
  restore_interrupts(ready_irq_state);
}

static void slave_link_task(void) {
  slave_process_completed_frames();
  slave_prepare_tx_frame();
}

#endif

void spi_link_init(void) {
  if (initialized) return;
  initialized = true;
  tx_read_frame = tx_write_frame = tx_fill_frames = 0;
  rx_read_frame = rx_write_frame = rx_fill_frames = 0;
  remote_sample_rate = 48000u;
  tx_sequence = 0;
  packet_frame_phase = 0;

#if defined(PICODAC_SPI_ROLE_SLAVE)
  slave_link_init();
  LOG_INFO("SPI0 audio link initialized as peripheral/slave at %u Hz",
           SPI_LINK_BAUD_HZ);
#else
  master_link_init();
  LOG_INFO("SPI0 audio link initialized as controller/master at %u Hz",
           SPI_LINK_BAUD_HZ);
#endif
}

void spi_link_task(void) {
  if (!initialized) return;
#if defined(PICODAC_SPI_ROLE_SLAVE)
  slave_link_task();
#else
  master_link_task();
#endif
}
