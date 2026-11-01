#ifndef _AUDIO_DEVICE_H_
#define _AUDIO_DEVICE_H_

#include <stdbool.h>
#include <stdint.h>

/* Rates supported by the full-speed simultaneous playback/capture profile. */
static const uint32_t SAMPLE_RATES[] = {44100, 48000};
#define N_SAMPLE_RATES (sizeof(SAMPLE_RATES) / sizeof(SAMPLE_RATES[0]))

/* Audio device state */
typedef enum {
  STATE_STOPPED,
  STATE_BUFFERING,
  STATE_PLAYING,
  STATE_STALLED
} app_state_t;

void audio_device_init(void);
void audio_device_task(void);
void audio_device_on_usb_rx(const int32_t *buffer, uint32_t num_samples);
float audio_device_get_steady_buffer_fill_ratio(void);
bool audio_device_is_playing(void);

void audio_device_stream_start(uint8_t bit_depth);
void audio_device_stream_stop(void);

void audio_device_set_mute(uint8_t channel, bool muted);
bool audio_device_get_mute(uint8_t channel);
void audio_device_set_volume(uint8_t channel, int16_t volume_db_256);
int16_t audio_device_get_volume(uint8_t channel);
void audio_device_get_volume_range(uint8_t channel, int16_t *min, int16_t *max,
                                   int16_t *res);

void audio_device_set_sampling_freq(uint32_t freq);
uint32_t audio_device_get_sampling_freq(void);
bool audio_device_is_clock_valid(void);

#endif
