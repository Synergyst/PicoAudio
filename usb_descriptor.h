#pragma once

#include "usb_common.h"
#include "usb_config.h"

#if HID_ENABLE
static const uint8_t report_descriptor[] = {
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01,
    0x09, 0x02, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x10,
    0x81, 0x02,
    0x09, 0x03, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x10,
    0x91, 0x02, 0xC0,
};
#endif

static const struct usb_device_descriptor device_descriptor = {
    .bLength = sizeof(struct usb_device_descriptor),
    .bDescriptorType = USB_DT_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0xEF,
    .bDeviceSubClass = 0x02,
    .bDeviceProtocol = 0x01,
    .bMaxPacketSize0 = 64,
    .idVendor = VENDOR_ID,
    .idProduct = PRODUCT_ID,
    .bcdDevice = 0x0003,
    .iManufacturer = 0,
    .iProduct = 0,
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

#define AUDIO_AS_INTERFACE(_interface, _alternate, _endpoints)             \
  {                                                                         \
    .bLength = sizeof(usb_standard_as_interface_descriptor),                \
    .bDescriptorType = USB_DT_INTERFACE,                                    \
    .bInterfaceNumber = (_interface),                                       \
    .bAlternateSetting = (_alternate),                                      \
    .bNumEndpoint = (_endpoints),                                            \
    .bInterfaceClass = 0x01,                                                \
    .bInterfaceSubClass = 0x02,                                             \
    .bInterfaceProtocol = 0x20,                                             \
    .iInterface = 0,                                                        \
  }

#define PLAYBACK_ALT(_alternate, _subslot, _resolution)                    \
  {                                                                         \
    .as_interface = AUDIO_AS_INTERFACE(INTERFACE_AUDIO_STREAM,              \
                                       (_alternate), 2),                    \
    .cs_as_interface = {                                                    \
      .bLength = sizeof(struct usb_class_specific_as_interface_descriptor), \
      .bDescriptorType = USB_DT_CS_INTERFACE,                               \
      .bDescriptorSubtype = 0x01,                                           \
      .bTerminalLink = AUDIO_CONTROL_ID_INPUT,                              \
      .bmControls = 0,                                                      \
      .bFormatType = 0x01,                                                  \
      .bmFormats = 0x00000001,                                              \
      .bNrChannels = 2,                                                     \
      .bmChannelConfig = 0x00000003,                                        \
      .iChannelNames = 0,                                                   \
    },                                                                      \
    .cs_as_format_type = {                                                  \
      .bLength = sizeof(struct usb_class_specific_as_type_i_format_descriptor), \
      .bDescriptorType = USB_DT_CS_INTERFACE,                               \
      .bDescriptorSubtype = 0x02,                                           \
      .bFormatType = 0x01,                                                  \
      .bSubslotSize = (_subslot),                                           \
      .bBitResolution = (_resolution),                                      \
    },                                                                      \
    .as_audio_data_endpoint = {                                             \
      .bLength = sizeof(struct usb_standard_as_isochronous_audio_data_endpoint_descriptor), \
      .bDescriptorType = USB_DT_ENDPOINT,                                   \
      .bEndpointAddress = EP_AUDIO_STREAM_OUT,                              \
      .bmAttributes = 0x05,                                                 \
      .wMaxPacketSize = AUDIO_MAX_PACKET_SIZE,                              \
      .bInterval = 1,                                                       \
    },                                                                      \
    .cs_as_audio_data_endpoint = {                                          \
      .bLength = sizeof(struct usb_class_specific_as_isochronous_audio_data_endpoint_descriptor), \
      .bDescriptorType = USB_DT_CS_ENDPOINT,                                \
      .bDescriptorSubtype = 0x01,                                           \
      .bmAttributes = 0,                                                    \
      .bmControls = 0,                                                      \
      .bLockDelayUnits = 0,                                                 \
      .wLockDelay = 0,                                                      \
    },                                                                      \
    .as_isochronous_feedback_endpoint = {                                   \
      .bLength = sizeof(struct usb_standard_as_isochronous_feedback_endpoint_descriptor), \
      .bDescriptorType = USB_DT_ENDPOINT,                                   \
      .bEndpointAddress = EP_AUDIO_FEEDBACK_IN,                             \
      .bmAttributes = 0x11,                                                 \
      .wMaxPacketSize = 4,                                                  \
      .bInterval = 1,                                                       \
    },                                                                      \
  }

struct configuration_descriptor {
  struct usb_configuration_descriptor config;
  struct usb_interface_association_descriptor iad;

  struct ac_descriptors {
    usb_standard_ac_interface_descriptor ac_interface;
    struct usb_class_specific_ac_interface_header_descriptor ac_header;
    struct usb_class_specific_ac_clock_source_descriptor clock_source;
    struct usb_class_specific_ac_input_terminal_descriptor playback_input;
    struct usb_class_specific_ac_output_terminal_descriptor playback_output;
    struct usb_class_specific_ac_feature_unit_descriptor_stereo playback_feature;
    struct usb_class_specific_ac_input_terminal_descriptor capture_input;
    struct usb_class_specific_ac_output_terminal_descriptor capture_output;
  } __attribute__((packed)) ac;

  struct playback_stream_descriptors {
    struct {
      usb_standard_as_interface_descriptor as_interface;
    } __attribute__((packed)) alt0;
    struct playback_alt {
      usb_standard_as_interface_descriptor as_interface;
      struct usb_class_specific_as_interface_descriptor cs_as_interface;
      struct usb_class_specific_as_type_i_format_descriptor cs_as_format_type;
      struct usb_standard_as_isochronous_audio_data_endpoint_descriptor as_audio_data_endpoint;
      struct usb_class_specific_as_isochronous_audio_data_endpoint_descriptor cs_as_audio_data_endpoint;
      struct usb_standard_as_isochronous_feedback_endpoint_descriptor as_isochronous_feedback_endpoint;
    } __attribute__((packed)) alt1, alt2, alt3;
  } __attribute__((packed)) playback;

  struct capture_stream_descriptors {
    struct {
      usb_standard_as_interface_descriptor as_interface;
    } __attribute__((packed)) alt0;
    struct capture_alt1 {
      usb_standard_as_interface_descriptor as_interface;
      struct usb_class_specific_as_interface_descriptor cs_as_interface;
      struct usb_class_specific_as_type_i_format_descriptor cs_as_format_type;
      struct usb_standard_as_isochronous_audio_data_endpoint_descriptor as_audio_data_endpoint;
      struct usb_class_specific_as_isochronous_audio_data_endpoint_descriptor cs_as_audio_data_endpoint;
    } __attribute__((packed)) alt1;
  } __attribute__((packed)) capture;

#if HID_ENABLE
  struct hid_descriptors {
    struct usb_interface_descriptor_t hid_interface;
    struct usb_hid_descriptor_t hid_descriptor;
    struct usb_endpoint_descriptor_t hid_in_descriptor;
    struct usb_endpoint_descriptor_t hid_out_descriptor;
  } __attribute__((packed)) hid;
#endif
} __attribute__((packed)) configuration_descriptor = {
    .config = {
        .bLength = sizeof(struct usb_configuration_descriptor),
        .bDescriptorType = USB_DT_CONFIG,
        .wTotalLength = sizeof(struct configuration_descriptor),
        .bNumInterfaces = INTERFACE_NUM,
        .bConfigurationValue = 1,
        .iConfiguration = 0,
        .bmAttributes = 0x80,
        .bMaxPower = 50,
    },
    .iad = {
        .bLength = sizeof(struct usb_interface_association_descriptor),
        .bDescriptorType = USB_DT_IAD,
        .bFirstInterface = INTERFACE_AUDIO_CONTROL,
        .bInterfaceCount = AUDIO_INTERFACE_NUM,
        .bFunctionClass = 0x01,
        .bFunctionSubClass = 0x00,
        .bFunctionProtocol = 0x20,
        .iFunction = 0,
    },
    .ac = {
        .ac_interface = {
            .bLength = sizeof(usb_standard_ac_interface_descriptor),
            .bDescriptorType = USB_DT_INTERFACE,
            .bInterfaceNumber = INTERFACE_AUDIO_CONTROL,
            .bAlternateSetting = 0,
            .bNumEndpoint = 0,
            .bInterfaceClass = 0x01,
            .bInterfaceSubClass = 0x01,
            .bInterfaceProtocol = 0x20,
            .iInterface = 0,
        },
        .ac_header = {
            .bLength = sizeof(struct usb_class_specific_ac_interface_header_descriptor),
            .bDescriptorType = USB_DT_CS_INTERFACE,
            .bDescriptorSubtype = 0x01,
            .bcdACD = 0x0200,
            .bCategory = 0x01,
            .wTotalLength = sizeof(struct ac_descriptors) - sizeof(usb_standard_ac_interface_descriptor),
            .bmControls = 0,
        },
        .clock_source = {
            .bLength = sizeof(struct usb_class_specific_ac_clock_source_descriptor),
            .bDescriptorType = USB_DT_CS_INTERFACE,
            .bDescriptorSubtype = 0x0A,
            .bClockID = AUDIO_CONTROL_ID_CLOCK,
            .bmAttributes = 0x03,
            .bmControls = 0x03,
            /* Shared by playback and capture; no single associated terminal. */
            .bAssocTerminal = 0,
            .iClockSource = 0,
        },
        .playback_input = {
            .bLength = sizeof(struct usb_class_specific_ac_input_terminal_descriptor),
            .bDescriptorType = USB_DT_CS_INTERFACE,
            .bDescriptorSubtype = 0x02,
            .bTerminalID = AUDIO_CONTROL_ID_INPUT,
            .wTerminalType = 0x0101,
            .bAssocTerminal = 0,
            .bCSourceID = AUDIO_CONTROL_ID_CLOCK,
            .bNrChannels = 2,
            .bmChannelConfig = 0x00000003,
            .iChannelNames = 0,
            .bmControls = 0,
            .iTerminal = 0,
        },
        .playback_output = {
            .bLength = sizeof(struct usb_class_specific_ac_output_terminal_descriptor),
            .bDescriptorType = USB_DT_CS_INTERFACE,
            .bDescriptorSubtype = 0x03,
            .bTerminalID = AUDIO_CONTROL_ID_OUTPUT,
            .wTerminalType = 0x0304,
            .bAssocTerminal = AUDIO_CONTROL_ID_INPUT,
            .bSourceID = AUDIO_CONTROL_ID_FEATURE_UNIT,
            .bCSourceID = AUDIO_CONTROL_ID_CLOCK,
            .bmControls = 0,
            .iTerminal = 0,
        },
        .playback_feature = {
            .bLength = sizeof(struct usb_class_specific_ac_feature_unit_descriptor_stereo),
            .bDescriptorType = USB_DT_CS_INTERFACE,
            .bDescriptorSubtype = 0x06,
            .bUnitID = AUDIO_CONTROL_ID_FEATURE_UNIT,
            .bSourceID = AUDIO_CONTROL_ID_INPUT,
            /* UAC2 control bit-pairs: Mute=0b11 and Volume=0b11.
               Advertise only the controls implemented by usb_audio.c. */
            .bmaControls = {0x0000000F, 0x0000000F, 0x0000000F},
            .iFeature = 0,
        },
        .capture_input = {
            .bLength = sizeof(struct usb_class_specific_ac_input_terminal_descriptor),
            .bDescriptorType = USB_DT_CS_INTERFACE,
            .bDescriptorSubtype = 0x02,
            .bTerminalID = AUDIO_CONTROL_ID_CAPTURE_INPUT,
            .wTerminalType = 0x0601, /* Digital Audio Interface input terminal. */
            .bAssocTerminal = 0,
            .bCSourceID = AUDIO_CONTROL_ID_CLOCK,
            .bNrChannels = 2,
            .bmChannelConfig = 0x00000003,
            .iChannelNames = 0,
            .bmControls = 0,
            .iTerminal = 0,
        },
        .capture_output = {
            .bLength = sizeof(struct usb_class_specific_ac_output_terminal_descriptor),
            .bDescriptorType = USB_DT_CS_INTERFACE,
            .bDescriptorSubtype = 0x03,
            .bTerminalID = AUDIO_CONTROL_ID_CAPTURE_OUTPUT,
            .wTerminalType = 0x0101, /* USB streaming output terminal. */
            .bAssocTerminal = 0,
            .bSourceID = AUDIO_CONTROL_ID_CAPTURE_INPUT,
            .bCSourceID = AUDIO_CONTROL_ID_CLOCK,
            .bmControls = 0,
            .iTerminal = 0,
        },
    },
    .playback = {
        .alt0 = {
            .as_interface = AUDIO_AS_INTERFACE(INTERFACE_AUDIO_STREAM, 0, 0),
        },
        .alt1 = PLAYBACK_ALT(1, 2, 16),
        .alt2 = PLAYBACK_ALT(2, 3, 24),
        .alt3 = PLAYBACK_ALT(3, 4, 32),
    },
    .capture = {
        .alt0 = {
            .as_interface = AUDIO_AS_INTERFACE(INTERFACE_AUDIO_STREAM_CAPTURE, 0, 0),
        },
        .alt1 = {
            .as_interface = AUDIO_AS_INTERFACE(INTERFACE_AUDIO_STREAM_CAPTURE, 1, 1),
            .cs_as_interface = {
                .bLength = sizeof(struct usb_class_specific_as_interface_descriptor),
                .bDescriptorType = USB_DT_CS_INTERFACE,
                .bDescriptorSubtype = 0x01,
                .bTerminalLink = AUDIO_CONTROL_ID_CAPTURE_OUTPUT,
                .bmControls = 0,
                .bFormatType = 0x01,
                .bmFormats = 0x00000001,
                .bNrChannels = 2,
                .bmChannelConfig = 0x00000003,
                .iChannelNames = 0,
            },
            .cs_as_format_type = {
                .bLength = sizeof(struct usb_class_specific_as_type_i_format_descriptor),
                .bDescriptorType = USB_DT_CS_INTERFACE,
                .bDescriptorSubtype = 0x02,
                .bFormatType = 0x01,
                .bSubslotSize = 4,
                .bBitResolution = 32,
            },
            .as_audio_data_endpoint = {
                .bLength = sizeof(struct usb_standard_as_isochronous_audio_data_endpoint_descriptor),
                .bDescriptorType = USB_DT_ENDPOINT,
                .bEndpointAddress = EP_AUDIO_CAPTURE_IN,
                .bmAttributes = 0x05,
                .wMaxPacketSize = AUDIO_CAPTURE_MAX_PACKET_SIZE,
                .bInterval = 1,
            },
            .cs_as_audio_data_endpoint = {
                .bLength = sizeof(struct usb_class_specific_as_isochronous_audio_data_endpoint_descriptor),
                .bDescriptorType = USB_DT_CS_ENDPOINT,
                .bDescriptorSubtype = 0x01,
                .bmAttributes = 0,
                .bmControls = 0,
                .bLockDelayUnits = 0,
                .wLockDelay = 0,
            },
        },
    },
#if HID_ENABLE
    .hid = {
        .hid_interface = {
            .bLength = sizeof(struct usb_interface_descriptor_t),
            .bDescriptorType = USB_DT_INTERFACE,
            .bInterfaceNumber = INTERFACE_HID,
            .bAlternateSetting = 0,
            .bNumEndpoints = 2,
            .bInterfaceClass = 0x03,
            .bInterfaceSubClass = 0,
            .bInterfaceProtocol = 0,
            .iInterface = 0,
        },
        .hid_descriptor = {
            .bLength = sizeof(struct usb_hid_descriptor_t),
            .bDescriptorType = USB_DT_HID,
            .bcdHID = 0x0111,
            .bCountryCode = 0,
            .bNumDescriptors = 1,
            .bDescriptorType2 = USB_DT_HID_REPORT,
            .wDescriptorLength = sizeof(report_descriptor),
        },
        .hid_in_descriptor = {
            .bLength = sizeof(struct usb_endpoint_descriptor_t),
            .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = EP_HID_IN,
            .bmAttributes = 0x03,
            .wMaxPacketSize = 0x10,
            .bInterval = HID_INTERVAL_MS,
        },
        .hid_out_descriptor = {
            .bLength = sizeof(struct usb_endpoint_descriptor_t),
            .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = EP_HID_OUT,
            .bmAttributes = 0x03,
            .wMaxPacketSize = 0x10,
            .bInterval = HID_INTERVAL_MS,
        },
    },
#endif
};

#undef PLAYBACK_ALT
#undef AUDIO_AS_INTERFACE
