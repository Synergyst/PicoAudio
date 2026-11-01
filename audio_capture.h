#pragma once

#include <stdint.h>

void audio_capture_init(void);
void audio_capture_task(void);
void audio_capture_stream_start(void);
void audio_capture_stream_stop(void);
uint16_t audio_capture_read_usb_packet(uint8_t *destination, uint16_t capacity);
