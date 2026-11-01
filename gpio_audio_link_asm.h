#pragma once

#include <stdint.h>

typedef struct {
  const volatile uint32_t *gpio_in;   /* offset 0 */
  volatile uint32_t *gpio_set;  /* offset 4 */
  volatile uint32_t *gpio_clr;  /* offset 8 */
  uint32_t cs_mask;             /* offset 12 */
  uint32_t sck_mask;            /* offset 16 */
  uint32_t mosi_mask;           /* offset 20 */
  uint32_t miso_mask;           /* offset 24 */
} gpio_bitbang_context_t;

void gpio_bitbang_master_transfer_asm(const gpio_bitbang_context_t *context,
                                      const uint8_t *transmit,
                                      uint8_t *receive, uint32_t length);

int gpio_bitbang_slave_transfer_asm(const gpio_bitbang_context_t *context,
                                    const uint8_t *transmit,
                                    uint8_t *receive, uint32_t length);
