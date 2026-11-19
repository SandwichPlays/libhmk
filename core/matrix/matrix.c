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
#include "keyboard_hooks.h"
#include "lib/bitmap.h"
#include "scan_rate.h"

#if defined(ANALOG_KEY_LIST)
static const uint8_t analog_key_list[MATRIX_NUM_ANALOG_KEYS] = ANALOG_KEY_LIST;
#define ANALOG_KEY_AT(idx) (analog_key_list[(idx)])
#else
#define ANALOG_KEY_AT(idx) (idx)
#endif

#if defined(DIGITAL_ENABLE)
static const uint8_t digital_key_map[DIGITAL_KEY_COUNT] = DIGITAL_KEY_MAP;
static bool digital_raw_states[DIGITAL_KEY_COUNT];
static uint32_t digital_last_change[DIGITAL_KEY_COUNT];
#endif

// Per-key AE64-style RT & rebound guard flags
#define RT_FLAG_ARMED         (1u << 0)
#define RT_FLAG_PRESS_GUARD   (1u << 1)
#define RT_FLAG_RELEASE_GUARD (1u << 2)
#define RT_FLAG_BOTTOM_HIT    (1u << 3)
#define RT_FLAG_GUARD_MASK    (RT_FLAG_PRESS_GUARD | RT_FLAG_RELEASE_GUARD | RT_FLAG_BOTTOM_HIT)

// Bitmap for tracking which keys have inverted polarity (North-facing magnet)
static bitmap_t key_inverted[] = MAKE_BITMAP(NUM_KEYS);
// Bitmap for tracking analog keys whose rest/bottom-out span fails sanity validation
static bitmap_t key_calib_invalid[] = MAKE_BITMAP(NUM_KEYS);

bool matrix_is_analog_key(uint8_t key) {
  if (key >= NUM_KEYS)
    return false;
#if defined(ANALOG_KEY_LIST)
  for (uint32_t idx = 0; idx < MATRIX_NUM_ANALOG_KEYS; idx++) {
    if (analog_key_list[idx] == key)
      return true;
  }
  return false;
#else
  return key < MATRIX_NUM_ANALOG_KEYS;
#endif
}

bool matrix_is_key_inverted(uint8_t key) {
  if (key < NUM_KEYS)
    return bitmap_get(key_inverted, key);
  return false;
}

__attribute__((always_inline)) static inline uint16_t
matrix_analog_read(uint8_t key) {
  uint16_t val = analog_read(key);
  if (bitmap_get(key_inverted, key)) {
    val = (uint16_t)(ADC_MAX_VALUE - val);
  }
#if defined(MATRIX_INVERT_ADC_VALUES)
  val = (uint16_t)(ADC_MAX_VALUE - val);
#endif
  return val;
}

__attribute__((always_inline)) static inline uint16_t
matrix_bottom_out_value(uint8_t key, uint16_t rest_value) {
  uint16_t threshold =
      eeconfig->bottom_out_threshold[key] & BOTTOM_OUT_THRESHOLD_MASK;
  if (threshold < MATRIX_CALIBRATION_MIN_SPAN) {
    threshold = eeconfig->calibration.initial_bottom_out_threshold;
  }
  if (threshold < MATRIX_CALIBRATION_MIN_SPAN) {
    threshold = MATRIX_DEFAULT_BOTTOM_OUT_SPAN;
  }
  return (uint16_t)M_MIN((uint32_t)rest_value + threshold, (uint32_t)ADC_MAX_VALUE);
}

static inline bool matrix_validate_key_range(uint8_t key) {
  const uint16_t rest = key_matrix[key].adc_rest_value;
  const uint16_t bot = key_matrix[key].adc_bottom_out_value;
  return (rest > 0u && rest < ADC_MAX_VALUE &&
          bot >= (uint32_t)rest + MATRIX_CALIBRATION_MIN_SPAN);
}

bool matrix_is_key_calibration_valid(uint8_t key) {
  if (!matrix_is_analog_key(key))
    return true;
  return !bitmap_get(key_calib_invalid, key);
}

key_state_t key_matrix[NUM_KEYS];

// Bitmap for tracking which keys have Rapid Trigger disabled
static bitmap_t rapid_trigger_disabled[] = MAKE_BITMAP(NUM_KEYS);

static bool manual_calib_active = false;
static uint8_t manual_calib_status[NUM_KEYS] = {0};
static uint16_t manual_calib_peak[NUM_KEYS] = {0};

// AE64-style multi-stage fractional IIR filter & RT state
static uint32_t fast_adc_acc[NUM_KEYS] = {0};      // Stage-0 4x IIR (alpha=1/4)
static uint32_t iir_acc1[NUM_KEYS] = {0};          // Stage-1 8x IIR (alpha=1/8)
static uint32_t iir_acc2[NUM_KEYS] = {0};          // Stage-2 8x IIR (alpha=1/8)
static uint32_t rt_adc_iir_acc[NUM_KEYS] = {0};    // RT 256x ADC IIR (alpha=1/256)
static uint16_t low_travel_iir_acc[NUM_KEYS] = {0}; // Low-travel <=100um 16x IIR
static int8_t slew_dir_cnt[NUM_KEYS] = {0};

static uint16_t rt_peak_adc[NUM_KEYS] = {0};
static uint16_t rt_trough_adc[NUM_KEYS] = {0};
static uint8_t rt_flags[NUM_KEYS] = {0};
static uint8_t speed_detect_step[NUM_KEYS] = {0};
static uint32_t speed_detect_start_ms[NUM_KEYS] = {0};
static uint32_t guard_deadline_ms[NUM_KEYS] = {0};
static uint16_t raw_travel_um[NUM_KEYS] = {0};

// Global AE64-style baseline drift cadence & matrix-wide crosstalk tracking
static uint32_t boot_or_recal_ms = 0;
static uint32_t rest_drift_next_ms = 0;
static uint32_t rt_drift_next_ms = 0;
static bool drift_inhibit_req = false;
static uint8_t drift_inhibit_cnt = 0;
static uint16_t max_matrix_travel_um = 0;

static uint16_t base_cal_rest[NUM_KEYS] = {0};
static uint16_t switch_travel_um[NUM_KEYS] = {0};
static uint16_t raw_boot_rest[NUM_KEYS] = {0};
static uint16_t last_raw_val[NUM_KEYS] = {0};
static uint32_t press_cycle_start[NUM_KEYS] = {0};
static uint32_t release_cycle_start[NUM_KEYS] = {0};

static inline void matrix_reset_filter_state(uint32_t i, uint16_t val) {
  key_matrix[i].adc_filtered = val;
  fast_adc_acc[i] = (uint32_t)val << 2;
  iir_acc1[i] = (uint32_t)val << 3;
  iir_acc2[i] = (uint32_t)val << 3;
  rt_adc_iir_acc[i] = (uint32_t)val << 8;
  rt_peak_adc[i] = val;
  rt_trough_adc[i] = val;
  low_travel_iir_acc[i] = 0u;
  raw_travel_um[i] = 0u;
  slew_dir_cnt[i] = 0;
}

static void matrix_wait_adc_sweep(void) {
  const uint32_t prev = analog_get_sweep_count();
  const uint32_t start = timer_read();
  while (analog_get_sweep_count() == prev) {
    board_watchdog_feed();
    analog_task();
    if (timer_elapsed(start) >= 5u) {
      break;
    }
  }
}

void matrix_update_calibration(void) {
  for (uint32_t i = 0; i < NUM_KEYS; i++) {
    uint8_t travel_tenths = eeconfig->calibration.switch_travel[i];
    if (travel_tenths < 20 || travel_tenths > 50) {
      travel_tenths = 36;
    }
    switch_travel_um[i] = (uint16_t)travel_tenths * 100u;
  }
}

uint16_t matrix_get_travel_um(uint8_t key) {
  if (key < NUM_KEYS) {
    return switch_travel_um[key] ? switch_travel_um[key] : 3600u;
  }
  return 3600u;
}

void matrix_init(void) {
#if defined(DIGITAL_ENABLE)
  digital_gpio_init();
#endif
  matrix_recalibrate(false);
}

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

  // 1. Wait 8 initial ADC sweeps to let analog front-end settle (AE64 hall_boot_seed)
  for (uint32_t w = 0; w < 8u; w++) {
    matrix_wait_adc_sweep();
  }

  // 2. Seed all filter accumulators from live ADC readings
  for (uint32_t i = 0; i < NUM_KEYS; i++) {
    raw_boot_rest[i] = 0;
    last_raw_val[i] = 0;
    matrix_reset_filter_state(i, 0);
    key_matrix[i].adc_rest_value = 0;
    base_cal_rest[i] = 0;
    key_matrix[i].distance = 0;
    key_matrix[i].extremum = 0;
    key_matrix[i].key_dir = KEY_DIR_INACTIVE;
    key_matrix[i].is_pressed = false;
    rt_flags[i] = 0;
    speed_detect_step[i] = 0;
    speed_detect_start_ms[i] = 0;
    guard_deadline_ms[i] = 0;
    press_cycle_start[i] = 0;
    release_cycle_start[i] = 0;
  }
  for (uint32_t idx = 0; idx < MATRIX_NUM_ANALOG_KEYS; idx++) {
    const uint32_t i = ANALOG_KEY_AT(idx);
    const uint16_t raw = analog_read((uint8_t)i);
    const uint16_t val = matrix_analog_read((uint8_t)i);
    raw_boot_rest[i] = raw;
    last_raw_val[i] = val;
    matrix_reset_filter_state(i, val);
    key_matrix[i].adc_rest_value = val;
    base_cal_rest[i] = val;
  }

  // 3. Run 128 filter-seeding sweeps across Stage-0/1/2 IIRs (AE64 hall_boot_seed)
  for (uint32_t iter = 0; iter < 128u; iter++) {
    matrix_wait_adc_sweep();
    for (uint32_t idx = 0; idx < MATRIX_NUM_ANALOG_KEYS; idx++) {
      const uint32_t i = ANALOG_KEY_AT(idx);
      const uint16_t val = matrix_analog_read((uint8_t)i);
      fast_adc_acc[i] = fast_adc_acc[i] - (fast_adc_acc[i] >> 2) + val;
      const uint16_t pre = (uint16_t)(fast_adc_acc[i] >> 2);
      iir_acc1[i] = iir_acc1[i] - (iir_acc1[i] >> 3) + pre;
      iir_acc2[i] = iir_acc2[i] - (iir_acc2[i] >> 3) + (iir_acc1[i] >> 3);
      const uint16_t filt = (uint16_t)(iir_acc2[i] >> 3);
      key_matrix[i].adc_filtered = filt;
      key_matrix[i].adc_rest_value = filt;
      rt_adc_iir_acc[i] = (uint32_t)filt << 8;
      rt_peak_adc[i] = filt;
      rt_trough_adc[i] = filt;
    }
  }

  // 4. Track resting floor during remaining calibration duration
  const uint32_t calibration_start = timer_read();
  while (timer_elapsed(calibration_start) < MATRIX_CALIBRATION_DURATION) {
    matrix_wait_adc_sweep();
    for (uint32_t idx = 0; idx < MATRIX_NUM_ANALOG_KEYS; idx++) {
      const uint32_t i = ANALOG_KEY_AT(idx);
      const uint16_t val = matrix_analog_read((uint8_t)i);
      fast_adc_acc[i] = fast_adc_acc[i] - (fast_adc_acc[i] >> 2) + val;
      const uint16_t pre = (uint16_t)(fast_adc_acc[i] >> 2);
      iir_acc1[i] = iir_acc1[i] - (iir_acc1[i] >> 3) + pre;
      iir_acc2[i] = iir_acc2[i] - (iir_acc2[i] >> 3) + (iir_acc1[i] >> 3);
      const uint16_t filt = (uint16_t)(iir_acc2[i] >> 3);
      key_matrix[i].adc_filtered = filt;
      rt_adc_iir_acc[i] = (uint32_t)filt << 8;
      if (filt < key_matrix[i].adc_rest_value) {
        key_matrix[i].adc_rest_value = filt;
      }
    }
  }

  // 5. Update bottom-out values using fresh rest values and validate range
  for (uint32_t idx = 0; idx < MATRIX_NUM_ANALOG_KEYS; idx++) {
    const uint32_t i = ANALOG_KEY_AT(idx);
    raw_boot_rest[i] = analog_read((uint8_t)i);
    base_cal_rest[i] = key_matrix[i].adc_rest_value;
    rt_peak_adc[i] = key_matrix[i].adc_filtered;
    rt_trough_adc[i] = key_matrix[i].adc_filtered;
    key_matrix[i].adc_bottom_out_value =
        matrix_bottom_out_value((uint8_t)i, key_matrix[i].adc_rest_value);
    bitmap_set(key_calib_invalid, i, !matrix_validate_key_range((uint8_t)i));
  }

  boot_or_recal_ms = timer_read();
  rest_drift_next_ms = boot_or_recal_ms + MATRIX_REST_WARMUP_TICK_MS;
  rt_drift_next_ms = boot_or_recal_ms + MATRIX_RT_DRIFT_TICK_MS;
  drift_inhibit_req = false;
  drift_inhibit_cnt = 0;
  max_matrix_travel_um = 0;

  // 6. Update cached switch travel
  matrix_update_calibration();
}

void matrix_start_manual_calibration(const uint8_t *keys, uint8_t count) {
  matrix_recalibrate(false);
  manual_calib_active = true;
  for (uint32_t i = 0; i < NUM_KEYS; i++) {
    if (!matrix_is_analog_key((uint8_t)i)) {
      manual_calib_status[i] = CALIB_STATE_IDLE;
      continue;
    }
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
        if (manual_calib_peak[i] >=
            (uint32_t)key_matrix[i].adc_rest_value + MATRIX_CALIBRATION_MIN_SPAN) {
          uint16_t delta = manual_calib_peak[i] - key_matrix[i].adc_rest_value;
          if (delta > BOTTOM_OUT_THRESHOLD_MASK) {
            delta = BOTTOM_OUT_THRESHOLD_MASK;
          }
          if (bitmap_get(key_inverted, i)) {
            delta |= BOTTOM_OUT_POLARITY_INVERTED;
          }
          bottom_out_threshold[i] = delta;
        }
      }
    }
    EECONFIG_WRITE(bottom_out_threshold, bottom_out_threshold);
  }
  for (uint32_t idx = 0; idx < MATRIX_NUM_ANALOG_KEYS; idx++) {
    const uint32_t i = ANALOG_KEY_AT(idx);
    key_matrix[i].adc_bottom_out_value =
        matrix_bottom_out_value((uint8_t)i, key_matrix[i].adc_rest_value);
    bitmap_set(key_calib_invalid, i, !matrix_validate_key_range((uint8_t)i));
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

__attribute__((section(".ramfunc"))) void matrix_scan(void) {
  static uint32_t last_sweep_count = 0;
  const uint32_t current_sweep = analog_get_sweep_count();
  const bool new_sweep = (current_sweep != last_sweep_count);
  last_sweep_count = current_sweep;

  const uint32_t now = timer_read();
  const uint32_t debounce_time = eeconfig->options.debounce_ms;
  const actuation_t *actuation_map = CURRENT_PROFILE.actuation_map;
  const bool benchmark_active = scan_rate_is_active();

  // Global AE64-style drift inhibit & warmup-aware cadence ticks
  bool rest_drift_tick = false;
  bool rt_drift_tick = false;
  if (new_sweep) {
    if (drift_inhibit_req) {
      drift_inhibit_req = false;
      drift_inhibit_cnt = MATRIX_DRIFT_INHIBIT_SWEEPS;
    } else if (drift_inhibit_cnt > 0u) {
      drift_inhibit_cnt--;
    }

    const uint32_t rest_period =
        ((now - boot_or_recal_ms) < MATRIX_REST_WARMUP_MS)
            ? MATRIX_REST_WARMUP_TICK_MS
            : MATRIX_REST_STEADY_TICK_MS;
    if ((int32_t)(now - rest_drift_next_ms) >= 0) {
      rest_drift_next_ms = now + rest_period;
      rest_drift_tick = true;
    }
    if ((int32_t)(now - rt_drift_next_ms) >= 0) {
      rt_drift_next_ms = now + MATRIX_RT_DRIFT_TICK_MS;
      rt_drift_tick = true;
    }
  }

  uint16_t max_travel_seen = 0;

  // Only scan keys that are connected to analog inputs
  for (uint32_t idx = 0; idx < MATRIX_NUM_ANALOG_KEYS; idx++) {
    const uint32_t i = ANALOG_KEY_AT(idx);
    const actuation_t *actuation = &actuation_map[i];
    const uint16_t travel_um = switch_travel_um[i];
    uint16_t dist = key_matrix[i].distance;
    uint16_t raw_val = last_raw_val[i];
    uint16_t new_adc_filtered = key_matrix[i].adc_filtered;

    if (new_sweep) {
      const uint16_t raw_current = analog_read((uint8_t)i);

      // Dynamic polarity auto-detection for uncalibrated keys (scaled by ADC_RES_SHIFT)
      const uint16_t pol_delta = (uint16_t)(150u << ADC_RES_SHIFT);
      if ((eeconfig->bottom_out_threshold[i] & BOTTOM_OUT_THRESHOLD_MASK) == 0 &&
          !manual_calib_active) {
        if (!bitmap_get(key_inverted, i)) {
          if ((uint32_t)raw_current + pol_delta < raw_boot_rest[i]) {
            bitmap_set(key_inverted, i, true);
            const uint16_t inv_rest = (uint16_t)(ADC_MAX_VALUE - raw_boot_rest[i]);
            const uint16_t inv_cur = (uint16_t)(ADC_MAX_VALUE - raw_current);
            key_matrix[i].adc_rest_value = inv_rest;
            base_cal_rest[i] = inv_rest;
            matrix_reset_filter_state(i, inv_cur);
            key_matrix[i].adc_bottom_out_value =
                matrix_bottom_out_value((uint8_t)i, inv_rest);
          }
        } else {
          if (raw_current > (uint32_t)raw_boot_rest[i] + pol_delta) {
            bitmap_set(key_inverted, i, false);
            key_matrix[i].adc_rest_value = raw_boot_rest[i];
            base_cal_rest[i] = raw_boot_rest[i];
            matrix_reset_filter_state(i, raw_current);
            key_matrix[i].adc_bottom_out_value =
                matrix_bottom_out_value((uint8_t)i, raw_boot_rest[i]);
          }
        }
      }

      raw_val = raw_current;
      if (bitmap_get(key_inverted, i)) {
        raw_val = (uint16_t)(ADC_MAX_VALUE - raw_val);
      }
#if defined(MATRIX_INVERT_ADC_VALUES)
      raw_val = (uint16_t)(ADC_MAX_VALUE - raw_val);
#endif
      last_raw_val[i] = raw_val;

      const uint16_t prev_filtered = key_matrix[i].adc_filtered;
      const uint16_t prev_rest = key_matrix[i].adc_rest_value;
      const uint16_t prev_bot = key_matrix[i].adc_bottom_out_value;

      // Stage 0: Fast 1/4 IIR pre-filter (AE64 ilm_hdma_adc_filter_isr_body)
      fast_adc_acc[i] = fast_adc_acc[i] - (fast_adc_acc[i] >> 2) + raw_val;
      const uint16_t raw = (uint16_t)(fast_adc_acc[i] >> 2);

      // Stage 1 & 2: Adaptive directional slew-limited cascaded 2-stage 1/8 IIR (AE64)
      const uint16_t slew_lim =
          (raw >= (uint32_t)prev_rest + MATRIX_ADC_SLEW_TIGHT_DEPTH)
              ? MATRIX_ADC_STEP_LIMIT_SMALL
              : MATRIX_ADC_STEP_LIMIT_LARGE;

      if (raw > prev_filtered) {
        // Pressing direction
        const uint16_t diff = (uint16_t)(raw - prev_filtered);
        if (diff > (uint16_t)(slew_lim << 2) ||
            (diff > slew_lim && slew_dir_cnt[i] == 1)) {
          slew_dir_cnt[i] = 1;
          const uint16_t snap = (uint16_t)(raw - (slew_lim >> 2));
          new_adc_filtered = snap;
          iir_acc1[i] = (uint32_t)snap << 3;
          iir_acc2[i] = (uint32_t)snap << 3;
          rt_adc_iir_acc[i] = (uint32_t)snap << 8;
        } else {
          slew_dir_cnt[i] = (diff > slew_lim) ? (int8_t)(slew_dir_cnt[i] + 1) : 0;
          const uint16_t target = (uint16_t)(prev_filtered + (diff >> 2));
          iir_acc1[i] = iir_acc1[i] - (iir_acc1[i] >> 3) + target;
          iir_acc2[i] = iir_acc2[i] - (iir_acc2[i] >> 3) + (iir_acc1[i] >> 3);
          new_adc_filtered = (uint16_t)(iir_acc2[i] >> 3);
        }
      } else if (raw < prev_filtered) {
        // Releasing direction
        const uint16_t diff = (uint16_t)(prev_filtered - raw);
        if (diff > (uint16_t)(slew_lim << 2) ||
            (diff > slew_lim && slew_dir_cnt[i] == -1)) {
          slew_dir_cnt[i] = -1;
          const uint16_t snap = (uint16_t)(raw + (slew_lim >> 2));
          new_adc_filtered = snap;
          iir_acc1[i] = (uint32_t)snap << 3;
          iir_acc2[i] = (uint32_t)snap << 3;
          rt_adc_iir_acc[i] = (uint32_t)snap << 8;
          low_travel_iir_acc[i] = 0u;
        } else {
          slew_dir_cnt[i] = (diff > slew_lim) ? (int8_t)(slew_dir_cnt[i] - 1) : 0;
          const uint16_t target = (uint16_t)(prev_filtered - (diff >> 2));
          iir_acc1[i] = iir_acc1[i] - (iir_acc1[i] >> 3) + target;
          iir_acc2[i] = iir_acc2[i] - (iir_acc2[i] >> 3) + (iir_acc1[i] >> 3);
          new_adc_filtered = (uint16_t)(iir_acc2[i] >> 3);
        }
      } else {
        slew_dir_cnt[i] = 0;
        iir_acc1[i] = iir_acc1[i] - (iir_acc1[i] >> 3) + raw;
        iir_acc2[i] = iir_acc2[i] - (iir_acc2[i] >> 3) + (iir_acc1[i] >> 3);
        new_adc_filtered = (uint16_t)(iir_acc2[i] >> 3);
      }
      key_matrix[i].adc_filtered = new_adc_filtered;

      // Dynamic rest baseline tracking (AE64: global inhibit, 1/16 fast recovery, warmup/steady drift)
      if (!manual_calib_active) {
        const uint16_t rest = key_matrix[i].adc_rest_value;
        if (!drift_inhibit_req && drift_inhibit_cnt == 0u) {
          uint16_t new_rest = rest;
          if (new_adc_filtered < rest) {
            new_rest = (uint16_t)(((uint32_t)rest * 15u + new_adc_filtered) >> 4);
            if (new_rest < base_cal_rest[i]) {
              base_cal_rest[i] = new_rest;
            }
          } else if (rest_drift_tick && new_adc_filtered > rest) {
            const uint16_t max_rest = (uint16_t)M_MIN(
                (uint32_t)base_cal_rest[i] + HALL_MAX_REST_UPWARD_DRIFT,
                (uint32_t)ADC_MAX_VALUE);
            if (rest < max_rest) {
              new_rest = (uint16_t)(rest + 1u);
            }
          }
          if (new_rest != rest) {
            key_matrix[i].adc_rest_value = new_rest;
            if (manual_calib_status[i] == CALIB_STATE_IDLE) {
              key_matrix[i].adc_bottom_out_value =
                  matrix_bottom_out_value((uint8_t)i, new_rest);
            }
          }
        } else if (rest > (uint32_t)new_adc_filtered + MATRIX_DRIFT_EMERGENCY_DELTA) {
          const uint16_t new_rest =
              (uint16_t)(new_adc_filtered + MATRIX_DRIFT_EMERGENCY_DELTA);
          key_matrix[i].adc_rest_value = new_rest;
          if (manual_calib_status[i] == CALIB_STATE_IDLE) {
            key_matrix[i].adc_bottom_out_value =
                matrix_bottom_out_value((uint8_t)i, new_rest);
          }
        }
      }

      // Calibration (AE64 3/4 smoothed peak prevents single-sample EMI corruption)
      const uint16_t cal_start_delta = (uint16_t)(60u << ADC_RES_SHIFT);
      const uint16_t cal_rel_delta = (uint16_t)(30u << ADC_RES_SHIFT);
      if (manual_calib_active && manual_calib_status[i] != CALIB_STATE_IDLE) {
        if (manual_calib_status[i] == CALIB_STATE_WAITING) {
          if (new_adc_filtered > (uint32_t)key_matrix[i].adc_rest_value + cal_start_delta) {
            manual_calib_status[i] = CALIB_STATE_RECORDING;
            manual_calib_peak[i] = new_adc_filtered;
          } else if ((uint32_t)new_adc_filtered + cal_start_delta <
                     key_matrix[i].adc_rest_value) {
            // North-facing switch detected: invert immediately on initial press
            bitmap_set(key_inverted, i, !bitmap_get(key_inverted, i));
            const uint16_t old_rest = key_matrix[i].adc_rest_value;
            const uint16_t old_val = new_adc_filtered;
            const uint16_t inv_rest = (uint16_t)(ADC_MAX_VALUE - old_rest);
            const uint16_t inv_val = (uint16_t)(ADC_MAX_VALUE - old_val);
            key_matrix[i].adc_rest_value = inv_rest;
            base_cal_rest[i] = inv_rest;
            matrix_reset_filter_state(i, inv_val);
            new_adc_filtered = inv_val;
            key_matrix[i].adc_bottom_out_value = inv_val;
            manual_calib_peak[i] = inv_val;
            manual_calib_status[i] = CALIB_STATE_RECORDING;
          }
        } else if (manual_calib_status[i] == CALIB_STATE_RECORDING) {
          if (new_adc_filtered > manual_calib_peak[i]) {
            manual_calib_peak[i] = (uint16_t)(
                ((uint32_t)manual_calib_peak[i] * 3u + new_adc_filtered) >> 2);
            key_matrix[i].adc_bottom_out_value = manual_calib_peak[i];
          } else if (new_adc_filtered <=
                     (uint32_t)key_matrix[i].adc_rest_value + cal_rel_delta) {
            manual_calib_status[i] = CALIB_STATE_COMPLETED;
            base_cal_rest[i] = key_matrix[i].adc_rest_value;
            key_matrix[i].adc_bottom_out_value = manual_calib_peak[i];
          }
        }
      } else if ((eeconfig->bottom_out_threshold[i] & BOTTOM_OUT_THRESHOLD_MASK) == 0) {
        // Dynamic auto-calibration running only when no static threshold is set
        if (new_adc_filtered >=
            (uint32_t)key_matrix[i].adc_bottom_out_value + MATRIX_CALIBRATION_EPSILON) {
          key_matrix[i].adc_bottom_out_value = (uint16_t)(
              ((uint32_t)key_matrix[i].adc_bottom_out_value * 3u +
               new_adc_filtered) >>
              2);
        }
      }

      if (key_matrix[i].adc_rest_value != prev_rest ||
          key_matrix[i].adc_bottom_out_value != prev_bot) {
        bitmap_set(key_calib_invalid, i, !matrix_validate_key_range((uint8_t)i));
      }

      if (new_adc_filtered != prev_filtered ||
          key_matrix[i].adc_rest_value != prev_rest ||
          key_matrix[i].adc_bottom_out_value != prev_bot) {
        raw_travel_um[i] = adc_to_distance(new_adc_filtered,
                                           key_matrix[i].adc_rest_value,
                                           key_matrix[i].adc_bottom_out_value,
                                           travel_um);
      }
      const uint16_t raw_dist = raw_travel_um[i];

      // Stage 3: Low-travel (<= 100um) 1/16 smoother & 10-step velocity detector (AE64)
      if (raw_dist <= MATRIX_LOW_TRAVEL_SMOOTH_UM) {
        speed_detect_step[i] = 0u;
        rt_flags[i] &= (uint8_t)~RT_FLAG_BOTTOM_HIT;
        low_travel_iir_acc[i] = (uint16_t)(
            low_travel_iir_acc[i] - (low_travel_iir_acc[i] >> 4) + raw_dist);
        dist = (uint16_t)(low_travel_iir_acc[i] >> 4);
      } else {
        drift_inhibit_req = true;
        dist = raw_dist;
        if (dist > max_travel_seen) {
          max_travel_seen = dist;
        }
        switch (speed_detect_step[i]) {
        case 0u:
          if (dist > 999u) {
            speed_detect_start_ms[i] = now;
            speed_detect_step[i] = 1u;
          }
          break;
        case 1u:
          if (dist > 1099u) {
            speed_detect_step[i] =
                ((now - speed_detect_start_ms[i]) <= 49u) ? 0xFFu : 2u;
          }
          break;
        case 2u:
          if (dist > 1299u) {
            speed_detect_start_ms[i] = now;
            speed_detect_step[i] = 3u;
          }
          break;
        case 3u:
          if (dist > 1399u) {
            speed_detect_step[i] =
                ((now - speed_detect_start_ms[i]) <= 49u) ? 0xFFu : 4u;
          }
          break;
        case 4u:
          if (dist > 1699u) {
            speed_detect_start_ms[i] = now;
            speed_detect_step[i] = 5u;
          }
          break;
        case 5u:
          if (dist > 1799u) {
            speed_detect_step[i] =
                ((now - speed_detect_start_ms[i]) <= 49u) ? 0xFFu : 6u;
          }
          break;
        case 6u:
          if (dist > 2099u) {
            speed_detect_start_ms[i] = now;
            speed_detect_step[i] = 7u;
          }
          break;
        case 7u:
          if (dist > 2199u) {
            speed_detect_step[i] =
                ((now - speed_detect_start_ms[i]) <= 49u) ? 0xFFu : 8u;
          }
          break;
        case 8u:
          if (dist > 2699u) {
            speed_detect_start_ms[i] = now;
            speed_detect_step[i] = 9u;
          }
          break;
        case 9u:
          if (dist > 2799u) {
            speed_detect_step[i] =
                ((now - speed_detect_start_ms[i]) <= 49u) ? 0xFFu : 10u;
          }
          break;
        default:
          break;
        }
      }
      key_matrix[i].distance = dist;
    }

    const uint16_t top_dz = actuation->rt_deadzone_top;
    const uint16_t bot_dz = actuation->rt_deadzone_bottom;
    const uint16_t bot_limit = (bot_dz < travel_um) ? (uint16_t)(travel_um - bot_dz) : travel_um;
    const uint16_t rt_up = actuation->rt_up == 0u ? actuation->rt_down : actuation->rt_up;
    const uint16_t rt_down = actuation->rt_down;

    uint16_t base_act_pt = M_MAX(actuation->actuation_point, top_dz);
    if ((uint32_t)base_act_pt + 29u >= travel_um) {
      base_act_pt = (travel_um > 30u) ? (uint16_t)(travel_um - 30u) : 0u;
    }
    uint16_t deact_point = (base_act_pt > MATRIX_HYST_GAP)
                               ? (uint16_t)(base_act_pt - MATRIX_HYST_GAP)
                               : 0u;
    if (deact_point > bot_limit) {
      deact_point = bot_limit;
    }

    if (benchmark_active) {
      const uint16_t sweep_us = scan_rate_get_sweep_period_us();
      const uint32_t sweep_cycles = (F_CPU / 1000000u) * sweep_us;
      const uint16_t raw_sensor_dist =
          adc_to_distance(raw_val,
                          key_matrix[i].adc_rest_value,
                          key_matrix[i].adc_bottom_out_value,
                          travel_um);

      if (!key_matrix[i].is_pressed) {
        bool would_press = false;
        if (bitmap_get(rapid_trigger_disabled, i) || (rt_down == 0u)) {
          would_press = (raw_sensor_dist >= base_act_pt);
        } else {
          if (raw_sensor_dist >= base_act_pt &&
              (key_matrix[i].key_dir == KEY_DIR_INACTIVE ||
               key_matrix[i].extremum <= base_act_pt)) {
            would_press = true;
          } else if (raw_sensor_dist > top_dz &&
                     (raw_sensor_dist >= (uint32_t)key_matrix[i].extremum + rt_down) &&
                     (actuation->continuous || raw_sensor_dist >= base_act_pt)) {
            would_press = true;
          }
        }
        if (would_press) {
          if (press_cycle_start[i] == 0u) {
            uint32_t cur = board_cycle_count();
            press_cycle_start[i] = (cur >= sweep_cycles) ? (cur - sweep_cycles) : cur;
          }
        } else {
          press_cycle_start[i] = 0u;
        }
      } else {
        bool would_release = false;
        if (bitmap_get(rapid_trigger_disabled, i) || (rt_down == 0u)) {
          would_release = (raw_sensor_dist <= deact_point);
        } else {
          would_release = (raw_sensor_dist <= deact_point ||
                           (key_matrix[i].extremum > top_dz &&
                            (uint32_t)raw_sensor_dist + rt_up <= key_matrix[i].extremum));
        }
        if (would_release) {
          if (release_cycle_start[i] == 0u) {
            uint32_t cur = board_cycle_count();
            release_cycle_start[i] = (cur >= sweep_cycles) ? (cur - sweep_cycles) : cur;
          }
        } else {
          release_cycle_start[i] = 0u;
        }
      }
    }

    bool next_pressed = key_matrix[i].is_pressed;

    if (bitmap_get(key_calib_invalid, i)) {
      key_matrix[i].key_dir = KEY_DIR_INACTIVE;
      key_matrix[i].extremum = 0;
      rt_flags[i] = 0;
      next_pressed = false;
    } else if (bitmap_get(rapid_trigger_disabled, i) || (rt_down == 0u)) {
      // Classic static actuation mode with AE64 crosstalk compensation
      rt_flags[i] = 0;
      key_matrix[i].key_dir = KEY_DIR_INACTIVE;
      uint16_t eff_act_pt = base_act_pt;
      if (dist <= 400u) {
        eff_act_pt = (uint16_t)(eff_act_pt + (max_matrix_travel_um >> 5));
      }
      if (key_matrix[i].is_pressed) {
        if (dist <= deact_point) {
          next_pressed = false;
        }
      } else {
        if (dist >= eff_act_pt) {
          next_pressed = true;
        }
      }
      key_matrix[i].extremum = dist;
    } else {
      // AE64-style Dual-Domain Rapid Trigger Engine
      if (dist == 0u) {
        rt_flags[i] &= (uint8_t)~(RT_FLAG_ARMED | RT_FLAG_GUARD_MASK);
        key_matrix[i].key_dir = KEY_DIR_INACTIVE;
        key_matrix[i].extremum = 0;
        next_pressed = false;
      } else if (dist >= bot_limit) {
        // Bottom deadzone: stay pressed, arm RT, track bottom peak
        if ((rt_flags[i] & RT_FLAG_ARMED) == 0u) {
          rt_flags[i] |= RT_FLAG_ARMED;
          rt_adc_iir_acc[i] = (uint32_t)new_adc_filtered << 8;
          rt_peak_adc[i] = new_adc_filtered;
          rt_trough_adc[i] = new_adc_filtered;
        }
        if (speed_detect_step[i] == 0xFFu && (rt_flags[i] & RT_FLAG_BOTTOM_HIT) == 0u) {
          rt_flags[i] |= (RT_FLAG_RELEASE_GUARD | RT_FLAG_BOTTOM_HIT);
          guard_deadline_ms[i] = now + 20u;
        }
        key_matrix[i].extremum = dist;
        if (new_adc_filtered > rt_peak_adc[i]) {
          rt_peak_adc[i] = new_adc_filtered;
        }
        rt_trough_adc[i] = new_adc_filtered;
        key_matrix[i].key_dir = KEY_DIR_DOWN;
        next_pressed = true;
      } else {
        bool rt_armed = (rt_flags[i] & RT_FLAG_ARMED) != 0u;

        // Disarm RT when returning into top reset zone
        if (rt_armed &&
            ((!actuation->continuous && dist <= deact_point) ||
             (actuation->continuous && dist <= top_dz && dist <= deact_point))) {
          rt_flags[i] &= (uint8_t)~(RT_FLAG_ARMED | RT_FLAG_GUARD_MASK);
          key_matrix[i].key_dir = KEY_DIR_INACTIVE;
          key_matrix[i].extremum = dist;
          next_pressed = false;
          rt_armed = false;
        }

        if (!rt_armed) {
          // Pre-RT actuation with AE64 matrix-wide crosstalk offset
          uint16_t eff_act_pt = base_act_pt;
          if (dist <= 400u) {
            eff_act_pt = (uint16_t)(eff_act_pt + (max_matrix_travel_um >> 5));
          }
          if (!key_matrix[i].is_pressed) {
            if (dist >= eff_act_pt) {
              next_pressed = true;
            }
          } else if (dist <= deact_point) {
            next_pressed = false;
          }

          if (next_pressed) {
            rt_flags[i] |= RT_FLAG_ARMED;
            key_matrix[i].extremum = dist;
            key_matrix[i].key_dir = KEY_DIR_DOWN;
            rt_peak_adc[i] = new_adc_filtered;
            rt_trough_adc[i] = new_adc_filtered;
            rt_adc_iir_acc[i] = (uint32_t)new_adc_filtered << 8;
          } else {
            key_matrix[i].extremum = dist;
          }
        } else {
          // Active Rapid Trigger tracking: Stage-4 256x RT ADC IIR with +-30 snap
          uint16_t rt_adc = (uint16_t)(rt_adc_iir_acc[i] >> 8);
          if (new_sweep) {
            if ((int32_t)new_adc_filtered >
                (int32_t)rt_adc + (int32_t)MATRIX_RT_ADC_SNAP_DELTA) {
              rt_adc_iir_acc[i] =
                  (uint32_t)(new_adc_filtered - MATRIX_RT_ADC_SNAP_MARGIN) << 8;
            } else if ((int32_t)new_adc_filtered <
                       (int32_t)rt_adc - (int32_t)MATRIX_RT_ADC_SNAP_DELTA) {
              rt_adc_iir_acc[i] =
                  (uint32_t)(new_adc_filtered + MATRIX_RT_ADC_SNAP_MARGIN) << 8;
            } else {
              rt_adc_iir_acc[i] = rt_adc_iir_acc[i] - rt_adc + new_adc_filtered;
            }
            rt_adc = (uint16_t)(rt_adc_iir_acc[i] >> 8);
          }

          uint16_t press_sens = rt_down;
          uint16_t rel_sens = rt_up;
          if (dist < top_dz) {
            press_sens = (uint16_t)(press_sens + top_dz);
          }

          // Fast-stroke bottom-out release guard (+500um for 20ms)
          if (speed_detect_step[i] == 0xFFu &&
              (rt_flags[i] & RT_FLAG_BOTTOM_HIT) == 0u &&
              (uint32_t)dist + 99u >= travel_um) {
            rt_flags[i] |= (RT_FLAG_RELEASE_GUARD | RT_FLAG_BOTTOM_HIT);
            guard_deadline_ms[i] = now + 20u;
          }

          // Active post-release press guard (+500um) & post-bottom release guard (+500um)
          if (rt_flags[i] & RT_FLAG_PRESS_GUARD) {
            if ((int32_t)(guard_deadline_ms[i] - now) >= 0) {
              press_sens = (uint16_t)(press_sens + 500u);
            } else {
              rt_flags[i] &= (uint8_t)~RT_FLAG_PRESS_GUARD;
            }
          }
          if (rt_flags[i] & RT_FLAG_RELEASE_GUARD) {
            if ((int32_t)(guard_deadline_ms[i] - now) >= 0) {
              rel_sens = (uint16_t)(rel_sens + 500u);
            } else {
              rt_flags[i] &= (uint8_t)~RT_FLAG_RELEASE_GUARD;
            }
          }

          const uint16_t adc_noise_guard =
              (dist <= 49u) ? MATRIX_RT_ADC_NOISE_GUARD_TOP
                            : MATRIX_RT_ADC_NOISE_GUARD_NORMAL;

          if (key_matrix[i].is_pressed) {
            // Dual-condition RT release: distance retreat AND ADC drop past noise guard
            const bool release_hit =
                (key_matrix[i].extremum > top_dz) &&
                ((uint32_t)dist + rel_sens <= key_matrix[i].extremum) &&
                ((uint32_t)rt_adc + adc_noise_guard < rt_peak_adc[i]);
            if (release_hit) {
              next_pressed = false;
              key_matrix[i].key_dir = KEY_DIR_UP;
              key_matrix[i].extremum = dist;
              rt_trough_adc[i] = rt_adc;
              rt_peak_adc[i] = rt_adc;
              if (speed_detect_step[i] == 0xFFu && dist <= 499u) {
                rt_flags[i] = (uint8_t)((rt_flags[i] & ~RT_FLAG_GUARD_MASK) |
                                        RT_FLAG_PRESS_GUARD);
                guard_deadline_ms[i] = now + 15u;
              }
            }
          } else {
            // Dual-condition RT re-press: distance press AND ADC rise past noise guard
            const bool press_hit =
                (dist > top_dz) &&
                (actuation->continuous || dist >= base_act_pt) &&
                (dist >= (uint32_t)key_matrix[i].extremum + press_sens) &&
                (rt_adc > (uint32_t)rt_trough_adc[i] + adc_noise_guard);
            if (press_hit) {
              next_pressed = true;
              key_matrix[i].key_dir = KEY_DIR_DOWN;
              key_matrix[i].extremum = dist;
              rt_peak_adc[i] = rt_adc;
              rt_trough_adc[i] = rt_adc;
            }
          }

          // Update RT peak / trough extrema + 1 count/sec drift leak (AE64)
          if (next_pressed) {
            if (dist > key_matrix[i].extremum) {
              key_matrix[i].extremum = dist;
            }
            if (rt_adc > rt_peak_adc[i]) {
              rt_peak_adc[i] = rt_adc;
            }
            rt_trough_adc[i] = rt_adc;
            if (rt_drift_tick && rt_peak_adc[i] > 0u) {
              rt_peak_adc[i]--;
            }
          } else {
            if (dist < key_matrix[i].extremum) {
              key_matrix[i].extremum = dist;
            }
            if (rt_adc < rt_trough_adc[i]) {
              rt_trough_adc[i] = rt_adc;
            }
            rt_peak_adc[i] = rt_adc;
            if (rt_drift_tick && rt_trough_adc[i] < ADC_MAX_VALUE) {
              rt_trough_adc[i]++;
            }
          }
        }
      }
    }

    if (next_pressed) {
      drift_inhibit_req = true;
    }

    // Lockout Debounce (only applies to press)
    if (next_pressed != key_matrix[i].is_pressed) {
      if (!next_pressed || now - key_matrix[i].last_state_change_time >= debounce_time) {
        key_matrix[i].is_pressed = next_pressed;
        key_matrix[i].last_state_change_time = now;

        if (benchmark_active) {
          const uint32_t cur = board_cycle_count();
          if (next_pressed) {
            if (press_cycle_start[i] != 0u && cur >= press_cycle_start[i]) {
              uint32_t us = (cur - press_cycle_start[i]) / (F_CPU / 1000000u);
              scan_rate_record_press_latency((uint16_t)M_MIN(us, UINT16_MAX));
            }
            press_cycle_start[i] = 0u;
          } else {
            if (release_cycle_start[i] != 0u && cur >= release_cycle_start[i]) {
              uint32_t us = (cur - release_cycle_start[i]) / (F_CPU / 1000000u);
              scan_rate_record_release_latency((uint16_t)M_MIN(us, UINT16_MAX));
            }
            release_cycle_start[i] = 0u;
          }
        }
      }
    }
  }

  if (new_sweep) {
    max_matrix_travel_um = max_travel_seen;
  }

#if defined(DIGITAL_ENABLE)
  digital_gpio_scan(digital_raw_states);
  for (uint32_t d = 0; d < DIGITAL_KEY_COUNT; d++) {
    const uint8_t k = digital_key_map[d];
    if (k == 0 || k > NUM_KEYS) continue;
    const uint8_t key_idx = k - 1;
    const bool raw_pressed = digital_raw_states[d];
    if (raw_pressed != key_matrix[key_idx].is_pressed) {
      if (now - digital_last_change[d] >= DIGITAL_DEBOUNCE_MS) {
        key_matrix[key_idx].is_pressed = raw_pressed;
        key_matrix[key_idx].distance = raw_pressed ? switch_travel_um[key_idx] : 0;
        key_matrix[key_idx].last_state_change_time = now;
        digital_last_change[d] = now;
      }
    } else {
      digital_last_change[d] = now;
    }
  }
#endif

  matrix_scan_user();
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
