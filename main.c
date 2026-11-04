#include <stdint.h>

#include "audio_capture.h"
#include "audio_device.h"
#include "blink.h"
#include "hardware/clocks.h"
#include "log.h"
#include "pico/stdlib.h"
#include "gpio_audio_link.h"
#include "usb.h"
#include "usb_audio.h"
#include "usb_audio_capture.h"
#include "usb_hid.h"

int main(void) {
#if PICO_RP2350
  /* Standard 150 MHz RP2350 system clock. */
  set_sys_clock_pll(12 * MHZ * 125u, 5, 2);
#else
  /* Retain the project's established RP2040 system clock configuration. */
  set_sys_clock_pll(12 * MHZ * 192u, 5, 5);
#endif

  stdio_init_all();

  /*
   * The GPIO startup handshake is intentionally first. It blocks here until
   * the other RP2350 board is present and has acknowledged GP16-GP19. USB
   * UAC2 is not initialized or enumerated while the peer is absent.
   */
  gpio_audio_link_init();

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

#if defined(PICODAC_GPIO_LINK_ROLE_SLAVE)
  LOG_INFO("Booted: USB UAC2 endpoints; GPIO C diagnostic slave (PCM disabled)");
#else
  LOG_INFO("Booted: USB UAC2 endpoints; GPIO C diagnostic master (PCM disabled)");
#endif

  while (true) {
    usb_device_task();
    audio_device_task();
    gpio_audio_link_task();
    audio_capture_task();
  }
}
