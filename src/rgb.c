/*
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "rgb.h"
#include "hardware/hardware.h"
#include "matrix.h"

#if defined(RGB_ENABLE)

#if !defined(RGB_MAX_CURRENT_MA)
#define RGB_MAX_CURRENT_MA 400u
#endif

static uint8_t led_grb[RGB_NUM_LEDS * 3u];
static uint8_t tx_grb[RGB_NUM_LEDS * 3u];
static bool rgb_dirty = true;

void rgb_set_led(uint32_t index, uint8_t r, uint8_t g, uint8_t b) {
  if (index >= RGB_NUM_LEDS) {
    return;
  }
  uint32_t base = index * 3u;
  if (led_grb[base] != g || led_grb[base + 1] != r || led_grb[base + 2] != b) {
    led_grb[base] = g;
    led_grb[base + 1] = r;
    led_grb[base + 2] = b;
    rgb_dirty = true;
  }
}

void rgb_init(void) {
  hardware_rgb_init();
  for (uint32_t i = 0; i < RGB_NUM_LEDS; i++) {
    rgb_set_led(i, 0, 32, 64);
  }
}

void rgb_task(void) {
  static uint32_t last_rgb_ms = 0;
  uint32_t now = timer_read();
  if (now - last_rgb_ms < 8) { // ~125 Hz frame cap
    return;
  }
  if (!rgb_dirty || !hardware_rgb_is_ready()) {
    return;
  }
  last_rgb_ms = now;
  rgb_dirty = false;

  // Enforce global USB / VREF current budget (~20mA per channel at 255 -> 60mA full white)
  // sum_channels * 20 / 255 mA <= RGB_MAX_CURRENT_MA
  uint32_t sum_channels = 0;
  for (uint32_t i = 0; i < RGB_NUM_LEDS * 3u; i++) {
    sum_channels += led_grb[i];
  }
  uint32_t max_allowed_sum = (RGB_MAX_CURRENT_MA * 255u) / 20u;
  if (sum_channels > max_allowed_sum && sum_channels > 0) {
    uint32_t scale_q8 = (max_allowed_sum << 8) / sum_channels;
    for (uint32_t i = 0; i < RGB_NUM_LEDS * 3u; i++) {
      tx_grb[i] = (uint8_t)(((uint32_t)led_grb[i] * scale_q8) >> 8);
    }
  } else {
    for (uint32_t i = 0; i < RGB_NUM_LEDS * 3u; i++) {
      tx_grb[i] = led_grb[i];
    }
  }

  hardware_rgb_show(tx_grb, RGB_NUM_LEDS);
}

#else

void rgb_init(void) {}
void rgb_set_led(uint32_t index, uint8_t r, uint8_t g, uint8_t b) {
  (void)index;
  (void)r;
  (void)g;
  (void)b;
}
void rgb_task(void) {}

#endif

