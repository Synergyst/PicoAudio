#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GPIO-only, master-clocked, full-duplex S32 stereo transport. */
void gpio_audio_link_init(void);
void gpio_audio_link_task(void);

void gpio_audio_link_set_local_sample_rate(uint32_t sample_rate);
uint32_t gpio_audio_link_get_local_sample_rate(void);
uint32_t gpio_audio_link_get_remote_sample_rate(void);

/* Main-core producer -> dedicated link-core consumer. */
uint32_t gpio_audio_link_write_tx(const int32_t *samples, uint32_t frames);
uint32_t gpio_audio_link_get_tx_queued_frames(void);
uint32_t gpio_audio_link_get_tx_free_frames(void);
void gpio_audio_link_clear_tx(void);

/* Dedicated link-core producer -> main-core consumer. */
uint32_t gpio_audio_link_read_rx(int32_t *samples, uint32_t max_frames,
                                 uint32_t *source_sample_rate);

#ifdef __cplusplus
}
#endif
