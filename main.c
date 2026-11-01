#include <stdint.h>

#include "audio_capture.h"
#include "audio_device.h"
#include "blink.h"
#include "hardware/clocks.h"
#include "log.h"
#include "pico/stdlib.h"
#include "usb.h"
#include "usb_audio.h"
#include "usb_audio_capture.h"
#include "usb_hid.h"

int main(void) {
#if PICO_RP2350
  /*
   * Use the standard 150 MHz RP2350 system clock. The 1.5 GHz PLL VCO
   * is within the RP2350 limit; the PIO clock divider handles the I2S rate.
   */
  set_sys_clock_pll(12 * MHZ * 125u, 5, 2);
#else
  /* 92.16 MHz gives an exact PIO divider for 48 kHz, 32-bit I2S slots. */
  set_sys_clock_pll(12 * MHZ * 192u, 5, 5);
#endif

  stdio_init_all();

#ifndef PICODAC_NO_PIO_LED
  blink_init(pio1, PICO_DEFAULT_LED_PIN);
#endif
  audio_device_init();
  audio_capture_init();

  usb_device_init();
  usb_audio_init();
  usb_audio_capture_init();
#ifdef HID_ENABLE
  usb_hid_init();
#endif

#if defined(PICODAC_I2S_ROLE_SLAVE)
  LOG_INFO("Booted: USB UAC2 playback + capture; I2S CLOCK SLAVE");
#else
  LOG_INFO("Booted: USB UAC2 playback + capture; I2S CLOCK MASTER");
#endif

  while (true) {
    usb_device_task();
    audio_device_task();
    audio_capture_task();
  }
}
