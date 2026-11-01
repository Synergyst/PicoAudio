#include "i2s.h"

#include <assert.h>
#include <stddef.h>

#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "i2s.pio.h"
#include "i2s_slave.pio.h"
#include "log.h"

#if !defined(PICODAC_I2S_ROLE_MASTER) && !defined(PICODAC_I2S_ROLE_SLAVE)
#define PICODAC_I2S_ROLE_MASTER 1
#endif

#if defined(PICODAC_I2S_ROLE_MASTER) && defined(PICODAC_I2S_ROLE_SLAVE)
#error "Select exactly one PICODAC_I2S_ROLE_MASTER or PICODAC_I2S_ROLE_SLAVE"
#endif

// --- Hardware Mute Pin ---
// To enable hardware mute, define I2S_MUTE_PIN to a valid GPIO number.
// #define I2S_MUTE_PIN 28

#define MAX_I2S_SAMPLE_RATE 96000u
#define MAX_BUFFER_FRAMES (MAX_I2S_SAMPLE_RATE / 1000u)
#define DMA_IRQ_INDEX 0u
#define DMA_IRQ DMA_IRQ_NUM(DMA_IRQ_INDEX)

/*
 * Three application buffers decouple the USB/main-loop producer from the
 * 1 ms I2S DMA cadence. The DMA ISR owns the active buffer; the main loop
 * owns a FILLING buffer; READY buffers are queued in FIFO order.
 *
 * If the producer is late, DMA streams a permanent zero buffer. This keeps
 * the master clock running and avoids replaying/overwriting a buffer that DMA
 * is still consuming.
 */
#define I2S_TX_BUFFER_COUNT 3u
#define I2S_TX_BUFFER_FREE 0u
#define I2S_TX_BUFFER_FILLING 1u
#define I2S_TX_BUFFER_READY 2u
#define I2S_TX_BUFFER_DMA 3u

// --- Module-level static variables ---
static uint pio_sm = 0;
static uint pio_offset = 0;
static const pio_program_t *loaded_pio_program = NULL;

static int32_t dma_buffer[I2S_TX_BUFFER_COUNT][MAX_BUFFER_FRAMES * 2u];
static int32_t silence_buffer[MAX_BUFFER_FRAMES * 2u];
static volatile uint8_t buffer_state[I2S_TX_BUFFER_COUNT];
static uint8_t ready_queue[I2S_TX_BUFFER_COUNT];
static volatile uint8_t ready_head;
static volatile uint8_t ready_tail;
static volatile uint8_t ready_count;
static volatile int8_t dma_active_buffer = -1;
static volatile int8_t cpu_filling_buffer = -1;
static volatile uint current_dma_channel;

static volatile bool dma_running = false;
static bool pio_running = false;
static bool initialized = false;

// #define TRACE_LOG LOG_DEBUG
#define TRACE_LOG(...) ((void)0)

static void tx_buffers_reset(void) {
  for (uint8_t i = 0; i < I2S_TX_BUFFER_COUNT; ++i) {
    buffer_state[i] = I2S_TX_BUFFER_FREE;
  }
  ready_head = 0;
  ready_tail = 0;
  ready_count = 0;
  dma_active_buffer = -1;
  cpu_filling_buffer = -1;
}

static void dma_irq_handler(void) {
  dma_irqn_acknowledge_channel(DMA_IRQ_INDEX, current_dma_channel);

  // A pending IRQ may arrive while the main loop is stopping the stream.
  if (!dma_running) {
    return;
  }

  // DMA has consumed every source word in the previous block. That block is
  // now safe for the application to reuse.
  if (dma_active_buffer >= 0) {
    buffer_state[(uint8_t)dma_active_buffer] = I2S_TX_BUFFER_FREE;
  }

  int8_t next_buffer = -1;
  if (ready_count != 0u) {
    next_buffer = (int8_t)ready_queue[ready_tail];
    ready_tail = (uint8_t)((ready_tail + 1u) % I2S_TX_BUFFER_COUNT);
    --ready_count;
    buffer_state[(uint8_t)next_buffer] = I2S_TX_BUFFER_DMA;
  }

  dma_active_buffer = next_buffer;
  const int32_t *next_source =
      (next_buffer >= 0) ? dma_buffer[(uint8_t)next_buffer] : silence_buffer;

  // If no prepared application block is available, stream silence instead of
  // allowing the PIO TX FIFO to empty. In master mode this also preserves the
  // shared BCLK/LRCLK waveform.
  dma_channel_set_read_addr(current_dma_channel, next_source, true);
}

static void dma_init(const i2s_config_t *config) {
  TRACE_LOG("dma_init begin\n");
  PIO pio = config->pio_instance;

  tx_buffers_reset();
  dma_running = false;

  current_dma_channel = dma_claim_unused_channel(true);
  dma_channel_config dma_config =
      dma_channel_get_default_config(current_dma_channel);
  channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
  channel_config_set_read_increment(&dma_config, true);
  channel_config_set_write_increment(&dma_config, false);
  channel_config_set_dreq(&dma_config, pio_get_dreq(pio, pio_sm, true));
  dma_channel_configure(current_dma_channel, &dma_config,
                        &pio->txf[pio_sm], NULL,
                        dma_encode_transfer_count(config->buffer_frames * 2u),
                        false);

  dma_irqn_acknowledge_channel(DMA_IRQ_INDEX, current_dma_channel);
  irq_set_exclusive_handler(DMA_IRQ, dma_irq_handler);
  irq_set_enabled(DMA_IRQ, true);
  TRACE_LOG("dma_init end\n");
}

static void dma_start(const i2s_config_t *config) {
  if (dma_running) {
    return;
  }

  TRACE_LOG("dma_start begin\n");
  uint32_t irq_state = save_and_disable_interrupts();
  tx_buffers_reset();
  dma_running = true;
  dma_irqn_acknowledge_channel(DMA_IRQ_INDEX, current_dma_channel);
  dma_irqn_set_channel_enabled(DMA_IRQ_INDEX, current_dma_channel, true);
  restore_interrupts(irq_state);

  // Begin with silence. The application queues audio blocks independently;
  // a late producer never stops the master's clock generator.
  (void)config;
  dma_channel_set_read_addr(current_dma_channel, silence_buffer, true);
  TRACE_LOG("dma_start end\n");
}

static void dma_stop(void) {
  uint32_t irq_state = save_and_disable_interrupts();
  const bool was_running = dma_running;
  dma_running = false;
  dma_irqn_set_channel_enabled(DMA_IRQ_INDEX, current_dma_channel, false);
  restore_interrupts(irq_state);

  if (was_running && dma_channel_is_busy(current_dma_channel)) {
    dma_channel_abort(current_dma_channel);
    while (dma_channel_is_busy(current_dma_channel)) {
    }
  }

  dma_irqn_acknowledge_channel(DMA_IRQ_INDEX, current_dma_channel);
  irq_state = save_and_disable_interrupts();
  tx_buffers_reset();
  restore_interrupts(irq_state);
}

static void dma_deinit(void) {
  TRACE_LOG("dma_deinit begin\n");
  dma_stop();

  irq_remove_handler(DMA_IRQ, dma_irq_handler);
  irq_set_enabled(DMA_IRQ, false);
  dma_channel_unclaim(current_dma_channel);
  TRACE_LOG("dma_deinit end\n");
}

static void pio_init(const i2s_config_t *config) {
  TRACE_LOG("pio_init begin\n");
  PIO pio = config->pio_instance;
  pio_sm = pio_claim_unused_sm(pio, true);

#if defined(PICODAC_I2S_ROLE_SLAVE)
  /* The slave only drives DATA and follows the master's external clocks. */
  loaded_pio_program = &i2s_stereo_slave_program;
  pio_offset = pio_add_program(pio, loaded_pio_program);
  i2s_slave_program_init(pio, pio_sm, pio_offset, config);
#else
  /* The master is the only board that generates BCLK and LRCLK. */
  if (config->bit_depth == 16) {
    loaded_pio_program = &i2s_stereo_16bit_program;
    pio_offset = pio_add_program(pio, loaded_pio_program);
    i2s_16bit_program_init(pio, pio_sm, pio_offset, config);
  } else if (config->bit_depth == 24) {
    loaded_pio_program = &i2s_stereo_24bit_program;
    pio_offset = pio_add_program(pio, loaded_pio_program);
    i2s_24bit_program_init(pio, pio_sm, pio_offset, config);
  } else if (config->bit_depth == 32) {
    loaded_pio_program = &i2s_stereo_32bit_program;
    pio_offset = pio_add_program(pio, loaded_pio_program);
    i2s_32bit_program_init(pio, pio_sm, pio_offset, config);
  } else {
    assert(false);
  }
#endif

  pio_sm_clear_fifos(pio, pio_sm);
  pio_running = false;
  TRACE_LOG("pio_init end\n");
}

static void pio_start(const i2s_config_t *config) {
  if (pio_running) {
    return;
  }
  TRACE_LOG("pio_start begin\n");
  pio_sm_set_enabled(config->pio_instance, pio_sm, true);
  pio_running = true;
  TRACE_LOG("pio_start end\n");
}

static void pio_stop(const i2s_config_t *config) {
  if (!pio_running) {
    return;
  }
  TRACE_LOG("pio_stop begin\n");
  pio_sm_set_enabled(config->pio_instance, pio_sm, false);
  pio_sm_clear_fifos(config->pio_instance, pio_sm);
  pio_sm_exec(config->pio_instance, pio_sm, pio_encode_jmp(pio_offset));
  pio_running = false;
  TRACE_LOG("pio_stop end\n");
}

static void pio_deinit(const i2s_config_t *config) {
  TRACE_LOG("pio_deinit begin\n");
  pio_sm_set_enabled(config->pio_instance, pio_sm, false);
  pio_running = false;

  if (loaded_pio_program) {
    pio_remove_program(config->pio_instance, loaded_pio_program, pio_offset);
    loaded_pio_program = NULL;
  }
  pio_sm_unclaim(config->pio_instance, pio_sm);
  TRACE_LOG("pio_deinit end\n");
}

void i2s_init(const i2s_config_t *config) {
  TRACE_LOG("i2s_init begin\n");
  assert(config != NULL);
  assert(config->buffer_frames > 0u);
  assert(config->buffer_frames <= MAX_BUFFER_FRAMES);

#ifdef I2S_MUTE_PIN
  gpio_init(I2S_MUTE_PIN);
  gpio_set_dir(I2S_MUTE_PIN, GPIO_OUT);
  i2s_mute();
#endif

  pio_init(config);
  dma_init(config);

  initialized = true;
  TRACE_LOG("i2s_init end\n");
}

void i2s_deinit(const i2s_config_t *config) {
  if (!initialized) {
    return;
  }
  TRACE_LOG("i2s_deinit begin\n");

  dma_deinit();
  pio_deinit(config);

  initialized = false;
  TRACE_LOG("i2s_deinit end\n");
}

void i2s_start(const i2s_config_t *config) {
  if (!initialized) {
    return;
  }
  TRACE_LOG("i2s_start begin\n");
  i2s_unmute();
  dma_start(config);
  pio_start(config);
  TRACE_LOG("i2s_start end\n");
}

void i2s_stop(const i2s_config_t *config) {
  if (!initialized || (!dma_running && !pio_running)) {
    return;
  }
  TRACE_LOG("i2s_stop begin\n");
  i2s_mute();
  dma_stop();
  pio_stop(config);
  TRACE_LOG("i2s_stop end\n");
}

void i2s_mute(void) {
#ifdef I2S_MUTE_PIN
  gpio_put(I2S_MUTE_PIN, 0);
#endif
}

void i2s_unmute(void) {
#ifdef I2S_MUTE_PIN
  gpio_put(I2S_MUTE_PIN, 1);
#endif
}

bool i2s_is_buffer_ready(void) {
  if (!initialized || !dma_running || cpu_filling_buffer >= 0) {
    return false;
  }

  uint32_t irq_state = save_and_disable_interrupts();
  bool ready = false;
  for (uint8_t i = 0; i < I2S_TX_BUFFER_COUNT; ++i) {
    if (buffer_state[i] == I2S_TX_BUFFER_FREE) {
      buffer_state[i] = I2S_TX_BUFFER_FILLING;
      cpu_filling_buffer = (int8_t)i;
      ready = true;
      break;
    }
  }
  restore_interrupts(irq_state);
  return ready;
}

int32_t *i2s_get_write_buffer(void) {
  if (cpu_filling_buffer < 0) {
    return NULL;
  }
  return dma_buffer[(uint8_t)cpu_filling_buffer];
}

void i2s_submit_write_buffer(void) {
  uint32_t irq_state = save_and_disable_interrupts();
  const int8_t index = cpu_filling_buffer;

  if (index >= 0 &&
      buffer_state[(uint8_t)index] == I2S_TX_BUFFER_FILLING &&
      ready_count < I2S_TX_BUFFER_COUNT) {
    // Publish the completed sample data before making its slot visible to ISR.
    __dmb();
    buffer_state[(uint8_t)index] = I2S_TX_BUFFER_READY;
    ready_queue[ready_head] = (uint8_t)index;
    ready_head = (uint8_t)((ready_head + 1u) % I2S_TX_BUFFER_COUNT);
    ++ready_count;
    cpu_filling_buffer = -1;
  }

  restore_interrupts(irq_state);
}

uint32_t i2s_get_buffer_size_frames(const i2s_config_t *config) {
  return config->buffer_frames;
}
