#pragma once

#include "usb_common.h"
#include "usb_config.h"

/* Capture-only UAC2 descriptor set for the RP2040 ADC-mode build.
   Signal topology: digital I2S input -> microphone terminal -> USB streaming
   output terminal. The I2S clock is supplied externally by Pico A. */

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
    .bcdDevice = 0x0002,
    .iManufacturer = 0,
    .iProduct = 0,
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

struct adc_configuration_descriptor {
    struct usb_configuration_descriptor config;
    struct usb_interface_association_descriptor iad;

    struct ac_descriptors {
        usb_standard_ac_interface_descriptor ac_interface;
        struct usb_class_specific_ac_interface_header_descriptor ac_header;
        struct usb_class_specific_ac_clock_source_descriptor clock_source;
        struct usb_class_specific_ac_input_terminal_descriptor capture_input;
        struct usb_class_specific_ac_output_terminal_descriptor capture_output;
    } __attribute__((packed)) ac;

    struct as_descriptors {
        struct as_alt0 {
            usb_standard_as_interface_descriptor interface;
        } __attribute__((packed)) alt0;

        struct as_alt1 {
            usb_standard_as_interface_descriptor interface;
            struct usb_class_specific_as_interface_descriptor as_general;
            struct usb_class_specific_as_type_i_format_descriptor format;
            struct usb_standard_as_isochronous_audio_data_endpoint_descriptor data_endpoint;
            struct usb_class_specific_as_isochronous_audio_data_endpoint_descriptor cs_data_endpoint;
        } __attribute__((packed)) alt1;
    } __attribute__((packed)) as;
} __attribute__((packed)) configuration_descriptor = {
    .config = {
        .bLength = sizeof(struct usb_configuration_descriptor),
        .bDescriptorType = USB_DT_CONFIG,
        .wTotalLength = sizeof(struct adc_configuration_descriptor),
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
            .bCategory = 0x03,
            .wTotalLength = sizeof(struct ac_descriptors) - sizeof(usb_standard_ac_interface_descriptor),
            .bmControls = 0,
        },
        .clock_source = {
            .bLength = sizeof(struct usb_class_specific_ac_clock_source_descriptor),
            .bDescriptorType = USB_DT_CS_INTERFACE,
            .bDescriptorSubtype = 0x0A,
            .bClockID = AUDIO_CONTROL_ID_CLOCK,
            .bmAttributes = 0x00, /* External clock: Pico A supplies I2S BCLK/LRCLK. */
            .bmControls = 0x01,   /* Clock frequency control: read-only. */
            .bAssocTerminal = AUDIO_CAPTURE_INPUT_TERMINAL_ID,
            .iClockSource = 0,
        },
        .capture_input = {
            .bLength = sizeof(struct usb_class_specific_ac_input_terminal_descriptor),
            .bDescriptorType = USB_DT_CS_INTERFACE,
            .bDescriptorSubtype = 0x02,
            .bTerminalID = AUDIO_CAPTURE_INPUT_TERMINAL_ID,
            .wTerminalType = 0x0201, /* Microphone terminal; signal itself is digital I2S. */
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
            .bTerminalID = AUDIO_CAPTURE_OUTPUT_TERMINAL_ID,
            .wTerminalType = 0x0101, /* USB streaming output terminal. */
            .bAssocTerminal = 0,
            .bSourceID = AUDIO_CAPTURE_INPUT_TERMINAL_ID,
            .bCSourceID = AUDIO_CONTROL_ID_CLOCK,
            .bmControls = 0,
            .iTerminal = 0,
        },
    },
    .as = {
        .alt0 = {
            .interface = {
                .bLength = sizeof(usb_standard_as_interface_descriptor),
                .bDescriptorType = USB_DT_INTERFACE,
                .bInterfaceNumber = INTERFACE_AUDIO_STREAM,
                .bAlternateSetting = 0,
                .bNumEndpoint = 0,
                .bInterfaceClass = 0x01,
                .bInterfaceSubClass = 0x02,
                .bInterfaceProtocol = 0x20,
                .iInterface = 0,
            },
        },
        .alt1 = {
            .interface = {
                .bLength = sizeof(usb_standard_as_interface_descriptor),
                .bDescriptorType = USB_DT_INTERFACE,
                .bInterfaceNumber = INTERFACE_AUDIO_STREAM,
                .bAlternateSetting = 1,
                .bNumEndpoint = 1,
                .bInterfaceClass = 0x01,
                .bInterfaceSubClass = 0x02,
                .bInterfaceProtocol = 0x20,
                .iInterface = 0,
            },
            .as_general = {
                .bLength = sizeof(struct usb_class_specific_as_interface_descriptor),
                .bDescriptorType = USB_DT_CS_INTERFACE,
                .bDescriptorSubtype = 0x01,
                .bTerminalLink = AUDIO_CAPTURE_OUTPUT_TERMINAL_ID,
                .bmControls = 0,
                .bFormatType = 0x01,
                .bmFormats = 0x00000001,
                .bNrChannels = 2,
                .bmChannelConfig = 0x00000003,
                .iChannelNames = 0,
            },
            .format = {
                .bLength = sizeof(struct usb_class_specific_as_type_i_format_descriptor),
                .bDescriptorType = USB_DT_CS_INTERFACE,
                .bDescriptorSubtype = 0x02,
                .bFormatType = 0x01,
                .bSubslotSize = 4,
                .bBitResolution = 32,
            },
            .data_endpoint = {
                .bLength = sizeof(struct usb_standard_as_isochronous_audio_data_endpoint_descriptor),
                .bDescriptorType = USB_DT_ENDPOINT,
                .bEndpointAddress = EP_AUDIO_CAPTURE_IN,
                .bmAttributes = 0x05, /* Isochronous, asynchronous data endpoint. */
                .wMaxPacketSize = AUDIO_CAPTURE_MAX_PACKET_SIZE,
                .bInterval = 1,
            },
            .cs_data_endpoint = {
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
};
