#pragma once

#include <stdbool.h>
#include <stdint.h>

#define I2S_RX_BUFFER_FRAMES 48u

bool i2s_rx_init(uint8_t data_pin);
void i2s_rx_start(void);
void i2s_rx_stop(void);
bool i2s_rx_take_buffer(const int32_t **stereo_samples);
