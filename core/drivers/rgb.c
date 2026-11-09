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

#include "eeconfig.h"
#include "hardware/hardware.h"
#include "matrix.h"

static uint8_t last_active_mode = RGB_MODE_RAINBOW;

void rgb_toggle(void) {
  rgb_options_t opts = CURRENT_PROFILE.rgb_options;
  if (opts.mode == RGB_MODE_OFF) {
    opts.mode = (last_active_mode > RGB_MODE_OFF &&
                 last_active_mode < RGB_MODE_COUNT)
                    ? last_active_mode
                    : RGB_MODE_RAINBOW;
  } else {
    last_active_mode = opts.mode;
    opts.mode = RGB_MODE_OFF;
  }
  EECONFIG_WRITE(profiles[eeconfig->current_profile].rgb_options, &opts);
}

void rgb_step_mode(bool forward) {
  rgb_options_t opts = CURRENT_PROFILE.rgb_options;
  uint8_t active_count = RGB_MODE_COUNT - 1; // modes 1..RGB_MODE_COUNT-1
  if (opts.mode == RGB_MODE_OFF || opts.mode >= RGB_MODE_COUNT) {
    opts.mode = forward ? RGB_MODE_SOLID : (RGB_MODE_COUNT - 1);
  } else {
    uint8_t idx = opts.mode - 1;
    idx = forward ? (uint8_t)((idx + 1) % active_count)
                  : (uint8_t)((idx + active_count - 1) % active_count);
    opts.mode = idx + 1;
  }
  last_active_mode = opts.mode;
  EECONFIG_WRITE(profiles[eeconfig->current_profile].rgb_options, &opts);
}

void rgb_step_brightness(bool increase) {
  rgb_options_t opts = CURRENT_PROFILE.rgb_options;
  if (increase) {
    opts.brightness = (opts.brightness > 230) ? 255 : (uint8_t)(opts.brightness + 25);
  } else {
    opts.brightness = (opts.brightness < 25) ? 0 : (uint8_t)(opts.brightness - 25);
  }
  EECONFIG_WRITE(profiles[eeconfig->current_profile].rgb_options, &opts);
}

void rgb_step_speed(bool increase) {
  rgb_options_t opts = CURRENT_PROFILE.rgb_options;
  if (increase) {
    opts.speed = (opts.speed > 230) ? 255 : (uint8_t)(opts.speed + 25);
  } else {
    opts.speed = (opts.speed < 25) ? 0 : (uint8_t)(opts.speed - 25);
  }
  EECONFIG_WRITE(profiles[eeconfig->current_profile].rgb_options, &opts);
}

#if defined(RGB_ENABLE)

#if !defined(RGB_MAX_CURRENT_MA)
#define RGB_MAX_CURRENT_MA 400u
#endif

static const uint8_t rgb_key_map[RGB_NUM_LEDS] = RGB_KEY_MAP;
static const uint8_t rgb_led_positions[RGB_NUM_LEDS][2] = RGB_LED_POSITIONS;

static uint8_t led_grb[RGB_NUM_LEDS * 3u];
static uint8_t tx_grb[RGB_NUM_LEDS * 3u];
static uint16_t reactive_val[RGB_NUM_LEDS];
static uint32_t anim_phase_q8 = 0;
static bool rgb_dirty = true;

static inline uint8_t scale8(uint8_t val, uint8_t scale) {
  return (uint8_t)(((uint32_t)val * ((uint32_t)scale + 1u)) >> 8);
}

static inline rgb_color_t blend_color(rgb_color_t c_base, rgb_color_t c_active,
                                      uint8_t t, uint8_t brightness) {
  uint8_t inv = (uint8_t)(255u - t);
  uint8_t r = (uint8_t)(((uint32_t)c_base.r * inv + (uint32_t)c_active.r * t) >> 8);
  uint8_t g = (uint8_t)(((uint32_t)c_base.g * inv + (uint32_t)c_active.g * t) >> 8);
  uint8_t b = (uint8_t)(((uint32_t)c_base.b * inv + (uint32_t)c_active.b * t) >> 8);
  return (rgb_color_t){
      .r = scale8(r, brightness),
      .g = scale8(g, brightness),
      .b = scale8(b, brightness),
  };
}

// Smooth S-curve triangle wave: maps phase (0..255) -> (0..255)
static inline uint8_t wave_tri8(uint8_t phase) {
  uint32_t lin = (phase & 0x80u) ? ((255u - phase) << 1) : ((uint32_t)phase << 1);
  if (lin > 255u) lin = 255u;
  // Smoothstep: t^2 * (3 - 2t)
  uint32_t t2 = (lin * lin) >> 8;
  uint32_t smooth = (t2 * (765u - (lin << 1))) >> 8;
  return (uint8_t)(smooth > 255u ? 255u : smooth);
}

static rgb_color_t hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v) {
  if (s == 0) {
    return (rgb_color_t){v, v, v};
  }
  uint32_t region = h / 43u;
  uint32_t remainder = (h - (region * 43u)) * 6u;

  uint8_t p = (uint8_t)(((uint32_t)v * (255u - s)) >> 8);
  uint8_t q = (uint8_t)(((uint32_t)v * (255u - ((s * remainder) >> 8))) >> 8);
  uint8_t t = (uint8_t)(((uint32_t)v * (255u - ((s * (255u - remainder)) >> 8))) >> 8);

  switch (region) {
  case 0:
    return (rgb_color_t){v, t, p};
  case 1:
    return (rgb_color_t){q, v, p};
  case 2:
    return (rgb_color_t){p, v, t};
  case 3:
    return (rgb_color_t){p, q, v};
  case 4:
    return (rgb_color_t){t, p, v};
  default:
    return (rgb_color_t){v, p, q};
  }
}

static uint8_t get_key_analog_depth8(uint8_t key_idx) {
  if (key_idx < NUM_KEYS) {
    uint32_t travel_um = (uint32_t)eeconfig->calibration.switch_travel[key_idx] * 100u;
    if (travel_um == 0) travel_um = 3600u;
    uint32_t dist = key_matrix[key_idx].distance;
    if (dist >= travel_um) return 255u;
    return (uint8_t)((dist * 255u) / travel_um);
  }
  // Unmapped / underglow LED: use maximum depth across all keys
  uint8_t max_depth = 0;
  for (uint32_t k = 0; k < NUM_KEYS; k++) {
    uint32_t travel_um = (uint32_t)eeconfig->calibration.switch_travel[k] * 100u;
    if (travel_um == 0) travel_um = 3600u;
    uint32_t dist = key_matrix[k].distance;
    uint8_t d = (dist >= travel_um) ? 255u : (uint8_t)((dist * 255u) / travel_um);
    if (d > max_depth) max_depth = d;
  }
  return max_depth;
}

static bool is_led_key_pressed(uint8_t key_idx) {
  if (key_idx < NUM_KEYS) {
    return key_matrix[key_idx].is_pressed;
  }
  for (uint32_t k = 0; k < NUM_KEYS; k++) {
    if (key_matrix[k].is_pressed) return true;
  }
  return false;
}

static uint8_t get_directional_coord(uint32_t led_idx, uint8_t direction) {
  uint8_t x = rgb_led_positions[led_idx][0];
  uint8_t y = rgb_led_positions[led_idx][1];
  switch (direction) {
  case RGB_DIR_RIGHT_TO_LEFT:
    return (uint8_t)(255u - x);
  case RGB_DIR_TOP_TO_BOTTOM:
    return y;
  case RGB_DIR_BOTTOM_TO_TOP:
    return (uint8_t)(255u - y);
  case RGB_DIR_CENTER_OUT: {
    int32_t dx = (int32_t)x - 128;
    int32_t dy = (int32_t)y - 128;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    uint32_t dist = (uint32_t)(dx + dy);
    return (uint8_t)(dist > 255u ? 255u : dist);
  }
  case RGB_DIR_LEFT_TO_RIGHT:
  default:
    return x;
  }
}

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

static void rgb_render_frame(void) {
  const eeconfig_profile_t *prof = &CURRENT_PROFILE;
  const rgb_options_t *opts = &prof->rgb_options;
  const uint8_t brightness = opts->brightness;

  if (opts->mode == RGB_MODE_OFF || brightness == 0) {
    for (uint32_t i = 0; i < RGB_NUM_LEDS; i++) {
      rgb_set_led(i, 0, 0, 0);
    }
    return;
  }

  // Advance animation phase proportional to speed (speed 0..255 -> step 16..1040 in Q8 per 8ms frame)
  uint32_t step_q8 = 16u + ((uint32_t)opts->speed * 4u);
  anim_phase_q8 = (anim_phase_q8 + step_q8) & 0xFFFFu;
  uint8_t phase = (uint8_t)(anim_phase_q8 >> 8);

  switch (opts->mode) {
  case RGB_MODE_SOLID: {
    uint8_t r = scale8(opts->color1.r, brightness);
    uint8_t g = scale8(opts->color1.g, brightness);
    uint8_t b = scale8(opts->color1.b, brightness);
    for (uint32_t i = 0; i < RGB_NUM_LEDS; i++) {
      rgb_set_led(i, r, g, b);
    }
    break;
  }
  case RGB_MODE_PER_KEY: {
    for (uint32_t i = 0; i < RGB_NUM_LEDS; i++) {
      rgb_color_t c = prof->rgb_colors[i];
      rgb_set_led(i, scale8(c.r, brightness), scale8(c.g, brightness),
                  scale8(c.b, brightness));
    }
    break;
  }
  case RGB_MODE_BREATHING: {
    uint8_t w = wave_tri8(phase);
    rgb_color_t c = blend_color(opts->color2, opts->color1, w, brightness);
    for (uint32_t i = 0; i < RGB_NUM_LEDS; i++) {
      rgb_set_led(i, c.r, c.g, c.b);
    }
    break;
  }
  case RGB_MODE_RAINBOW: {
    for (uint32_t i = 0; i < RGB_NUM_LEDS; i++) {
      uint8_t spatial = get_directional_coord(i, opts->direction);
      uint8_t hue = (uint8_t)((spatial >> 1) + (uint8_t)((i * 64u) / RGB_NUM_LEDS) - phase);
      rgb_color_t c = hsv_to_rgb(hue, 255u, brightness);
      rgb_set_led(i, c.r, c.g, c.b);
    }
    break;
  }
  case RGB_MODE_WAVE: {
    for (uint32_t i = 0; i < RGB_NUM_LEDS; i++) {
      uint8_t spatial = get_directional_coord(i, opts->direction);
      uint8_t w = wave_tri8((uint8_t)(spatial - phase));
      rgb_color_t c = blend_color(opts->color2, opts->color1, w, brightness);
      rgb_set_led(i, c.r, c.g, c.b);
    }
    break;
  }
  case RGB_MODE_REACTIVE: {
    uint16_t decay = 64u + ((uint16_t)opts->speed * 8u);
    for (uint32_t i = 0; i < RGB_NUM_LEDS; i++) {
      if (is_led_key_pressed(rgb_key_map[i])) {
        reactive_val[i] = 65535u;
      } else if (reactive_val[i] > decay) {
        reactive_val[i] -= decay;
      } else {
        reactive_val[i] = 0;
      }
      uint8_t t = (uint8_t)(reactive_val[i] >> 8);
      rgb_color_t c = blend_color(opts->color2, opts->color1, t, brightness);
      rgb_set_led(i, c.r, c.g, c.b);
    }
    break;
  }
  case RGB_MODE_ANALOG_DEPTH:
  default: {
    for (uint32_t i = 0; i < RGB_NUM_LEDS; i++) {
      uint8_t depth = get_key_analog_depth8(rgb_key_map[i]);
      rgb_color_t c = blend_color(opts->color2, opts->color1, depth, brightness);
      rgb_set_led(i, c.r, c.g, c.b);
    }
    break;
  }
  }
}

void rgb_init(void) {
  hardware_rgb_init();
  last_active_mode = CURRENT_PROFILE.rgb_options.mode > RGB_MODE_OFF
                         ? CURRENT_PROFILE.rgb_options.mode
                         : RGB_MODE_RAINBOW;
  rgb_render_frame();
}

void rgb_task(void) {
  static uint32_t last_rgb_ms = 0;
  static uint32_t last_tx_ms = 0;
  uint32_t now = timer_read();
  if (now - last_rgb_ms < 8) { // ~125 Hz frame cap
    return;
  }
  if (!hardware_rgb_is_ready()) {
    return;
  }
  last_rgb_ms = now;

  rgb_render_frame();

  if (!rgb_dirty && (now - last_tx_ms < 250)) {
    return;
  }
  rgb_dirty = false;
  last_tx_ms = now;

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
