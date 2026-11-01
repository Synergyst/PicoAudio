#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Full-duplex 32-bit stereo audio transport over SPI0. */
void spi_link_init(void);
void spi_link_task(void);

void spi_link_set_local_sample_rate(uint32_t sample_rate);
uint32_t spi_link_get_local_sample_rate(void);
uint32_t spi_link_get_remote_sample_rate(void);

/* Playback samples are queued as interleaved signed S32 stereo frames. */
uint32_t spi_link_write_tx(const int32_t *samples, uint32_t frames);
uint32_t spi_link_get_tx_queued_frames(void);
uint32_t spi_link_get_tx_free_frames(void);
void spi_link_clear_tx(void);

/* Pull received interleaved S32 stereo frames. */
uint32_t spi_link_read_rx(int32_t *samples, uint32_t max_frames,
                          uint32_t *source_sample_rate);

#ifdef __cplusplus
}
#endif
