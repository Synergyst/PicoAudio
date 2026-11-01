#include "i2s_rx.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "i2s_rx.pio.h"

#define I2S_RX_PIO pio0
#define I2S_RX_BCLK_PIN 20u
#define I2S_RX_LRCLK_PIN 21u

/* The existing DAC transmitter uses DMA_IRQ_0. Keep RX on DMA_IRQ_1. */
#define I2S_RX_DMA_IRQ_INDEX 1u
#define I2S_RX_DMA_WORDS (I2S_RX_BUFFER_FRAMES * 2u)

static uint rx_sm;
static uint rx_offset;
static uint rx_dma_channel;
static uint8_t rx_active_index;
static volatile uint8_t rx_completed_index;
static volatile bool rx_buffer_ready;
static volatile bool rx_initialized;
static volatile bool rx_running;
static int32_t rx_buffers[2][I2S_RX_DMA_WORDS];

static void i2s_rx_dma_irq_handler(void) {
  dma_irqn_acknowledge_channel(I2S_RX_DMA_IRQ_INDEX, rx_dma_channel);

  /* A completion may already be pending when the USB capture stream stops. */
  if (!rx_running) {
    return;
  }

  const uint8_t completed = rx_active_index;
  const uint8_t next = completed ^ 1u;
  rx_active_index = next;

  dma_channel_set_write_addr(rx_dma_channel, rx_buffers[next], false);
  dma_channel_set_trans_count(rx_dma_channel, I2S_RX_DMA_WORDS, true);

  const uint32_t irq_state = save_and_disable_interrupts();
  rx_completed_index = completed;
  rx_buffer_ready = true;
  restore_interrupts(irq_state);
}

bool i2s_rx_init(uint8_t data_pin) {
  if (rx_initialized) {
    return true;
  }

  if (data_pin >= NUM_BANK0_GPIOS || I2S_RX_BCLK_PIN >= NUM_BANK0_GPIOS ||
      I2S_RX_LRCLK_PIN >= NUM_BANK0_GPIOS || data_pin == I2S_RX_BCLK_PIN ||
      data_pin == I2S_RX_LRCLK_PIN) {
    return false;
  }

  /* BCLK/LRCLK are already configured as outputs by the DAC I2S PIO
     transmitter. Do not change their direction here: RX samples the same
     clock pins while the DAC transmitter drives them. */
  pio_gpio_init(I2S_RX_PIO, data_pin);
  gpio_set_input_enabled(data_pin, true);
  gpio_disable_pulls(data_pin);

  /* PIO WAIT instructions observe the physical clock pin levels. The pins
     are shared with the DAC PIO state machine and remain under its control. */
  gpio_set_input_enabled(I2S_RX_BCLK_PIN, true);
  gpio_set_input_enabled(I2S_RX_LRCLK_PIN, true);

  rx_sm = pio_claim_unused_sm(I2S_RX_PIO, true);
  rx_offset = pio_add_program(I2S_RX_PIO, &i2s_rx_program);

  pio_sm_config sm_config = i2s_rx_program_get_default_config(rx_offset);
  sm_config_set_in_pins(&sm_config, data_pin);
  sm_config_set_in_shift(&sm_config, false, true, 32);
  sm_config_set_fifo_join(&sm_config, PIO_FIFO_JOIN_RX);
  sm_config_set_clkdiv(&sm_config, 1.0f);

  pio_sm_init(I2S_RX_PIO, rx_sm, rx_offset, &sm_config);
  pio_sm_set_consecutive_pindirs(I2S_RX_PIO, rx_sm, data_pin, 1, false);
  pio_sm_set_enabled(I2S_RX_PIO, rx_sm, false);

  rx_dma_channel = dma_claim_unused_channel(true);
  dma_channel_config dma_config = dma_channel_get_default_config(rx_dma_channel);
  channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
  channel_config_set_read_increment(&dma_config, false);
  channel_config_set_write_increment(&dma_config, true);
  channel_config_set_dreq(&dma_config,
                          pio_get_dreq(I2S_RX_PIO, rx_sm, false));

  dma_channel_configure(rx_dma_channel, &dma_config, rx_buffers[0],
                        &I2S_RX_PIO->rxf[rx_sm], I2S_RX_DMA_WORDS, false);

  dma_irqn_acknowledge_channel(I2S_RX_DMA_IRQ_INDEX, rx_dma_channel);
  irq_set_exclusive_handler(DMA_IRQ_NUM(I2S_RX_DMA_IRQ_INDEX),
                            i2s_rx_dma_irq_handler);
  irq_set_enabled(DMA_IRQ_NUM(I2S_RX_DMA_IRQ_INDEX), true);
  dma_irqn_set_channel_enabled(I2S_RX_DMA_IRQ_INDEX, rx_dma_channel, true);

  rx_buffer_ready = false;
  rx_active_index = 0;
  rx_initialized = true;
  return true;
}

void i2s_rx_start(void) {
  if (!rx_initialized || rx_running) {
    return;
  }

  pio_sm_set_enabled(I2S_RX_PIO, rx_sm, false);
  pio_sm_clear_fifos(I2S_RX_PIO, rx_sm);
  pio_sm_exec(I2S_RX_PIO, rx_sm, pio_encode_jmp(rx_offset));

  rx_buffer_ready = false;
  rx_active_index = 0;
  rx_running = true;
  dma_irqn_acknowledge_channel(I2S_RX_DMA_IRQ_INDEX, rx_dma_channel);
  dma_irqn_set_channel_enabled(I2S_RX_DMA_IRQ_INDEX, rx_dma_channel, true);
  dma_channel_set_write_addr(rx_dma_channel, rx_buffers[0], false);
  dma_channel_set_trans_count(rx_dma_channel, I2S_RX_DMA_WORDS, true);
  pio_sm_set_enabled(I2S_RX_PIO, rx_sm, true);
}

void i2s_rx_stop(void) {
  if (!rx_initialized || !rx_running) {
    return;
  }

  /* Stop re-arming first; a pending DMA IRQ must not restart the channel. */
  rx_running = false;
  dma_irqn_set_channel_enabled(I2S_RX_DMA_IRQ_INDEX, rx_dma_channel, false);
  pio_sm_set_enabled(I2S_RX_PIO, rx_sm, false);

  if (dma_channel_is_busy(rx_dma_channel)) {
    dma_channel_abort(rx_dma_channel);
    while (dma_channel_is_busy(rx_dma_channel)) {
    }
  }

  dma_irqn_acknowledge_channel(I2S_RX_DMA_IRQ_INDEX, rx_dma_channel);
  pio_sm_clear_fifos(I2S_RX_PIO, rx_sm);
  rx_buffer_ready = false;
}

bool i2s_rx_take_buffer(const int32_t **stereo_samples) {
  if (!stereo_samples || !rx_running) {
    return false;
  }

  const uint32_t irq_state = save_and_disable_interrupts();
  const bool ready = rx_buffer_ready;
  if (ready) {
    *stereo_samples = rx_buffers[rx_completed_index];
    rx_buffer_ready = false;
  }
  restore_interrupts(irq_state);
  return ready;
}
