#include "audio_device.h"

#include <assert.h>
#include <string.h>

#include "blink.h"
#include "i2s.h"
#include "log.h"
#include "ringbuffer.h"

//--------------------------------------------------------------------+/
// MACRO CONSTANT TYPEDEF PROTOTYPES
//--------------------------------------------------------------------+/

// --- Configuration ---
#define I2S_DATA_PIN PICODAC_I2S_DATA_PIN
#define I2S_CLOCK_PIN_BASE PICODAC_I2S_BASE_CLOCK_PIN  // LRCLK = BASE + 1
#define SAFE_WATER_LEVEL 0.5
#define UNDERRUN_WATER_LEVEL 0.16
#define RECOVERY_WATER_LEVEL 0.4
#define PIO pio0

// 256 刻みで指定
// Volume range: -96 dB through 0 dB, in 1 dB steps.
enum {
  VOLUME_CTRL_0_DB = 0,
  VOLUME_CTRL_96_DB = 96 * 256,
};

// --- Module-level Static Variables ---
static ringbuffer_t rb;
static app_state_t g_current_state;
static i2s_config_t i2s_config;

static uint32_t current_sample_rate = 48000;
static uint8_t current_bit_depth = 16;

static float steady_buffer_fill_ratio = 0;

// Audio controls - Current states. Index 0 is master, 1 is left, 2 is right.
static int8_t mute[3] = {0, 0, 0};
static int16_t volume[3] = {VOLUME_CTRL_0_DB, VOLUME_CTRL_0_DB,
                            VOLUME_CTRL_0_DB};

// Precomputed gain table: round(10^(dB/20) * 2^31), from -96 dB to 0 dB.
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
  switch (sample_rate) {
    case 44100:
    case 48000:
      return true;
    default:
      return false;
  }
}

static uint32_t calc_buffer_size(uint32_t sample_rate) {
  // sample_rate * 2 channels * 16 ms * 4 bytes/sample.
  return sample_rate * 2u * 16u * 4u / 1000u;
}

//--------------------------------------------------------------------+/
// Initialization
//--------------------------------------------------------------------+/
void audio_device_init(void) {
  g_current_state = STATE_STOPPED;

  memset(&rb, 0, sizeof(ringbuffer_t));
  ringbuffer_init(&rb, calc_buffer_size(current_sample_rate),
                  calc_buffer_size(SAMPLE_RATES[N_SAMPLE_RATES - 1]));

  i2s_config = (i2s_config_t){
      .data_pin = I2S_DATA_PIN,
      .clock_pin_base = I2S_CLOCK_PIN_BASE,
      .pio_instance = PIO,
      .bit_depth = current_bit_depth,
      .buffer_frames = current_sample_rate / 1000,
      .sample_rate = current_sample_rate,
  };

  i2s_init(&i2s_config);
  blink_set_period_us(1000000);
}

//--------------------------------------------------------------------+/
// Main loop tasks
//--------------------------------------------------------------------+/
void audio_device_task(void) {
  switch (g_current_state) {
    case STATE_STOPPED:
      break;

    case STATE_BUFFERING:
      if (SAFE_WATER_LEVEL <= ringbuffer_fill_ratio(&rb)) {
        LOG_DEBUG("Buffer reached safe level. Starting I2S playback....");
        i2s_start(&i2s_config);
        i2s_unmute();
        g_current_state = STATE_PLAYING;
        blink_led_on();
      }
      break;

    case STATE_STALLED:
      if (RECOVERY_WATER_LEVEL <= ringbuffer_fill_ratio(&rb)) {
        LOG_DEBUG("Buffer recovered. Resuming playback.");
        g_current_state = STATE_PLAYING;
        blink_led_on();
      } else if (i2s_is_buffer_ready()) {
        int32_t *i2s_buf = i2s_get_write_buffer();
        const uint32_t frames = i2s_get_buffer_size_frames(&i2s_config);
        memset(i2s_buf, 0, frames * sizeof(int32_t) * 2u);
      }
      break;

    case STATE_PLAYING:
      if (i2s_is_buffer_ready()) {
        int32_t *i2s_buf = i2s_get_write_buffer();
        const uint32_t frames = i2s_get_buffer_size_frames(&i2s_config);
        const uint32_t bytes_to_read = frames * sizeof(int32_t) * 2u;

        const float buffer_level = steady_buffer_fill_ratio =
            ringbuffer_fill_ratio(&rb);
        if (buffer_level <= UNDERRUN_WATER_LEVEL) {
          LOG_DEBUG("Underrun! Ratio: %.2f. Entering STALLED state.",
                    buffer_level);
          g_current_state = STATE_STALLED;
          blink_set_period_us(250000);
          memset(i2s_buf, 0, frames * sizeof(int32_t) * 2u);
        } else {
          static int32_t temp_buf[96 * 2];
          assert(bytes_to_read <= sizeof(temp_buf));
          ringbuffer_read(&rb, (uint8_t *)temp_buf, bytes_to_read);

          const int16_t left_gain_idx = (volume[1] / 256) + 96;
          const int16_t right_gain_idx = (volume[2] / 256) + 96;
          const int16_t master_gain_idx = (volume[0] / 256) + 96;
          uint32_t left_gain_scaled = gain_lookup_table[left_gain_idx];
          uint32_t right_gain_scaled = gain_lookup_table[right_gain_idx];
          const uint32_t master_gain_scaled =
              gain_lookup_table[master_gain_idx];

          if (mute[0] || mute[1]) left_gain_scaled = 0;
          if (mute[0] || mute[2]) right_gain_scaled = 0;

          for (uint32_t i = 0; i < frames; ++i) {
            int64_t left = (int64_t)temp_buf[i * 2u];
            int64_t right = (int64_t)temp_buf[i * 2u + 1u];

            left = (left * left_gain_scaled) >> 31;
            left = (left * master_gain_scaled) >> 31;
            right = (right * right_gain_scaled) >> 31;
            right = (right * master_gain_scaled) >> 31;

            i2s_buf[2u * i] = (int32_t)left;
            i2s_buf[2u * i + 1u] = (int32_t)right;
          }
        }
      }
      break;
  }
}

//--------------------------------------------------------------------+/
// Data flow
//--------------------------------------------------------------------+/
void audio_device_on_usb_rx(const int32_t *buffer, uint32_t num_samples) {
  const uint32_t bytes = num_samples * sizeof(int32_t);
  const size_t written = ringbuffer_write(&rb, (void *)buffer, bytes);
  if (written != bytes) {
    LOG_DEBUG("bytes: %d, but written: %d", bytes, written);
  }

  if (g_current_state != STATE_PLAYING) {
    steady_buffer_fill_ratio = ringbuffer_fill_ratio(&rb);
  }
}

float audio_device_get_steady_buffer_fill_ratio(void) {
  return steady_buffer_fill_ratio;
}

bool audio_device_is_playing(void) {
  return g_current_state == STATE_PLAYING;
}

//--------------------------------------------------------------------+/
// Audio Stream State Control
//--------------------------------------------------------------------+/
void audio_device_stream_start(uint8_t bit_depth) {
  if (bit_depth != 16 && bit_depth != 24 && bit_depth != 32) {
    LOG_ERROR("Unsupported I2S bit depth: %u", bit_depth);
    return;
  }

  LOG_INFO("Starting stream with %d bits, %lu Hz", bit_depth,
           current_sample_rate);
  current_bit_depth = bit_depth;

  // A stream restart may arrive while a previous stream is still active.
  // Stop only if it actually reached the running/stalled state.
  audio_device_stream_stop();
  i2s_deinit(&i2s_config);

  i2s_config = (i2s_config_t){
      .data_pin = I2S_DATA_PIN,
      .clock_pin_base = I2S_CLOCK_PIN_BASE,
      .bit_depth = bit_depth,
      .pio_instance = PIO,
      .buffer_frames = current_sample_rate / 1000,
      .sample_rate = current_sample_rate,
  };
  i2s_init(&i2s_config);

  ringbuffer_resize(&rb, calc_buffer_size(current_sample_rate));
  g_current_state = STATE_BUFFERING;
  blink_set_period_us(500000);
}

void audio_device_stream_stop(void) {
  // USB hosts may issue redundant SET_INTERFACE/SET_CUR requests while the
  // stream is already stopped. Do not touch the DMA/PIO hardware in that
  // state: dma_channel_abort() is not part of a USB control-request path.
  if (g_current_state == STATE_STOPPED) {
    return;
  }

  LOG_DEBUG("Stopping stream");

  // BUFFERING has not started I2S/DMA yet. Only PLAYING and STALLED own an
  // active transmit DMA stream and need a hardware stop.
  if (g_current_state == STATE_PLAYING || g_current_state == STATE_STALLED) {
    i2s_stop(&i2s_config);
  }

  g_current_state = STATE_STOPPED;
  blink_set_period_us(1000000);
}

//--------------------------------------------------------------------+/
// Audio Feature Control
//--------------------------------------------------------------------+/
void audio_device_set_mute(uint8_t channel, bool muted) {
  if (channel >= 3) return;
  LOG_DEBUG("Set channel %d Mute: %d", channel, muted);
  mute[channel] = muted ? 1 : 0;
}

bool audio_device_get_mute(uint8_t channel) {
  if (channel >= 3) return false;
  return mute[channel] != 0;
}

void audio_device_set_volume(uint8_t channel, int16_t volume_db_256) {
  if (channel >= 3) return;
  if (volume_db_256 < -VOLUME_CTRL_96_DB) volume_db_256 = -VOLUME_CTRL_96_DB;
  if (volume_db_256 > VOLUME_CTRL_0_DB) volume_db_256 = VOLUME_CTRL_0_DB;
  LOG_DEBUG("Set channel %d volume: %d dB", channel, volume_db_256 / 256);
  volume[channel] = volume_db_256;
}

int16_t audio_device_get_volume(uint8_t channel) {
  if (channel >= 3) return VOLUME_CTRL_0_DB;
  return volume[channel];
}

void audio_device_get_volume_range(uint8_t channel, int16_t *min, int16_t *max,
                                   int16_t *res) {
  (void)channel;
  if (min) *min = -VOLUME_CTRL_96_DB;
  if (max) *max = VOLUME_CTRL_0_DB;
  if (res) *res = 256;
}

//--------------------------------------------------------------------+/
// Clock Control
//--------------------------------------------------------------------+/
void audio_device_set_sampling_freq(uint32_t freq) {
  if (!sample_rate_supported(freq)) {
    LOG_ERROR("Ignoring unsupported sample rate: %lu Hz", (unsigned long)freq);
    return;
  }

  // Linux commonly writes the current rate back during stream setup. Treat
  // that as an idempotent request; stopping the I2S/DMA engine here used to
  // stall USB EP0 even though no clock change was necessary.
  if (freq == current_sample_rate) {
    return;
  }

  // Stop an actually running playback stream before changing its clock.
  // When stopped, leave the initialized-but-idle I2S hardware alone.
  audio_device_stream_stop();
  current_sample_rate = freq;
  LOG_INFO("Clock changed to %lu Hz", (unsigned long)freq);
}

uint32_t audio_device_get_sampling_freq(void) {
  return current_sample_rate;
}

bool audio_device_is_clock_valid(void) {
  return true;
}
