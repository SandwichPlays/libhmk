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

// Exponential moving average (EMA) filter
#define EMA(x, y)                                                              \
  (((uint32_t)(x) +                                                            \
    ((uint32_t)(y) * ((1 << MATRIX_EMA_ALPHA_EXPONENT) - 1))) >>               \
   MATRIX_EMA_ALPHA_EXPONENT)

__attribute__((always_inline)) static inline uint16_t
matrix_analog_read(uint8_t key) {
#if defined(MATRIX_INVERT_ADC_VALUES)
  return ADC_MAX_VALUE - analog_read(key);
#else
  return analog_read(key);
#endif
}

__attribute__((always_inline)) static inline uint16_t
matrix_bottom_out_value(uint8_t key, uint16_t rest_value) {
  return M_MIN(rest_value +
                   M_MAX(eeconfig->calibration.initial_bottom_out_threshold,
                         eeconfig->bottom_out_threshold[key]),
               ADC_MAX_VALUE);
}

key_state_t key_matrix[NUM_KEYS];

// Bitmap for tracking which keys have Rapid Trigger disabled
static bitmap_t rapid_trigger_disabled[] = MAKE_BITMAP(NUM_KEYS);

static bool manual_calib_active = false;
static uint8_t manual_calib_status[NUM_KEYS] = {0};
static uint16_t manual_calib_peak[NUM_KEYS] = {0};

void matrix_init(void) { matrix_recalibrate(false); }

void matrix_recalibrate(bool reset_bottom_out_threshold) {
  if (reset_bottom_out_threshold) {
    uint16_t bottom_out_threshold[NUM_KEYS] = {0};
    EECONFIG_WRITE(bottom_out_threshold, bottom_out_threshold);
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
    key_matrix[i].adc_rest_value = key_matrix[i].adc_filtered;
    key_matrix[i].distance = 0;
    key_matrix[i].extremum = 0;
    key_matrix[i].key_dir = KEY_DIR_INACTIVE;
    key_matrix[i].is_pressed = false;
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
    key_matrix[i].adc_bottom_out_value =
        matrix_bottom_out_value(i, key_matrix[i].adc_rest_value);
  }
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
    uint32_t delta_sum = 0;
    uint32_t delta_count = 0;
    for (uint32_t i = 0; i < NUM_KEYS; i++) {
      bottom_out_threshold[i] = eeconfig->bottom_out_threshold[i];
      if (manual_calib_status[i] == CALIB_STATE_COMPLETED ||
          manual_calib_status[i] == CALIB_STATE_RECORDING) {
        if (manual_calib_peak[i] > key_matrix[i].adc_rest_value + 50) {
          uint16_t delta = manual_calib_peak[i] - key_matrix[i].adc_rest_value;
          bottom_out_threshold[i] = delta;
          delta_sum += delta;
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
  }
}

uint8_t matrix_get_calibration_status(uint8_t key) {
  if (key < NUM_KEYS)
    return manual_calib_status[key];
  return CALIB_STATE_IDLE;
}

void matrix_scan(void) {
  // Only scan keys that are connected to analog inputs
  for (uint32_t i = 0; i < ADC_NUM_MUX_INPUTS + ADC_NUM_RAW_INPUTS; i++) {
    const uint16_t new_adc_filtered =
        EMA(matrix_analog_read(i), key_matrix[i].adc_filtered);
    const actuation_t *actuation = &CURRENT_PROFILE.actuation_map[i];

    key_matrix[i].adc_filtered = new_adc_filtered;

    if (new_adc_filtered < key_matrix[i].adc_rest_value) {
      key_matrix[i].adc_rest_value = new_adc_filtered;
      if (!manual_calib_active || manual_calib_status[i] == CALIB_STATE_IDLE) {
        key_matrix[i].adc_bottom_out_value =
            matrix_bottom_out_value(i, key_matrix[i].adc_rest_value);
      }
    }

    if (manual_calib_active && manual_calib_status[i] != CALIB_STATE_IDLE) {
      if (new_adc_filtered > key_matrix[i].adc_rest_value + 60) {
        manual_calib_status[i] = CALIB_STATE_RECORDING;
        if (new_adc_filtered > manual_calib_peak[i]) {
          manual_calib_peak[i] = new_adc_filtered;
        }
      } else if (manual_calib_status[i] == CALIB_STATE_RECORDING &&
                 new_adc_filtered <= key_matrix[i].adc_rest_value + 30) {
        manual_calib_status[i] = CALIB_STATE_COMPLETED;
        key_matrix[i].adc_bottom_out_value = manual_calib_peak[i];
      }
    } else if (eeconfig->bottom_out_threshold[i] == 0) {
      // Dynamic auto-calibration running only when no static threshold is set
      if (new_adc_filtered >=
          key_matrix[i].adc_bottom_out_value + MATRIX_CALIBRATION_EPSILON)
        key_matrix[i].adc_bottom_out_value = new_adc_filtered;
    }

    key_matrix[i].distance =
        adc_to_distance(new_adc_filtered, key_matrix[i].adc_rest_value,
                        key_matrix[i].adc_bottom_out_value);

    bool next_pressed = key_matrix[i].is_pressed;

    if (bitmap_get(rapid_trigger_disabled, i) | (actuation->rt_down == 0)) {
      key_matrix[i].key_dir = KEY_DIR_INACTIVE;
      next_pressed = (key_matrix[i].distance >= actuation->actuation_point);
    } else {
      const uint16_t dist = key_matrix[i].distance;
      const uint16_t top_dz = actuation->rt_deadzone_top;
      const uint16_t bot_dz = actuation->rt_deadzone_bottom;
      const uint16_t bot_limit = (bot_dz < 10000) ? (10000 - bot_dz) : 10000;

      if (dist <= top_dz) {
        key_matrix[i].extremum = dist;
        key_matrix[i].key_dir = KEY_DIR_INACTIVE;
        next_pressed = false;
      } else if (dist >= bot_limit) {
        key_matrix[i].extremum = dist;
        key_matrix[i].key_dir = KEY_DIR_DOWN;
        next_pressed = true;
      } else {
        const uint16_t reset_point =
            actuation->continuous ? 0 : actuation->actuation_point;
        const uint16_t rt_up =
            actuation->rt_up == 0 ? actuation->rt_down : actuation->rt_up;

        switch (key_matrix[i].key_dir) {
        case KEY_DIR_INACTIVE:
          if (key_matrix[i].distance > actuation->actuation_point) {
            // Pressed down past actuation point
            key_matrix[i].extremum = key_matrix[i].distance;
            key_matrix[i].key_dir = KEY_DIR_DOWN;
            next_pressed = true;
          }
          break;

        case KEY_DIR_DOWN:
          if (key_matrix[i].distance <= reset_point) {
            // Released past reset point
            key_matrix[i].extremum = key_matrix[i].distance;
            key_matrix[i].key_dir = KEY_DIR_INACTIVE;
            next_pressed = false;
          } else if (key_matrix[i].distance + rt_up < key_matrix[i].extremum) {
            // Released by Rapid Trigger
            key_matrix[i].extremum = key_matrix[i].distance;
            key_matrix[i].key_dir = KEY_DIR_UP;
            next_pressed = false;
          } else if (key_matrix[i].distance > key_matrix[i].extremum)
            // Pressed down further
            key_matrix[i].extremum = key_matrix[i].distance;
          break;

        case KEY_DIR_UP:
          if (key_matrix[i].distance <= reset_point) {
            // Released past reset point
            key_matrix[i].extremum = key_matrix[i].distance;
            key_matrix[i].key_dir = KEY_DIR_INACTIVE;
            next_pressed = false;
          } else if (key_matrix[i].extremum + actuation->rt_down <
                     key_matrix[i].distance) {
            // Pressed by Rapid Trigger
            key_matrix[i].extremum = key_matrix[i].distance;
            key_matrix[i].key_dir = KEY_DIR_DOWN;
            next_pressed = true;
          } else if (key_matrix[i].distance < key_matrix[i].extremum)
            // Released further
            key_matrix[i].extremum = key_matrix[i].distance;
          break;

        default:
          break;
        }
      }
    }

    // 0ms Latency Lockout Debounce: Instant key actuation on frame 0, 
    // with 2ms lockout window against electrical noise chatter.
    if (next_pressed != key_matrix[i].is_pressed) {
      if (timer_elapsed(key_matrix[i].last_state_change_time) >= eeconfig->options.debounce_ms) {
        key_matrix[i].is_pressed = next_pressed;
        key_matrix[i].last_state_change_time = timer_read();
      }
    }
  }
}

void matrix_disable_rapid_trigger(uint8_t key, bool disable) {
  bitmap_set(rapid_trigger_disabled, key, disable);
}

void matrix_trigger_virtual_key(uint8_t key, bool is_pressed) {
  if (key < NUM_KEYS) {
    key_matrix[key].is_pressed = is_pressed;
  }
}
