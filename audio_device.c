#include "audio_device.h"

#include <assert.h>
#include <string.h>

#include "blink.h"
#include "log.h"
#include "ringbuffer.h"
#include "gpio_audio_link.h"

#define SAFE_WATER_LEVEL 0.5f
#define UNDERRUN_WATER_LEVEL 0.16f
#define RECOVERY_WATER_LEVEL 0.4f
#define LINK_TX_TARGET_MS 8u
#define LINK_TX_LOW_WATER_MS 2u
#define LINK_TX_MAX_MOVE_FRAMES 192u
#define STEREO_FRAME_BYTES (sizeof(int32_t) * 2u)

enum {
  VOLUME_CTRL_0_DB = 0,
  VOLUME_CTRL_96_DB = 96 * 256,
};

static ringbuffer_t rb;
static app_state_t g_current_state;
static uint32_t current_sample_rate = 48000u;
static uint8_t current_bit_depth = 16u;
static float steady_buffer_fill_ratio;

static int8_t mute[3] = {0, 0, 0};
static int16_t volume[3] = {VOLUME_CTRL_0_DB, VOLUME_CTRL_0_DB,
                            VOLUME_CTRL_0_DB};

/* round(10^(dB/20) * 2^31), from -96 dB through 0 dB in 1 dB steps. */
static const uint32_t gain_lookup_table[97] = {
    0x000084f3, 0x0000952c, 0x0000a760, 0x0000bbcc, 0x0000d2b6, 0x0000ec6c,
    0x00010945, 0x000129a4, 0x00014df5, 0x000176b5, 0x0001a46d, 0x0001d7ba,
    0x00021149, 0x000251de, 0x00029a55, 0x0002eba3, 0x000346dc, 0x0003ad38,
    0x00042010, 0x0004a0ec, 0x00053181, 0x0005d3bb, 0x000689bf, 0x000755fa,
    0x00083b20, 0x00093c3b, 0x000a5cb6, 0x000ba064, 0x000d0b91, 0x000ea30e,
    0x00106c43, 0x00126d43, 0x0014acdb, 0x001732ae, 0x001a074f, 0x001d345b,
    0x0020c49c, 0x0024c42c, 0x002940a2, 0x002e4939, 0x0033ef0c, 0x003a454a,
    0x00416179, 0x00495bc1, 0x00524f3b, 0x005c5a4f, 0x00679f1c, 0x007443e8,
    0x008273a6, 0x00925e89, 0x00a43aa2, 0x00b8449c, 0x00cec08a, 0x00e7facc,
    0x01044915, 0x01240b8c, 0x0147ae14, 0x016fa9bb, 0x019c8651, 0x01cedc3d,
    0x0207567a, 0x0246b4e4, 0x028dcebc, 0x02dd958a, 0x0337184e, 0x039b8719,
    0x040c3714, 0x048aa70b, 0x05188480, 0x05b7b15b, 0x066a4a53, 0x0732ae18,
    0x08138562, 0x090fcbf8, 0x0a2adad2, 0x0b68737a, 0x0ccccccd, 0x0e5ca14c,
    0x101d3f2e, 0x12149a60, 0x144960c5, 0x16c310e3, 0x198a1357, 0x1ca7d768,
    0x2026f310, 0x241346f6, 0x287a26c5, 0x2d6a866f, 0x32f52cff, 0x392ced8e,
    0x4026e73d, 0x47faccf0, 0x50c335d4, 0x5a9df7ac, 0x65ac8c2f, 0x721482c0,
    0x80000000,
};

static bool sample_rate_supported(uint32_t sample_rate) {
  return sample_rate == 44100u || sample_rate == 48000u;
}

static uint32_t calc_buffer_size(uint32_t sample_rate) {
  /* 16 ms, rounded up to a whole interleaved stereo S32 frame. */
  const uint32_t frames = (sample_rate * 16u + 999u) / 1000u;
  return frames * (uint32_t)STEREO_FRAME_BYTES;
}

static uint32_t playback_ring_frames(void) {
  if (rb.size == 0) return 0;
  return (uint32_t)(ringbuffer_fill_ratio(&rb) * (float)rb.size) /
         (uint32_t)STEREO_FRAME_BYTES;
}

static void update_steady_fill_ratio(void) {
  const uint32_t buffered_frames =
      playback_ring_frames() + gpio_audio_link_get_tx_queued_frames();
  const uint32_t nominal_capacity_frames =
      (current_sample_rate * 24u) / 1000u; /* 16 ms local + 8 ms GPIO-link queue */
  if (nominal_capacity_frames == 0) {
    steady_buffer_fill_ratio = 0.0f;
  } else {
    steady_buffer_fill_ratio =
        (float)buffered_frames / (float)nominal_capacity_frames;
    if (steady_buffer_fill_ratio > 1.0f) steady_buffer_fill_ratio = 1.0f;
  }
}

static void apply_output_gain(int32_t *samples, uint32_t frames) {
  const int16_t left_gain_idx = (volume[1] / 256) + 96;
  const int16_t right_gain_idx = (volume[2] / 256) + 96;
  const int16_t master_gain_idx = (volume[0] / 256) + 96;
  uint32_t left_gain_scaled = gain_lookup_table[left_gain_idx];
  uint32_t right_gain_scaled = gain_lookup_table[right_gain_idx];
  const uint32_t master_gain_scaled = gain_lookup_table[master_gain_idx];

  if (mute[0] || mute[1]) left_gain_scaled = 0;
  if (mute[0] || mute[2]) right_gain_scaled = 0;

  for (uint32_t i = 0; i < frames; ++i) {
    int64_t left = samples[i * 2u];
    int64_t right = samples[i * 2u + 1u];

    left = (left * left_gain_scaled) >> 31;
    left = (left * master_gain_scaled) >> 31;
    right = (right * right_gain_scaled) >> 31;
    right = (right * master_gain_scaled) >> 31;

    samples[i * 2u] = (int32_t)left;
    samples[i * 2u + 1u] = (int32_t)right;
  }
}

void audio_device_init(void) {
  g_current_state = STATE_STOPPED;
  memset(&rb, 0, sizeof(rb));
  ringbuffer_init(&rb, calc_buffer_size(current_sample_rate),
                  calc_buffer_size(SAMPLE_RATES[N_SAMPLE_RATES - 1]));
  gpio_audio_link_set_local_sample_rate(current_sample_rate);
  steady_buffer_fill_ratio = 0.0f;
  blink_set_period_us(1000000);
}

void audio_device_task(void) {
  switch (g_current_state) {
    case STATE_STOPPED:
      break;

    case STATE_BUFFERING:
      if (SAFE_WATER_LEVEL <= ringbuffer_fill_ratio(&rb)) {
        LOG_INFO("Playback prebuffer ready; feeding the GPIO-link audio queue");
        g_current_state = STATE_PLAYING;
        blink_led_on();
      }
      break;

    case STATE_STALLED:
      if (RECOVERY_WATER_LEVEL <= ringbuffer_fill_ratio(&rb)) {
        LOG_INFO("Playback queue recovered");
        g_current_state = STATE_PLAYING;
        blink_led_on();
      }
      break;

    case STATE_PLAYING:
      break;
  }

  if (g_current_state == STATE_PLAYING) {
    const uint32_t target_frames =
        (current_sample_rate * LINK_TX_TARGET_MS) / 1000u;

    while (gpio_audio_link_get_tx_queued_frames() < target_frames) {
      uint32_t queued = gpio_audio_link_get_tx_queued_frames();
      uint32_t free_frames = gpio_audio_link_get_tx_free_frames();
      uint32_t available_frames = playback_ring_frames();
      if (free_frames == 0 || available_frames == 0) break;

      uint32_t wanted = target_frames - queued;
      if (wanted > LINK_TX_MAX_MOVE_FRAMES) wanted = LINK_TX_MAX_MOVE_FRAMES;
      if (wanted > free_frames) wanted = free_frames;
      if (wanted > available_frames) wanted = available_frames;
      if (wanted == 0) break;

      static int32_t temp_samples[LINK_TX_MAX_MOVE_FRAMES * 2u];
      const uint32_t bytes = wanted * (uint32_t)STEREO_FRAME_BYTES;
      const size_t read = ringbuffer_read(&rb, (uint8_t *)temp_samples, bytes);
      const uint32_t frames_read = (uint32_t)(read / STEREO_FRAME_BYTES);
      if (frames_read == 0) break;

      apply_output_gain(temp_samples, frames_read);
      if (gpio_audio_link_write_tx(temp_samples, frames_read) != frames_read) {
        /* The transport queue is single-producer/single-consumer on core 0. */
        LOG_ERROR("GPIO-link playback queue accepted fewer frames than requested");
      }
    }

    if (ringbuffer_fill_ratio(&rb) <= UNDERRUN_WATER_LEVEL &&
        gpio_audio_link_get_tx_queued_frames() <=
            (current_sample_rate * LINK_TX_LOW_WATER_MS) / 1000u) {
      LOG_WARN("Playback underrun; GPIO link will transmit silence until recovery");
      g_current_state = STATE_STALLED;
      blink_set_period_us(250000);
    }
  }

  update_steady_fill_ratio();
}

void audio_device_on_usb_rx(const int32_t *buffer, uint32_t num_samples) {
  if (!buffer || num_samples == 0) return;
  const uint32_t bytes = num_samples * (uint32_t)sizeof(int32_t);
  const size_t written = ringbuffer_write(&rb, (const uint8_t *)buffer, bytes);
  if (written != bytes) {
    LOG_DEBUG("Playback ring accepted %u of %u bytes", (unsigned)written,
              (unsigned)bytes);
  }
  update_steady_fill_ratio();
}

float audio_device_get_steady_buffer_fill_ratio(void) {
  update_steady_fill_ratio();
  return steady_buffer_fill_ratio;
}

bool audio_device_is_playing(void) {
  return g_current_state == STATE_PLAYING;
}

void audio_device_stream_start(uint8_t bit_depth) {
  if (bit_depth != 16 && bit_depth != 24 && bit_depth != 32) {
    LOG_ERROR("Unsupported USB playback bit depth: %u", bit_depth);
    return;
  }

  audio_device_stream_stop();
  current_bit_depth = bit_depth;
  gpio_audio_link_clear_tx();
  ringbuffer_clear(&rb);
  ringbuffer_resize(&rb, calc_buffer_size(current_sample_rate));
  gpio_audio_link_set_local_sample_rate(current_sample_rate);
  g_current_state = STATE_BUFFERING;
  blink_set_period_us(500000);
  LOG_INFO("Starting USB %u-bit playback at %lu Hz (GPIO bitbang link carries S32 stereo)",
           current_bit_depth, (unsigned long)current_sample_rate);
}

void audio_device_stream_stop(void) {
  if (g_current_state != STATE_STOPPED) {
    LOG_DEBUG("Stopping USB playback stream");
  }
  g_current_state = STATE_STOPPED;
  ringbuffer_clear(&rb);
  gpio_audio_link_clear_tx();
  steady_buffer_fill_ratio = 0.0f;
  blink_set_period_us(1000000);
}

void audio_device_set_mute(uint8_t channel, bool muted) {
  if (channel >= 3) return;
  LOG_DEBUG("Set channel %u mute: %u", channel, muted ? 1u : 0u);
  mute[channel] = muted ? 1 : 0;
}

bool audio_device_get_mute(uint8_t channel) {
  if (channel >= 3) return false;
  return mute[channel] != 0;
}

void audio_device_set_volume(uint8_t channel, int16_t volume_db_256) {
  if (channel >= 3) return;
  if (volume_db_256 < -VOLUME_CTRL_96_DB)
    volume_db_256 = -VOLUME_CTRL_96_DB;
  if (volume_db_256 > VOLUME_CTRL_0_DB)
    volume_db_256 = VOLUME_CTRL_0_DB;
  LOG_DEBUG("Set channel %u volume: %d dB", channel, volume_db_256 / 256);
  volume[channel] = volume_db_256;
}

int16_t audio_device_get_volume(uint8_t channel) {
  if (channel >= 3) return VOLUME_CTRL_0_DB;
  return volume[channel];
}

void audio_device_get_volume_range(uint8_t channel, int16_t *min,
                                   int16_t *max, int16_t *res) {
  (void)channel;
  if (min) *min = -VOLUME_CTRL_96_DB;
  if (max) *max = VOLUME_CTRL_0_DB;
  if (res) *res = 256;
}

void audio_device_set_sampling_freq(uint32_t freq) {
  if (!sample_rate_supported(freq)) {
    LOG_ERROR("Ignoring unsupported sample rate: %lu Hz", (unsigned long)freq);
    return;
  }
  if (freq == current_sample_rate) return;

  audio_device_stream_stop();
  current_sample_rate = freq;
  gpio_audio_link_set_local_sample_rate(freq);
  ringbuffer_resize(&rb, calc_buffer_size(current_sample_rate));
  LOG_INFO("USB audio clock changed to %lu Hz", (unsigned long)freq);
}

uint32_t audio_device_get_sampling_freq(void) {
  return current_sample_rate;
}

bool audio_device_is_clock_valid(void) {
  return true;
}
