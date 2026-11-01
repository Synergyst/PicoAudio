#include "usb_audio.h"

#include <stdbool.h>
#include <stdint.h>

#include "audio_capture.h"
#include "usb.h"
#include "usb_config.h"

#define UAC2_CS_REQ_CUR 0x01
#define UAC2_CS_REQ_RANGE 0x02
#define UAC2_CS_SAM_FREQ_CONTROL 0x01
#define CAPTURE_SAMPLE_RATE 48000u
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static bool capture_stream_active;
static uint8_t capture_packet[AUDIO_CAPTURE_MAX_PACKET_SIZE] __attribute__((aligned(4)));

static void capture_submit_packet(void) {
    if (!capture_stream_active) {
        return;
    }

    const uint16_t length = audio_capture_read_usb_packet(
        capture_packet, sizeof(capture_packet));
    if (length != 0) {
        usb_ep_n_start_transfer(EP_AUDIO_CAPTURE_IN & 0x7f, true,
                                capture_packet, length);
    }
}

static void ep_audio_capture_in_handler(void) {
    capture_submit_packet();
}

static bool usb_audio_control_set_interface(uint8_t alt) {
    return alt == 0;
}

static bool usb_audio_capture_set_interface(uint8_t alt) {
    if (alt == 0) {
        capture_stream_active = false;
        audio_capture_stream_stop();
        return true;
    }

    if (alt != 1) {
        return false;
    }

    audio_capture_stream_stop();
    audio_capture_stream_start();
    capture_stream_active = true;
    capture_submit_packet();
    return true;
}

static bool usb_audio_control_in_request(const struct usb_setup_packet_t *pkt) {
    const uint8_t interface_number = (uint8_t)(pkt->wIndex & 0xffu);
    const uint8_t entity_id = (uint8_t)(pkt->wIndex >> 8);
    const uint8_t control_selector = (uint8_t)(pkt->wValue >> 8);

    if (interface_number != INTERFACE_AUDIO_CONTROL ||
        entity_id != AUDIO_CONTROL_ID_CLOCK ||
        control_selector != UAC2_CS_SAM_FREQ_CONTROL) {
        return false;
    }

    if (pkt->bRequest == UAC2_CS_REQ_CUR) {
        static const uint32_t current_frequency = CAPTURE_SAMPLE_RATE;
        usb_ep0_start_transfer((const uint8_t *)&current_frequency,
                               MIN(pkt->wLength, sizeof(current_frequency)));
        return true;
    }

    if (pkt->bRequest == UAC2_CS_REQ_RANGE) {
        struct clock_range {
            uint16_t number_of_subranges;
            struct {
                uint32_t minimum;
                uint32_t maximum;
                uint32_t resolution;
            } __attribute__((packed)) subrange;
        } __attribute__((packed));

        static const struct clock_range range = {
            .number_of_subranges = 1,
            .subrange = {
                .minimum = CAPTURE_SAMPLE_RATE,
                .maximum = CAPTURE_SAMPLE_RATE,
                .resolution = 0,
            },
        };

        usb_ep0_start_transfer((const uint8_t *)&range,
                               MIN(pkt->wLength, sizeof(range)));
        return true;
    }

    return false;
}

void usb_audio_init(void) {
    usb_device_set_ep_in_handler(EP_AUDIO_CAPTURE_IN & 0x7f,
                                 ep_audio_capture_in_handler);

    usb_device_set_control_in_handler(INTERFACE_AUDIO_CONTROL,
                                      usb_audio_control_in_request);
    usb_device_set_set_interface_handler(INTERFACE_AUDIO_CONTROL,
                                         usb_audio_control_set_interface);
    usb_device_set_set_interface_handler(INTERFACE_AUDIO_STREAM,
                                         usb_audio_capture_set_interface);
}
