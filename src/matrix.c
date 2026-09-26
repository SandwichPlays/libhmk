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

#include "matrix.h"

#include "distance.h"
#include "eeconfig.h"
#include "hardware/hardware.h"
#include "lib/bitmap.h"

// Exponential moving average (EMA) filter with symmetric half-step rounding
#define EMA(x, y)                                                              \
  (((uint32_t)(x) +                                                            \
    ((uint32_t)(y) * ((1 << MATRIX_EMA_ALPHA_EXPONENT) - 1)) +                 \
    (1 << (MATRIX_EMA_ALPHA_EXPONENT - 1))) >>                                 \
   MATRIX_EMA_ALPHA_EXPONENT)

// Bitmap for tracking which keys have inverted polarity (North-facing magnet)
static bitmap_t key_inverted[] = MAKE_BITMAP(NUM_KEYS);

bool matrix_is_key_inverted(uint8_t key) {
  if (key < NUM_KEYS)
    return bitmap_get(key_inverted, key);
  return false;
}

__attribute__((always_inline)) static inline uint16_t
matrix_analog_read(uint8_t key) {
  uint16_t val = analog_read(key);
  if (bitmap_get(key_inverted, key)) {
    val = ADC_MAX_VALUE - val;
  }
#if defined(MATRIX_INVERT_ADC_VALUES)
  val = ADC_MAX_VALUE - val;
#endif
  return val;
}

__attribute__((always_inline)) static inline uint16_t
matrix_bottom_out_value(uint8_t key, uint16_t rest_value) {
  uint16_t threshold =
      eeconfig->bottom_out_threshold[key] & BOTTOM_OUT_THRESHOLD_MASK;
  return M_MIN(rest_value +
                   M_MAX(eeconfig->calibration.initial_bottom_out_threshold,
                         threshold),
                ADC_MAX_VALUE);
}

#if !defined(MATRIX_REST_LENIENCE_COUNTS)
#define MATRIX_REST_LENIENCE_COUNTS 2
#endif

// Recompute and store the cached rest lenience for a key.
// Must be called whenever adc_rest_value or adc_bottom_out_value changes.
__attribute__((always_inline)) static inline void
matrix_update_lenience(uint8_t key) {
  key_matrix[key].adc_rest_lenience = MATRIX_REST_LENIENCE_COUNTS;
}

key_state_t key_matrix[NUM_KEYS];

// Bitmap for tracking which keys have Rapid Trigger disabled
static bitmap_t rapid_trigger_disabled[] = MAKE_BITMAP(NUM_KEYS);

static bool manual_calib_active = false;
static uint8_t manual_calib_status[NUM_KEYS] = {0};
static uint8_t manual_calib_dir[NUM_KEYS] = {0};
static uint16_t manual_calib_peak[NUM_KEYS] = {0};

static uint32_t stable_timer[NUM_KEYS] = {0};
static uint16_t hyst_gap[NUM_KEYS] = {0};
static uint16_t raw_boot_rest[NUM_KEYS] = {0};
static uint16_t last_raw_val[NUM_KEYS] = {0};
static bool is_moving[NUM_KEYS] = {false};
static uint16_t locked_dist[NUM_KEYS] = {0};
static uint16_t last_motion_anchor[NUM_KEYS] = {0};
static uint8_t stationary_count[NUM_KEYS] = {0};
static volatile bool matrix_state_changed = false;

void matrix_update_calibration(void) {
  for (uint32_t i = 0; i < NUM_KEYS; i++) {
    uint8_t travel_tenths = eeconfig->calibration.switch_travel[i];
    if (travel_tenths < 20 || travel_tenths > 50) {
      travel_tenths = 36;
    }
    uint16_t gap = (uint16_t)(5000 / travel_tenths);
    hyst_gap[i] = (gap < 50) ? 139 : gap;
  }
}

void matrix_init(void) { matrix_recalibrate(false); }

void matrix_recalibrate(bool reset_bottom_out_threshold) {
  if (reset_bottom_out_threshold) {
    uint16_t bottom_out_threshold[NUM_KEYS] = {0};
    for (uint32_t i = 0; i < NUM_KEYS; i++) {
      if (bitmap_get(key_inverted, i) ||
          (eeconfig->bottom_out_threshold[i] & BOTTOM_OUT_POLARITY_INVERTED)) {
        bottom_out_threshold[i] = BOTTOM_OUT_POLARITY_INVERTED;
      }
    }
    EECONFIG_WRITE(bottom_out_threshold, bottom_out_threshold);
  }

  // Load saved polarity from bottom_out_threshold bit 15
  for (uint32_t i = 0; i < NUM_KEYS; i++) {
    bool inv =
        bitmap_get(key_inverted, i) ||
        ((eeconfig->bottom_out_threshold[i] & BOTTOM_OUT_POLARITY_INVERTED) != 0);
    bitmap_set(key_inverted, i, inv);
  }

  // 1. Flush EMA filter and sample live ADC readings
  for (uint32_t step = 0; step < 16; step++) {
    analog_task();
    for (uint32_t i = 0; i < NUM_KEYS; i++) {
      key_matrix[i].adc_filtered = matrix_analog_read(i);
    }
  }

  // 2. Initialize rest values to current live ADC readings
  for (uint32_t i = 0; i < NUM_KEYS; i++) {
    raw_boot_rest[i] = analog_read(i);
    last_raw_val[i] = raw_boot_rest[i];
    key_matrix[i].adc_rest_value = key_matrix[i].adc_filtered;
    key_matrix[i].distance = 0;
    key_matrix[i].extremum = 0;
    key_matrix[i].key_dir = KEY_DIR_INACTIVE;
    key_matrix[i].is_pressed = false;
    stable_timer[i] = timer_read();
    is_moving[i] = false;
    locked_dist[i] = 0;
    last_motion_anchor[i] = 0;
    stationary_count[i] = 0;
  }

  // 3. Track resting noise during calibration duration
  const uint32_t calibration_start = timer_read();
  while (timer_elapsed(calibration_start) < MATRIX_CALIBRATION_DURATION) {
    analog_task();

    for (uint32_t i = 0; i < NUM_KEYS; i++) {
      const uint16_t new_adc_filtered =
          EMA(matrix_analog_read(i), key_matrix[i].adc_filtered);

      key_matrix[i].adc_filtered = new_adc_filtered;

      if (new_adc_filtered < key_matrix[i].adc_rest_value) {
        key_matrix[i].adc_rest_value = new_adc_filtered;
      }
    }
  }

  // 4. Update initial_rest_value in EEPROM to average of actual rest values
  uint32_t rest_sum = 0;
  for (uint32_t i = 0; i < NUM_KEYS; i++) {
    rest_sum += key_matrix[i].adc_rest_value;
  }
  uint16_t avg_rest = (uint16_t)(rest_sum / NUM_KEYS);
  if (avg_rest > 0) {
    eeconfig_calibration_t calib = eeconfig->calibration;
    calib.initial_rest_value = avg_rest;
    EECONFIG_WRITE(calibration, &calib);
  }

  // 5. Update bottom out values and lenience using the fresh rest values
  for (uint32_t i = 0; i < NUM_KEYS; i++) {
    raw_boot_rest[i] = analog_read(i);
    key_matrix[i].adc_bottom_out_value =
        matrix_bottom_out_value(i, key_matrix[i].adc_rest_value);
    matrix_update_lenience(i);
  }

  // 6. Update cached hysteresis gap
  matrix_update_calibration();
}

void matrix_start_manual_calibration(const uint8_t *keys, uint8_t count) {
  matrix_recalibrate(false);
  manual_calib_active = true;
  for (uint32_t i = 0; i < NUM_KEYS; i++) {
    bool target = (count == 0);
    if (!target && keys != NULL) {
      for (uint8_t k = 0; k < count; k++) {
        if (keys[k] == i) {
          target = true;
          break;
        }
      }
    }
    if (target) {
      manual_calib_status[i] = CALIB_STATE_WAITING;
      manual_calib_dir[i] = 0;
      manual_calib_peak[i] = key_matrix[i].adc_rest_value;
    } else {
      manual_calib_status[i] = CALIB_STATE_IDLE;
      manual_calib_dir[i] = 0;
    }
  }
}

void matrix_finish_manual_calibration(bool save) {
  if (save) {
    uint16_t bottom_out_threshold[NUM_KEYS];
    uint32_t delta_sum = 0;
    uint32_t delta_count = 0;
    for (uint32_t i = 0; i < NUM_KEYS; i++) {
      bottom_out_threshold[i] = eeconfig->bottom_out_threshold[i];
      if (manual_calib_status[i] == CALIB_STATE_COMPLETED ||
          manual_calib_status[i] == CALIB_STATE_RECORDING) {
        if (manual_calib_peak[i] > key_matrix[i].adc_rest_value + 50) {
          uint16_t delta = manual_calib_peak[i] - key_matrix[i].adc_rest_value;
          if (bitmap_get(key_inverted, i)) {
            delta |= BOTTOM_OUT_POLARITY_INVERTED;
          }
          bottom_out_threshold[i] = delta;
          delta_sum += (delta & BOTTOM_OUT_THRESHOLD_MASK);
          delta_count++;
        }
      }
    }
    EECONFIG_WRITE(bottom_out_threshold, bottom_out_threshold);

    if (delta_count > 0) {
      eeconfig_calibration_t calib = eeconfig->calibration;
      calib.initial_bottom_out_threshold = (uint16_t)(delta_sum / delta_count);
      EECONFIG_WRITE(calibration, &calib);
    }
  }
  manual_calib_active = false;
  for (uint32_t i = 0; i < NUM_KEYS; i++) {
    manual_calib_status[i] = CALIB_STATE_IDLE;
    manual_calib_dir[i] = 0;
  }
}

uint8_t matrix_get_calibration_status(uint8_t key) {
  if (key < NUM_KEYS)
    return manual_calib_status[key];
  return CALIB_STATE_IDLE;
}

void matrix_scan(void) {
  const uint32_t now = timer_read();
  const uint32_t debounce_time = eeconfig->options.debounce_ms;


  // Only scan keys that are connected to analog inputs
  for (uint32_t i = 0; i < ADC_NUM_MUX_INPUTS + ADC_NUM_RAW_INPUTS; i++) {
    const uint16_t raw_current = analog_read(i);

    // Dynamic polarity auto-detection for uncalibrated keys
    if ((eeconfig->bottom_out_threshold[i] & BOTTOM_OUT_THRESHOLD_MASK) == 0 &&
        !manual_calib_active) {
      if (!bitmap_get(key_inverted, i)) {
        if (raw_current + 150 < raw_boot_rest[i]) {
          bitmap_set(key_inverted, i, true);
          key_matrix[i].adc_rest_value = ADC_MAX_VALUE - raw_boot_rest[i];
          key_matrix[i].adc_filtered = ADC_MAX_VALUE - raw_current;
          key_matrix[i].adc_bottom_out_value =
              matrix_bottom_out_value(i, key_matrix[i].adc_rest_value);
          matrix_update_lenience(i);
        }
      } else {
        if (raw_current > raw_boot_rest[i] + 150) {
          bitmap_set(key_inverted, i, false);
          key_matrix[i].adc_rest_value = raw_boot_rest[i];
          key_matrix[i].adc_filtered = raw_current;
          key_matrix[i].adc_bottom_out_value =
              matrix_bottom_out_value(i, key_matrix[i].adc_rest_value);
          matrix_update_lenience(i);
        }
      }
    }

    uint16_t raw_val = raw_current;
    if (bitmap_get(key_inverted, i)) {
      raw_val = ADC_MAX_VALUE - raw_val;
    }
#if defined(MATRIX_INVERT_ADC_VALUES)
    raw_val = ADC_MAX_VALUE - raw_val;
#endif
    const uint16_t prev_filtered = key_matrix[i].adc_filtered;
    const actuation_t *actuation = &CURRENT_PROFILE.actuation_map[i];

    // Run-length persistence filter: eliminate single-sample comparator toggle
    uint16_t effective_raw = prev_filtered;
    if (raw_val == last_raw_val[i] || abs((int32_t)raw_val - (int32_t)last_raw_val[i]) > 1) {
      effective_raw = raw_val;
    }
    last_raw_val[i] = raw_val;

    uint16_t new_adc_filtered = EMA(effective_raw, prev_filtered);
    key_matrix[i].adc_filtered = new_adc_filtered;

    // Stability baseline tracking (only when key is completely released and idle)
    int32_t diff = (int32_t)new_adc_filtered - (int32_t)prev_filtered;
    if (diff < -3 || diff > 3) {
      stable_timer[i] = now;
    } else {
      if (now - stable_timer[i] >= 15) {
        if (!key_matrix[i].is_pressed && key_matrix[i].distance == 0 &&
            !manual_calib_active &&
            new_adc_filtered < key_matrix[i].adc_rest_value) {
          key_matrix[i].adc_rest_value = new_adc_filtered;
          if (manual_calib_status[i] == CALIB_STATE_IDLE) {
            key_matrix[i].adc_bottom_out_value =
                matrix_bottom_out_value(i, key_matrix[i].adc_rest_value);
            matrix_update_lenience(i);
          }
        }
      }
    }

    if (manual_calib_active && manual_calib_status[i] != CALIB_STATE_IDLE) {
      if (manual_calib_status[i] == CALIB_STATE_WAITING) {
        if (new_adc_filtered > key_matrix[i].adc_rest_value + 60) {
          manual_calib_status[i] = CALIB_STATE_RECORDING;
          manual_calib_peak[i] = new_adc_filtered;
        } else if (new_adc_filtered + 60 < key_matrix[i].adc_rest_value) {
          // North-facing switch detected: invert immediately on initial press!
          bitmap_set(key_inverted, i, !bitmap_get(key_inverted, i));
          uint16_t old_rest = key_matrix[i].adc_rest_value;
          uint16_t old_val = new_adc_filtered;
          key_matrix[i].adc_rest_value = ADC_MAX_VALUE - old_rest;
          key_matrix[i].adc_filtered = ADC_MAX_VALUE - old_val;
          new_adc_filtered = key_matrix[i].adc_filtered;
          key_matrix[i].adc_bottom_out_value = new_adc_filtered;
          manual_calib_peak[i] = new_adc_filtered;
          matrix_update_lenience(i);
          manual_calib_status[i] = CALIB_STATE_RECORDING;
        }
      } else if (manual_calib_status[i] == CALIB_STATE_RECORDING) {
        if (new_adc_filtered > manual_calib_peak[i]) {
          manual_calib_peak[i] = new_adc_filtered;
          key_matrix[i].adc_bottom_out_value = manual_calib_peak[i];
          matrix_update_lenience(i);
        } else if (new_adc_filtered <= key_matrix[i].adc_rest_value + 30) {
          manual_calib_status[i] = CALIB_STATE_COMPLETED;
          key_matrix[i].adc_bottom_out_value = manual_calib_peak[i];
          matrix_update_lenience(i);
        }
      }
    } else if ((eeconfig->bottom_out_threshold[i] & BOTTOM_OUT_THRESHOLD_MASK) == 0) {
      // Dynamic auto-calibration running only when no static threshold is set
      if (new_adc_filtered >=
          key_matrix[i].adc_bottom_out_value + MATRIX_CALIBRATION_EPSILON) {
        key_matrix[i].adc_bottom_out_value = new_adc_filtered;
        matrix_update_lenience(i);
      }
    }

    const uint16_t gap = hyst_gap[i];
    const uint16_t raw_dist =
        adc_to_distance(new_adc_filtered,
                        key_matrix[i].adc_rest_value + key_matrix[i].adc_rest_lenience,
                        key_matrix[i].adc_bottom_out_value);
    const int32_t noise_thresh = gap >> 3;
    uint16_t dist = key_matrix[i].distance;

    if (raw_dist == 0) {
      is_moving[i] = false;
      locked_dist[i] = 0;
      last_motion_anchor[i] = 0;
      stationary_count[i] = 0;
      dist = 0;
      key_matrix[i].distance = 0;
    } else if (raw_dist == 10000) {
      is_moving[i] = false;
      locked_dist[i] = 10000;
      last_motion_anchor[i] = 10000;
      stationary_count[i] = 0;
      dist = 10000;
      key_matrix[i].distance = 10000;
    } else if (!is_moving[i]) {
      // STATIONARY: Output locked to eliminate analog noise flicker
      if (abs((int32_t)raw_dist - (int32_t)locked_dist[i]) > noise_thresh) {
        // Physical motion detected: unlock immediately
        is_moving[i] = true;
        last_motion_anchor[i] = raw_dist;
        stationary_count[i] = 0;
        dist = raw_dist;
        key_matrix[i].distance = dist;
      } else {
        dist = locked_dist[i];
        key_matrix[i].distance = dist;
      }
    } else {
      // MOVING: Zero deadband real-time pass-through
      dist = raw_dist;
      key_matrix[i].distance = dist;

      // Check if motion has stopped
      if (abs((int32_t)raw_dist - (int32_t)last_motion_anchor[i]) <= noise_thresh) {
        stationary_count[i]++;
        if (stationary_count[i] >= 30) {
          // Stationary for ~30 scan sweeps (~1ms) -> re-lock
          is_moving[i] = false;
          locked_dist[i] = raw_dist;
          dist = locked_dist[i];
          key_matrix[i].distance = dist;
        }
      } else {
        // Still moving -> update anchor and reset hold counter
        last_motion_anchor[i] = raw_dist;
        stationary_count[i] = 0;
      }
    }

    const uint16_t deact_point = (actuation->actuation_point > gap)
                                ? (actuation->actuation_point - gap)
                                : 0;

    bool next_pressed = key_matrix[i].is_pressed;

    if (bitmap_get(rapid_trigger_disabled, i) || (actuation->rt_down == 0)) {
      key_matrix[i].key_dir = KEY_DIR_INACTIVE;
      if (key_matrix[i].is_pressed) {
        if (dist <= deact_point) {
          next_pressed = false;
        }
      } else {
        if (dist >= actuation->actuation_point) {
          next_pressed = true;
        }
      }
    } else {
      const uint16_t top_dz = actuation->rt_deadzone_top;
      const uint16_t bot_dz = actuation->rt_deadzone_bottom;
      const uint16_t bot_limit = (bot_dz < 10000) ? (10000 - bot_dz) : 10000;

      if (dist <= top_dz) {
        key_matrix[i].extremum = dist;
        if (!key_matrix[i].is_pressed || dist <= deact_point) {
          key_matrix[i].key_dir = KEY_DIR_INACTIVE;
          next_pressed = false;
        }
      } else if (dist >= bot_limit) {
        key_matrix[i].extremum = dist;
        key_matrix[i].key_dir = KEY_DIR_DOWN;
        next_pressed = true;
      } else {
        const uint16_t reset_point =
            (actuation->continuous && top_dz < deact_point) ? top_dz : deact_point;
        const uint16_t rt_up =
            actuation->rt_up == 0 ? actuation->rt_down : actuation->rt_up;
        const uint16_t rt_active_min =
            (actuation->actuation_point + gap < 10000)
                ? (actuation->actuation_point + gap)
                : 10000;

        switch (key_matrix[i].key_dir) {
        case KEY_DIR_INACTIVE:
          if (dist >= actuation->actuation_point) {
            // Pressed down past actuation point
            key_matrix[i].extremum = dist;
            key_matrix[i].key_dir = KEY_DIR_DOWN;
            next_pressed = true;
          }
          break;

        case KEY_DIR_DOWN:
          if (dist <= deact_point) {
            // Released past deactuation point (hysteresis)
            key_matrix[i].extremum = dist;
            key_matrix[i].key_dir = KEY_DIR_INACTIVE;
            next_pressed = false;
          } else if (key_matrix[i].extremum >= rt_active_min &&
                     dist + rt_up < key_matrix[i].extremum) {
            // Released by Rapid Trigger (only once pressed past actuation + hyst_gap)
            key_matrix[i].extremum = dist;
            key_matrix[i].key_dir = KEY_DIR_UP;
            next_pressed = false;
          } else if (dist > key_matrix[i].extremum) {
            // Pressed down further
            key_matrix[i].extremum = dist;
          }
          break;

        case KEY_DIR_UP:
          if (dist <= reset_point) {
            // Released past reset/deactuation point
            key_matrix[i].extremum = dist;
            key_matrix[i].key_dir = KEY_DIR_INACTIVE;
            next_pressed = false;
          } else if (key_matrix[i].extremum + actuation->rt_down < dist &&
                     (actuation->continuous || dist >= actuation->actuation_point)) {
            // Re-pressed by Rapid Trigger
            key_matrix[i].extremum = dist;
            key_matrix[i].key_dir = KEY_DIR_DOWN;
            next_pressed = true;
          } else if (dist < key_matrix[i].extremum) {
            // Released further
            key_matrix[i].extremum = dist;
          }
          break;

        default:
          break;
        }
      }
    }

    // Lockout Debounce
    if (next_pressed != key_matrix[i].is_pressed) {
      if (now - key_matrix[i].last_state_change_time >= debounce_time) {
        key_matrix[i].is_pressed = next_pressed;
        key_matrix[i].last_state_change_time = now;
        matrix_state_changed = true;
      }
    }
  }
}

void matrix_disable_rapid_trigger(uint8_t key, bool disable) {
  bitmap_set(rapid_trigger_disabled, key, disable);
}

void matrix_trigger_virtual_key(uint8_t key, bool is_pressed) {
  if (key < NUM_KEYS) {
    if (key_matrix[key].is_pressed != is_pressed) {
      key_matrix[key].is_pressed = is_pressed;
      matrix_state_changed = true;
    }
  }
}

bool matrix_has_changed(void) {
  if (matrix_state_changed) {
    matrix_state_changed = false;
    return true;
  }
  return false;
}
