#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stage-1 GPIO diagnostic interface.
 *
 * gpio_audio_link_init() blocks until the peer has passed the startup probe
 * and the C-only, slow-clock, full-duplex challenge/response test.
 *
 * PCM forwarding is intentionally disabled in this diagnostic build. The
 * functions below remain as compatibility stubs for the existing UAC2 code.
 */
void gpio_audio_link_init(void);
void gpio_audio_link_task(void);

void gpio_audio_link_set_local_sample_rate(uint32_t sample_rate);
uint32_t gpio_audio_link_get_local_sample_rate(void);
uint32_t gpio_audio_link_get_remote_sample_rate(void);

uint32_t gpio_audio_link_write_tx(const int32_t *samples, uint32_t frames);
uint32_t gpio_audio_link_get_tx_queued_frames(void);
uint32_t gpio_audio_link_get_tx_free_frames(void);
void gpio_audio_link_clear_tx(void);

uint32_t gpio_audio_link_read_rx(int32_t *samples, uint32_t max_frames,
                                 uint32_t *source_sample_rate);

#ifdef __cplusplus
}
#endif
