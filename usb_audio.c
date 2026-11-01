#include "usb_audio.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "audio_device.h"
#include "log.h"
#include "usb.h"
#include "usb_config.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

typedef enum {
  USB_SAMPLE_FORMAT_16,
  USB_SAMPLE_FORMAT_24,
  USB_SAMPLE_FORMAT_32,
} usb_sample_format_t;

static usb_sample_format_t g_format = USB_SAMPLE_FORMAT_16;

static void ep_audio_out_handler(const uint8_t* buf, uint16_t len) {
  /*
   * The I2S transmitter and receiver share a fixed 32-bit stereo slot.
   * USB playback formats are converted to signed, left-aligned S32 samples
   * before entering the audio ring buffer:
   *   S16_LE  -> sample << 16
   *   S24_3LE -> sample << 8
   *   S32_LE  -> unchanged
   *
   * This keeps the I2S BCLK/LRCLK framing identical for playback and capture.
   */
  static int32_t samples[512 * 2];
  const uint8_t* src = buf;
  int num_samples = 0;

  if (g_format == USB_SAMPLE_FORMAT_16) {
    num_samples = len / 2;
    for (int i = 0; i < num_samples; ++i) {
      const uint16_t raw = (uint16_t)src[i * 2] |
                           ((uint16_t)src[i * 2 + 1] << 8);
      const int32_t sample = (raw & 0x8000u)
                                 ? (int32_t)raw - 0x10000
                                 : (int32_t)raw;
      samples[i] = (int32_t)((int64_t)sample * 65536LL);
    }
  } else if (g_format == USB_SAMPLE_FORMAT_24) {
    /* UAC2 descriptor now advertises 3-byte S24_3LE, not 24-in-32 S32_LE. */
    num_samples = len / 3;
    for (int i = 0; i < num_samples; ++i) {
      const uint32_t raw = (uint32_t)src[i * 3] |
                           ((uint32_t)src[i * 3 + 1] << 8) |
                           ((uint32_t)src[i * 3 + 2] << 16);
      const int32_t sample = (raw & 0x800000u)
                                 ? (int32_t)raw - 0x1000000
                                 : (int32_t)raw;
      samples[i] = (int32_t)((int64_t)sample * 256LL);
    }
  } else if (g_format == USB_SAMPLE_FORMAT_32) {
    num_samples = len / sizeof(int32_t);
    memcpy(samples, buf, (size_t)num_samples * sizeof(int32_t));
  }

  if (num_samples > 0) {
    audio_device_on_usb_rx(samples, (uint32_t)num_samples);
  }

  /* Re-arm OUT with the endpoint's actual full-speed maximum packet size. */
  usb_ep_n_start_transfer(EP_AUDIO_STREAM_OUT, false, NULL,
                          AUDIO_MAX_PACKET_SIZE);
}

static void feedback(void) {
  static float filtered_buffer_ratio = 0.5f;
  static const float lpf_alpha = 0.01f;
  static const float feedback_rate = 0.01f;

  const uint32_t sample_rate = audio_device_get_sampling_freq();
  float adjusted_rate_per_ms;
  if (audio_device_is_playing()) {
    const float steady_buffer_fill_ratio =
        audio_device_get_steady_buffer_fill_ratio();
    filtered_buffer_ratio = steady_buffer_fill_ratio * lpf_alpha +
                           filtered_buffer_ratio * (1.0f - lpf_alpha);
    const float error = filtered_buffer_ratio - 0.5f;
    adjusted_rate_per_ms =
        (sample_rate / 1000.0f) * (1.0f - error * feedback_rate);
  } else {
    adjusted_rate_per_ms = sample_rate / 1000.0f;
    filtered_buffer_ratio = 0.5f;
  }

  const uint32_t feedback_value =
      (uint32_t)(adjusted_rate_per_ms * (1u << 16));
  usb_ep_n_start_transfer(EP_AUDIO_FEEDBACK_IN & 0x7F, true,
                          (const uint8_t*)&feedback_value,
                          sizeof(feedback_value));
}

static void ep_audio_in_handler(void) { feedback(); }

bool usb_audio_control_set_interface(uint8_t alt) { return alt == 0; }

static uint8_t audio_stream_current_alt = 0;
bool usb_audio_stream_set_interface(uint8_t alt) {
  LOG_INFO("Set interface AUDIO_STREAM alt %d\r", alt);
  if (3 < alt) {
    LOG_ERROR("unknown alt: %d", alt);
    return false;
  }

  audio_stream_current_alt = alt;
  audio_device_stream_stop();

  if (alt == 1) {
    g_format = USB_SAMPLE_FORMAT_16;
  } else if (alt == 2) {
    g_format = USB_SAMPLE_FORMAT_24;
  } else if (alt == 3) {
    g_format = USB_SAMPLE_FORMAT_32;
  }

  /* Playback and capture use the same fixed 32-bit I2S slot width. */
  if (alt != 0) {
    audio_device_stream_start(32);
  }

  if (alt != 0) {
    feedback();
  }
  return true;
}

#define UAC2_CS_REQ_CUR 0x01
#define UAC2_CS_REQ_RANGE 0x02
#define UAC2_FU_MUTE_CONTROL 0x01
#define UAC2_FU_VOLUME_CONTROL 0x02
#define UAC2_CS_SAM_FREQ_CONTROL 0x01

bool usb_audio_control_in_request(const struct usb_setup_packet_t* pkt) {
  const uint8_t itf = pkt->wIndex & 0xFF;
  if (itf != INTERFACE_AUDIO_CONTROL) {
    LOG_ERROR("unhandled class in request for iff: %d", itf);
    return false;
  }

  if (pkt->bRequest == UAC2_CS_REQ_CUR) {
    if ((pkt->wIndex >> 8) == AUDIO_CONTROL_ID_CLOCK &&
        (pkt->wValue >> 8) == UAC2_CS_SAM_FREQ_CONTROL &&
        (pkt->wValue & 0xFF) == 0) {
      static uint8_t response[4];
      const uint32_t freq = audio_device_get_sampling_freq();
      response[0] = (uint8_t)freq;
      response[1] = (uint8_t)(freq >> 8);
      response[2] = (uint8_t)(freq >> 16);
      response[3] = (uint8_t)(freq >> 24);
      usb_ep0_start_transfer(response,
                             MIN(pkt->wLength, sizeof(response)));
      return true;
    } else if ((pkt->wIndex >> 8) == AUDIO_CONTROL_ID_FEATURE_UNIT &&
               (pkt->wValue >> 8) == UAC2_FU_VOLUME_CONTROL) {
      static int16_t vol;
      const uint8_t ch = pkt->wValue & 0xFF;
      vol = audio_device_get_volume(ch);
      usb_ep0_start_transfer((const uint8_t*)&vol,
                             MIN(pkt->wLength, sizeof(vol)));
      return true;
    } else if ((pkt->wIndex >> 8) == AUDIO_CONTROL_ID_FEATURE_UNIT &&
               (pkt->wValue >> 8) == UAC2_FU_MUTE_CONTROL) {
      static uint8_t mute;
      const uint8_t ch = pkt->wValue & 0xFF;
      mute = audio_device_get_mute(ch);
      usb_ep0_start_transfer(&mute, MIN(pkt->wLength, sizeof(mute)));
      return true;
    }
  } else if (pkt->bRequest == UAC2_CS_REQ_RANGE) {
    if ((pkt->wIndex >> 8) == AUDIO_CONTROL_ID_FEATURE_UNIT &&
        (pkt->wValue >> 8) == UAC2_FU_VOLUME_CONTROL) {
      int16_t min, max, res;
      const uint8_t ch = pkt->wValue & 0xFF;
      audio_device_get_volume_range(ch, &min, &max, &res);

      struct range2b {
        uint16_t wNumSubRanges;
        struct {
          int16_t dMIN;
          int16_t dMAX;
          int16_t dRES;
        } subranges[1];
      } __attribute__((packed));

      static struct range2b ret;
      ret.wNumSubRanges = 1;
      ret.subranges[0].dMIN = min;
      ret.subranges[0].dMAX = max;
      ret.subranges[0].dRES = res;
      usb_ep0_start_transfer((const uint8_t*)&ret,
                             MIN(pkt->wLength, sizeof(ret)));
      return true;
    } else if ((pkt->wIndex >> 8) == AUDIO_CONTROL_ID_CLOCK &&
               (pkt->wValue >> 8) == UAC2_CS_SAM_FREQ_CONTROL) {
      /* 44.1/48 kHz only: sized for simultaneous full-speed duplex. */
      struct range4b {
        uint16_t wNumSubRanges;
        struct {
          uint32_t dMIN;
          uint32_t dMAX;
          uint32_t dRES;
        } subranges[2];
      } __attribute__((packed));

      static const struct range4b ret = {
          .wNumSubRanges = 2,
          .subranges = {
              {.dMIN = 44100, .dMAX = 44100, .dRES = 0},
              {.dMIN = 48000, .dMAX = 48000, .dRES = 0},
          },
      };
      usb_ep0_start_transfer((const uint8_t*)&ret,
                             MIN(pkt->wLength, sizeof(ret)));
      return true;
    }
  }

  LOG_INFO("unhandled class in request: unknown request");
  return false;
}

bool usb_audio_control_ouot_request(const struct usb_setup_packet_t* pkt,
                                    const uint8_t* buf, uint16_t len) {
  if (pkt->bmRequestType == 0x21 && pkt->bRequest == UAC2_CS_REQ_CUR &&
      (pkt->wValue >> 8) == UAC2_CS_SAM_FREQ_CONTROL &&
      (pkt->wIndex >> 8) == AUDIO_CONTROL_ID_CLOCK &&
      (pkt->wIndex & 0xFF) == INTERFACE_AUDIO_CONTROL) {
    assert(pkt->wLength == 4);
    assert(len == 4);
    const uint32_t freq = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                          ((uint32_t)buf[2] << 16) |
                          ((uint32_t)buf[3] << 24);
    audio_device_set_sampling_freq(freq);
    return true;
  } else if (pkt->bmRequestType == 0x21 &&
             pkt->bRequest == UAC2_CS_REQ_CUR &&
             (pkt->wValue >> 8) == UAC2_FU_MUTE_CONTROL &&
             (pkt->wIndex >> 8) == AUDIO_CONTROL_ID_FEATURE_UNIT &&
             (pkt->wIndex & 0xFF) == INTERFACE_AUDIO_CONTROL) {
    assert(pkt->wLength == 1);
    assert(len == 1);
    const uint8_t ch = pkt->wValue & 0xFF;
    audio_device_set_mute(ch, buf[0]);
    LOG_DEBUG("set %s, ch(%d)", buf[0] ? "mute" : "unmute", ch);
    return true;
  } else if (pkt->bmRequestType == 0x21 &&
             pkt->bRequest == UAC2_CS_REQ_CUR &&
             (pkt->wValue >> 8) == UAC2_FU_VOLUME_CONTROL &&
             (pkt->wIndex >> 8) == AUDIO_CONTROL_ID_FEATURE_UNIT &&
             (pkt->wIndex & 0xFF) == INTERFACE_AUDIO_CONTROL) {
    assert(pkt->wLength == 2);
    assert(len == 2);
    const int16_t vol = (int16_t)((uint16_t)buf[0] |
                                  ((uint16_t)buf[1] << 8));
    const uint8_t ch = pkt->wValue & 0xFF;
    audio_device_set_volume(ch, vol);
    LOG_DEBUG("set volume %d dB, ch(%d)", vol / 256, ch);
    return true;
  }

  LOG_INFO("unhandled OUT request(type=0x%02x, request=0x%02x, value=0x%04x, index=0x%04x, length=0x%04x)",
           pkt->bmRequestType, pkt->bRequest, pkt->wValue, pkt->wIndex,
           pkt->wLength);
  return false;
}

void usb_audio_init(void) {
  usb_device_set_ep_out_handler(EP_AUDIO_STREAM_OUT, ep_audio_out_handler);
  usb_device_set_ep_in_handler(EP_AUDIO_FEEDBACK_IN & 0x7F,
                               ep_audio_in_handler);

  usb_device_set_control_in_handler(INTERFACE_AUDIO_CONTROL,
                                    usb_audio_control_in_request);
  usb_device_set_control_out_handler(INTERFACE_AUDIO_CONTROL,
                                     usb_audio_control_ouot_request);

  usb_device_set_set_interface_handler(INTERFACE_AUDIO_CONTROL,
                                       usb_audio_control_set_interface);
  usb_device_set_set_interface_handler(INTERFACE_AUDIO_STREAM,
                                       usb_audio_stream_set_interface);
}
