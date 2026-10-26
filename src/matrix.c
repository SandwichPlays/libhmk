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
#include "scan_rate.h"

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
  if (threshold == 0) {
    threshold = eeconfig->calibration.initial_bottom_out_threshold;
  }
  return M_MIN((uint32_t)rest_value + threshold, ADC_MAX_VALUE);
}

key_state_t key_matrix[NUM_KEYS];

// Bitmap for tracking which keys have Rapid Trigger disabled
static bitmap_t rapid_trigger_disabled[] = MAKE_BITMAP(NUM_KEYS);

static bool manual_calib_active = false;
static uint8_t manual_calib_status[NUM_KEYS] = {0};
static uint16_t manual_calib_peak[NUM_KEYS] = {0};

static uint32_t stable_timer[NUM_KEYS] = {0};
static uint16_t stable_ref[NUM_KEYS] = {0};
static uint16_t switch_travel_um[NUM_KEYS] = {0};
static uint16_t raw_boot_rest[NUM_KEYS] = {0};
static uint16_t last_raw_val[NUM_KEYS] = {0};
static uint32_t press_cycle_start[NUM_KEYS] = {0};
static uint32_t release_cycle_start[NUM_KEYS] = {0};

void matrix_update_calibration(void) {
  for (uint32_t i = 0; i < NUM_KEYS; i++) {
    uint8_t travel_tenths = eeconfig->calibration.switch_travel[i];
    if (travel_tenths < 20 || travel_tenths > 50) {
      travel_tenths = 36;
    }
    switch_travel_um[i] = (uint16_t)travel_tenths * 100;
  }
}

uint16_t matrix_get_travel_um(uint8_t key) {
  if (key < NUM_KEYS) {
    return switch_travel_um[key] ? switch_travel_um[key] : 3600;
  }
  return 3600;
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
    stable_ref[i] = key_matrix[i].adc_filtered;
    key_matrix[i].adc_rest_value = key_matrix[i].adc_filtered;
    key_matrix[i].distance = 0;
    key_matrix[i].extremum = 0;
    key_matrix[i].key_dir = KEY_DIR_INACTIVE;
    key_matrix[i].is_pressed = false;
    press_cycle_start[i] = 0;
    release_cycle_start[i] = 0;
    stable_timer[i] = timer_read();
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

  // 5. Update bottom out values using the fresh rest values
  for (uint32_t i = 0; i < NUM_KEYS; i++) {
    raw_boot_rest[i] = analog_read(i);
    key_matrix[i].adc_bottom_out_value =
        matrix_bottom_out_value(i, key_matrix[i].adc_rest_value);
  }

  // 6. Update cached switch travel
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
      manual_calib_peak[i] = key_matrix[i].adc_rest_value;
    } else {
      manual_calib_status[i] = CALIB_STATE_IDLE;
    }
  }
}

void matrix_finish_manual_calibration(bool save) {
  if (save) {
    uint16_t bottom_out_threshold[NUM_KEYS];
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
        }
      }
    }
    EECONFIG_WRITE(bottom_out_threshold, bottom_out_threshold);
  }
  manual_calib_active = false;
  for (uint32_t i = 0; i < NUM_KEYS; i++) {
    manual_calib_status[i] = CALIB_STATE_IDLE;
  }
}

uint8_t matrix_get_calibration_status(uint8_t key) {
  if (key < NUM_KEYS)
    return manual_calib_status[key];
  return CALIB_STATE_IDLE;
}

uint16_t matrix_get_raw_adc(uint8_t key) {
  if (key < NUM_KEYS)
    return last_raw_val[key];
  return 0;
}

void matrix_scan(void) {
  const uint32_t now = timer_read();
  const uint32_t debounce_time = eeconfig->options.debounce_ms;
  const actuation_t *actuation_map = CURRENT_PROFILE.actuation_map;

  // Only scan keys that are connected to analog inputs
  for (uint32_t i = 0; i < MATRIX_NUM_ANALOG_KEYS; i++) {
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
        }
      } else {
        if (raw_current > raw_boot_rest[i] + 150) {
          bitmap_set(key_inverted, i, false);
          key_matrix[i].adc_rest_value = raw_boot_rest[i];
          key_matrix[i].adc_filtered = raw_current;
          key_matrix[i].adc_bottom_out_value =
              matrix_bottom_out_value(i, key_matrix[i].adc_rest_value);
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
    const actuation_t *actuation = &actuation_map[i];

    // Run-length persistence filter: eliminate single-sample comparator toggle
    uint16_t effective_raw = prev_filtered;
    if (raw_val == last_raw_val[i] || abs((int32_t)raw_val - (int32_t)last_raw_val[i]) > 1) {
      effective_raw = raw_val;
    }
    last_raw_val[i] = raw_val;

    uint16_t new_adc_filtered = EMA(effective_raw, prev_filtered);
    key_matrix[i].adc_filtered = new_adc_filtered;

    // Stability auto-baseline recovery (only when key reading drops below rest)
    if (key_matrix[i].is_pressed || manual_calib_active) {
      stable_ref[i] = new_adc_filtered;
      stable_timer[i] = now;
    } else {
      if (abs((int32_t)new_adc_filtered - (int32_t)stable_ref[i]) > 2) {
        stable_ref[i] = new_adc_filtered;
        stable_timer[i] = now;
      } else if (new_adc_filtered < key_matrix[i].adc_rest_value) {
        if (now - stable_timer[i] >= MATRIX_BASELINE_DOWN_TIME_MS) {
          key_matrix[i].adc_rest_value = new_adc_filtered;
          if (manual_calib_status[i] == CALIB_STATE_IDLE) {
            key_matrix[i].adc_bottom_out_value =
                matrix_bottom_out_value(i, key_matrix[i].adc_rest_value);
          }
          stable_timer[i] = now;
          stable_ref[i] = new_adc_filtered;
        }
      } else {
        stable_timer[i] = now;
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
          manual_calib_status[i] = CALIB_STATE_RECORDING;
        }
      } else if (manual_calib_status[i] == CALIB_STATE_RECORDING) {
        if (new_adc_filtered > manual_calib_peak[i]) {
          manual_calib_peak[i] = new_adc_filtered;
          key_matrix[i].adc_bottom_out_value = manual_calib_peak[i];
        } else if (new_adc_filtered <= key_matrix[i].adc_rest_value + 30) {
          manual_calib_status[i] = CALIB_STATE_COMPLETED;
          key_matrix[i].adc_bottom_out_value = manual_calib_peak[i];
        }
      }
    } else if ((eeconfig->bottom_out_threshold[i] & BOTTOM_OUT_THRESHOLD_MASK) == 0) {
      // Dynamic auto-calibration running only when no static threshold is set
      if (new_adc_filtered >=
          key_matrix[i].adc_bottom_out_value + MATRIX_CALIBRATION_EPSILON) {
        key_matrix[i].adc_bottom_out_value = new_adc_filtered;
      }
    }

    const uint16_t travel_um = switch_travel_um[i];
    const uint16_t raw_dist =
        adc_to_distance(new_adc_filtered,
                        key_matrix[i].adc_rest_value,
                        key_matrix[i].adc_bottom_out_value,
                        travel_um);
    uint16_t dist = key_matrix[i].distance;
    if (raw_dist == 0 || raw_dist == travel_um ||
        abs((int32_t)raw_dist - (int32_t)dist) > 3) {
      dist = raw_dist;
      key_matrix[i].distance = dist;
    }

    const uint16_t deact_point = (actuation->actuation_point > MATRIX_HYST_GAP)
                                ? (actuation->actuation_point - MATRIX_HYST_GAP)
                                : 0;

    const uint16_t sweep_us = scan_rate_get_sweep_period_us();
    const uint32_t sweep_cycles = (F_CPU / 1000000u) * sweep_us;
    const uint16_t raw_sensor_dist =
        adc_to_distance(raw_val,
                        key_matrix[i].adc_rest_value,
                        key_matrix[i].adc_bottom_out_value,
                        travel_um);

    const uint16_t top_dz = actuation->rt_deadzone_top;
    const uint16_t bot_dz = actuation->rt_deadzone_bottom;
    const uint16_t bot_limit = (bot_dz < travel_um) ? (travel_um - bot_dz) : travel_um;
    const uint16_t rt_up = actuation->rt_up == 0 ? actuation->rt_down : actuation->rt_up;
    const uint16_t rt_down = actuation->rt_down;
    const uint16_t release_floor = actuation->continuous ? top_dz : deact_point;
    const uint16_t press_min = actuation->continuous ? top_dz : actuation->actuation_point;

    if (!key_matrix[i].is_pressed) {
      bool would_press = false;
      if (bitmap_get(rapid_trigger_disabled, i) || (rt_down == 0)) {
        would_press = (raw_sensor_dist >= actuation->actuation_point);
      } else {
        would_press = (raw_sensor_dist > top_dz &&
                       raw_sensor_dist >= press_min &&
                       (raw_sensor_dist >= key_matrix[i].extremum + rt_down));
      }
      if (would_press) {
        if (press_cycle_start[i] == 0) {
          uint32_t cur = board_cycle_count();
          press_cycle_start[i] = (cur >= sweep_cycles) ? (cur - sweep_cycles) : cur;
        }
      } else {
        press_cycle_start[i] = 0;
      }
    } else {
      bool would_release = false;
      if (bitmap_get(rapid_trigger_disabled, i) || (rt_down == 0)) {
        would_release = (raw_sensor_dist <= deact_point);
      } else {
        would_release = (raw_sensor_dist <= release_floor ||
                         (raw_sensor_dist + rt_up <= key_matrix[i].extremum));
      }
      if (would_release) {
        if (release_cycle_start[i] == 0) {
          uint32_t cur = board_cycle_count();
          release_cycle_start[i] = (cur >= sweep_cycles) ? (cur - sweep_cycles) : cur;
        }
      } else {
        release_cycle_start[i] = 0;
      }
    }

    bool next_pressed = key_matrix[i].is_pressed;

    if (bitmap_get(rapid_trigger_disabled, i) || (rt_down == 0)) {
      // Classic static actuation mode
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
      key_matrix[i].extremum = dist;
    } else {
      // Pure Asymmetric Rapid Trigger State Machine
      if (dist >= bot_limit) {
        // Bottom deadzone: lock to pressed and peak
        key_matrix[i].extremum = dist;
        key_matrix[i].key_dir = KEY_DIR_DOWN;
        next_pressed = true;
      } else if (key_matrix[i].is_pressed) {
        // --- IN PRESSED STATE: Listen ONLY for upward retreat ---
        if (dist > key_matrix[i].extremum) {
          // Push local peak deeper
          key_matrix[i].extremum = dist;
        } else if (dist <= release_floor || (key_matrix[i].extremum - dist >= rt_up)) {
          // Retreat exceeded release sensitivity or reached floor -> RELEASE
          key_matrix[i].extremum = dist; // Lock valley at release point
          key_matrix[i].key_dir = KEY_DIR_UP;
          next_pressed = false;
        }
      } else {
        // --- IN RELEASED STATE: Listen ONLY for downward plunge ---
        if (dist < key_matrix[i].extremum) {
          // Pull local valley higher
          key_matrix[i].extremum = dist;
        } else if (dist > top_dz && dist >= press_min &&
                   (dist - key_matrix[i].extremum >= rt_down)) {
          // Plunge exceeded press sensitivity -> PRESS
          key_matrix[i].extremum = dist; // Lock peak at press point
          key_matrix[i].key_dir = KEY_DIR_DOWN;
          next_pressed = true;
        }
      }
    }

    // Lockout Debounce (only applies to press)
    if (next_pressed != key_matrix[i].is_pressed) {
      if (!next_pressed || now - key_matrix[i].last_state_change_time >= debounce_time) {
        key_matrix[i].is_pressed = next_pressed;
        key_matrix[i].last_state_change_time = now;

        const uint32_t cur = board_cycle_count();
        if (next_pressed) {
          if (press_cycle_start[i] != 0 && cur >= press_cycle_start[i]) {
            uint32_t us = (cur - press_cycle_start[i]) / (F_CPU / 1000000u);
            scan_rate_record_press_latency((uint16_t)M_MIN(us, UINT16_MAX));
          }
          press_cycle_start[i] = 0;
        } else {
          if (release_cycle_start[i] != 0 && cur >= release_cycle_start[i]) {
            uint32_t us = (cur - release_cycle_start[i]) / (F_CPU / 1000000u);
            scan_rate_record_release_latency((uint16_t)M_MIN(us, UINT16_MAX));
          }
          release_cycle_start[i] = 0;
        }
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
    }
  }
}

