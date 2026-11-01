#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hardware/pio.h"

// --- Configuration Struct ---
typedef struct {
  uint8_t data_pin;        // I2S DATA pin
  uint8_t clock_pin_base;  // BCLK pin. LRCLK will be clock_pin_base + 1
  uint8_t bit_depth;       // 16 or 24; playback is normalized to 32-bit slots
  PIO pio_instance;        // PIO instance to use (pio0 or pio1)
  uint32_t buffer_frames;  // Number of stereo frames per DMA block
  uint32_t sample_rate;    // Nominal sample rate
} i2s_config_t;

void i2s_init(const i2s_config_t *config);
void i2s_deinit(const i2s_config_t *config);
void i2s_start(const i2s_config_t *config);
void i2s_stop(const i2s_config_t *config);
void i2s_mute(void);
void i2s_unmute(void);

/*
 * Reserve one free TX block for the main-loop producer. The caller must fill
 * i2s_get_write_buffer() and then call i2s_submit_write_buffer(). A block is
 * never made visible to DMA until it has been explicitly submitted.
 */
bool i2s_is_buffer_ready(void);
int32_t *i2s_get_write_buffer(void);
void i2s_submit_write_buffer(void);
uint32_t i2s_get_buffer_size_frames(const i2s_config_t *config);
