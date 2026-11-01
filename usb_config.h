#pragma once

#ifndef VENDOR_ID
#define VENDOR_ID 0xcafe
#endif

#ifndef PRODUCT_ID
#define PRODUCT_ID 0xbabe
#endif

enum INTERFACE_ID {
  INTERFACE_AUDIO_CONTROL = 0,
  INTERFACE_AUDIO_STREAM,          // USB playback: host -> Pico
  INTERFACE_AUDIO_STREAM_CAPTURE,  // USB capture: Pico -> host
#if HID_ENABLE
  INTERFACE_HID,
#endif
  INTERFACE_NUM,
};

/* One AudioControl interface and two AudioStreaming interfaces. */
#define AUDIO_INTERFACE_NUM 3

/* Full-speed duplex limit: 48 kHz, stereo S32, plus one frame. */
#define AUDIO_MAX_PACKET_SIZE ((48 + 1) * 4 * 2)
#define AUDIO_CAPTURE_MAX_PACKET_SIZE ((48 + 1) * 4 * 2)

#define EP_AUDIO_STREAM_OUT 0x01
#define EP_AUDIO_FEEDBACK_IN 0x81
#define EP_AUDIO_CAPTURE_IN 0x83

#define EP_HID_OUT 0x02
#define EP_HID_IN 0x82

#define AUDIO_CONTROL_ID_INPUT 0x01
#define AUDIO_CONTROL_ID_FEATURE_UNIT 0x02
#define AUDIO_CONTROL_ID_OUTPUT 0x03
#define AUDIO_CONTROL_ID_CLOCK 0x04
#define AUDIO_CONTROL_ID_CAPTURE_INPUT 0x05
#define AUDIO_CONTROL_ID_CAPTURE_OUTPUT 0x06

#define HID_INTERVAL_MS 200
