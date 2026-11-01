/*
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 */

#include "hardware/hardware.h"

#if defined(ENCODER_ENABLE)
void hardware_encoder_init(void) {}
void hardware_encoder_poll(void) {}
int16_t hardware_encoder_read(uint32_t index) { (void)index; return 0; }
bool hardware_encoder_has_button(uint32_t index) { (void)index; return false; }
bool hardware_encoder_button_read(uint32_t index) { (void)index; return true; }
#endif

#if defined(DIGITAL_ENABLE)
void digital_gpio_init(void) {}
void digital_gpio_scan(bool *raw_states) { (void)raw_states; }
#endif

#if defined(RGB_ENABLE)
void hardware_rgb_init(void) {}
bool hardware_rgb_is_ready(void) { return true; }
void hardware_rgb_show(const uint8_t *grb_buf, uint32_t num_leds) {
  (void)grb_buf;
  (void)num_leds;
}
#endif

