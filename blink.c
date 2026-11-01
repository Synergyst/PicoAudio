#include "blink.h"

#if defined(PICODAC_NO_PIO_LED)

/*
 * Pico 2 W's onboard LED is connected to the CYW43 wireless chip, not to a
 * normal RP2350 GPIO. This project does not initialize the CYW43, so provide
 * harmless no-op implementations instead of treating GPIO25 as an LED pin.
 */
void blink_init(PIO pio, uint led_pin) {
  (void)pio;
  (void)led_pin;
}

void blink_deinit(void) {}
void blink_start(void) {}
void blink_stop(void) {}
void blink_set_period_us(uint32_t period_us) { (void)period_us; }
void blink_led_on(void) {}
void blink_led_off(void) {}
void blink_notify_cpu_freq_change(void) {}

#else

#include "blink.pio.h"
#include "hardware/pio.h"
#include "pico/assert.h"

// Module-level state
static PIO pio_instance = NULL;
static uint sm = 0;
static uint pio_offset = 0;
static uint led_pin;
static uint32_t current_period_us = 500000;
static bool is_blinking = false;

void blink_init(PIO pio, uint pin) {
  assert(pio_instance == NULL);

  pio_instance = pio;
  led_pin = pin;
  is_blinking = false;

  pio_offset = pio_add_program(pio_instance, &blink_program);
  sm = pio_claim_unused_sm(pio_instance, true);
  blink_program_init(pio_instance, sm, pio_offset, led_pin);
  blink_notify_cpu_freq_change();
}

void blink_deinit(void) {
  assert(pio_instance != NULL);
  assert(pio_sm_is_claimed(pio_instance, sm));

  blink_program_deinit(pio_instance, sm, led_pin);
  pio_sm_unclaim(pio_instance, sm);
  pio_remove_program(pio_instance, &blink_program, pio_offset);

  pio_instance = NULL;
  is_blinking = false;
}

void blink_start(void) {
  assert(pio_instance != NULL);
  if (is_blinking) return;
  pio_sm_set_enabled(pio_instance, sm, true);
  is_blinking = true;
}

void blink_stop(void) {
  assert(pio_instance != NULL);
  if (!is_blinking) return;
  pio_sm_set_enabled(pio_instance, sm, false);
  pio_sm_exec(pio_instance, sm, pio_encode_set(pio_pins, 0));
  is_blinking = false;
}

void blink_set_period_us(uint32_t period_us) {
  assert(pio_instance != NULL);
  assert(pio_sm_is_claimed(pio_instance, sm));

  blink_start();
  current_period_us = period_us;
  pio_sm_clear_fifos(pio_instance, sm);

  if (period_us < 10) {
    pio_sm_put_blocking(pio_instance, sm, 0);
    return;
  }

  uint32_t x = (period_us / 2) - 4;
  pio_sm_put_blocking(pio_instance, sm, x);
}

void blink_led_on(void) {
  assert(pio_instance != NULL);
  blink_stop();
  pio_sm_exec(pio_instance, sm, pio_encode_set(pio_pins, 1));
}

void blink_led_off(void) {
  assert(pio_instance != NULL);
  blink_stop();
  pio_sm_exec(pio_instance, sm, pio_encode_set(pio_pins, 0));
}

void blink_notify_cpu_freq_change(void) {
  assert(pio_instance != NULL);
  assert(pio_sm_is_claimed(pio_instance, sm));

  if (is_blinking) pio_sm_set_enabled(pio_instance, sm, false);
  pio_sm_set_clkdiv(pio_instance, sm, blink_get_clkdiv());
  if (is_blinking) pio_sm_set_enabled(pio_instance, sm, true);
}

#endif
