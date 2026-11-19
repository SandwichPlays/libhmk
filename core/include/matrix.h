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

#pragma once

#include "common.h"

//--------------------------------------------------------------------+
// Key Matrix Configuration
//--------------------------------------------------------------------+

#if !defined(MATRIX_CALIBRATION_DURATION)
// Duration of the calibration process in milliseconds
#define MATRIX_CALIBRATION_DURATION 500
#endif

#if !defined(MATRIX_EMA_ALPHA_NUMERATOR)
// Numerator of the alpha parameter (alpha = NUMERATOR / 16) of the exponential
// moving average (EMA) filter used to smooth ADC values.
// 2 = 2/16 (exponent 3), 3 = 3/16 (midpoint), 4 = 4/16 (exponent 2).
#define MATRIX_EMA_ALPHA_NUMERATOR 3
#endif

#if !defined(MATRIX_EMA_ALPHA_SHIFT)
#define MATRIX_EMA_ALPHA_SHIFT 4
#endif

#if !defined(MATRIX_EMA_STEP_BYPASS_THRESHOLD)
// Fast step-jump bypass threshold in ADC counts for EMA filter
#define MATRIX_EMA_STEP_BYPASS_THRESHOLD 30
#endif

#if defined(ADC_RESOLUTION) && ADC_RESOLUTION > 12
#define ADC_RES_SHIFT (ADC_RESOLUTION - 12)
#else
#define ADC_RES_SHIFT 0
#endif

// Scale a 16-bit ADC-domain constant down to the board's active ADC_RESOLUTION (ceiling >= 1)
#if defined(ADC_RESOLUTION) && ADC_RESOLUTION < 16
#define ADC_FROM_16BIT(val16)                                                  \
  ((uint16_t)(((uint32_t)(val16) + (1u << (16 - ADC_RESOLUTION)) - 1u) >>      \
              (16 - ADC_RESOLUTION)))
#else
#define ADC_FROM_16BIT(val16) ((uint16_t)(val16))
#endif

#if !defined(MATRIX_CALIBRATION_EPSILON)
// Minimum change in ADC values required to update the calibration values.
#define MATRIX_CALIBRATION_EPSILON (5u << ADC_RES_SHIFT)
#endif

#if !defined(MATRIX_CALIBRATION_MIN_SPAN)
// Minimum valid ADC travel span between rest and bottom-out
#define MATRIX_CALIBRATION_MIN_SPAN (150u << ADC_RES_SHIFT)
#endif

#if !defined(MATRIX_DEFAULT_BOTTOM_OUT_SPAN)
// Fallback travel span when saved eeconfig threshold is stale/below min span (600 @ 12-bit, 9600 @ 16-bit)
#define MATRIX_DEFAULT_BOTTOM_OUT_SPAN (600u << ADC_RES_SHIFT)
#endif

#if !defined(MATRIX_DEBOUNCE_MS)
// Lockout debounce duration in milliseconds to prevent double-clicking/chatter
#define MATRIX_DEBOUNCE_MS 2
#endif

// AE64 Pro directional slew-limited 2-stage IIR filter thresholds
#if !defined(MATRIX_ADC_STEP_LIMIT_LARGE)
#define MATRIX_ADC_STEP_LIMIT_LARGE ADC_FROM_16BIT(240u)
#endif

#if !defined(MATRIX_ADC_STEP_LIMIT_SMALL)
#define MATRIX_ADC_STEP_LIMIT_SMALL ADC_FROM_16BIT(60u)
#endif

#if !defined(MATRIX_ADC_SLEW_TIGHT_DEPTH)
#define MATRIX_ADC_SLEW_TIGHT_DEPTH ADC_FROM_16BIT(200u)
#endif

// AE64 Pro low-travel smoothing & baseline drift parameters
#if !defined(MATRIX_LOW_TRAVEL_SMOOTH_UM)
#define MATRIX_LOW_TRAVEL_SMOOTH_UM 100u
#endif

#if !defined(MATRIX_REST_WARMUP_MS)
// First 10 seconds after boot/recal use 5ms drift cadence, then 1000ms
#define MATRIX_REST_WARMUP_MS 10000u
#endif

#if !defined(MATRIX_REST_WARMUP_TICK_MS)
#define MATRIX_REST_WARMUP_TICK_MS 5u
#endif

#if !defined(MATRIX_REST_STEADY_TICK_MS)
#define MATRIX_REST_STEADY_TICK_MS 1000u
#endif

#if !defined(MATRIX_DRIFT_INHIBIT_SWEEPS)
#define MATRIX_DRIFT_INHIBIT_SWEEPS 100u
#endif

#if !defined(MATRIX_DRIFT_EMERGENCY_DELTA)
#define MATRIX_DRIFT_EMERGENCY_DELTA ADC_FROM_16BIT(1000u)
#endif

#if !defined(HALL_MAX_REST_UPWARD_DRIFT)
// Maximum toward-press thermal drift allowed above calibrated baseline
#define HALL_MAX_REST_UPWARD_DRIFT (25u << ADC_RES_SHIFT)
#endif

// AE64 Pro Rapid Trigger dual-domain ADC tracker & rebound guard parameters
#if !defined(MATRIX_RT_ADC_SNAP_DELTA)
#define MATRIX_RT_ADC_SNAP_DELTA ADC_FROM_16BIT(30u)
#endif

#if !defined(MATRIX_RT_ADC_SNAP_MARGIN)
#define MATRIX_RT_ADC_SNAP_MARGIN ADC_FROM_16BIT(7u)
#endif

#if !defined(MATRIX_RT_ADC_NOISE_GUARD_NORMAL)
#define MATRIX_RT_ADC_NOISE_GUARD_NORMAL ADC_FROM_16BIT(7u)
#endif

#if !defined(MATRIX_RT_ADC_NOISE_GUARD_TOP)
#define MATRIX_RT_ADC_NOISE_GUARD_TOP ADC_FROM_16BIT(12u)
#endif

#if !defined(MATRIX_RT_DRIFT_TICK_MS)
#define MATRIX_RT_DRIFT_TICK_MS 1000u
#endif

#if !defined(MATRIX_HYST_GAP)
// Default static hysteresis gap in micrometers (10 µm @ >12-bit like AE64 Pro, 20 µm @ 12-bit)
#if defined(ADC_RESOLUTION) && ADC_RESOLUTION > 12
#define MATRIX_HYST_GAP 10u
#else
#define MATRIX_HYST_GAP 20u
#endif
#endif

#if defined(ANALOG_KEY_COUNT)
#define MATRIX_NUM_ANALOG_KEYS ANALOG_KEY_COUNT
#elif ADC_NUM_MUX_INPUTS > 0
#define MATRIX_NUM_ANALOG_KEYS NUM_KEYS
#elif defined(ADC_NUM_RAW_INPUTS)
#define MATRIX_NUM_ANALOG_KEYS ADC_NUM_RAW_INPUTS
#else
#define MATRIX_NUM_ANALOG_KEYS NUM_KEYS
#endif



//--------------------------------------------------------------------+
// Key Matrix
//--------------------------------------------------------------------+

typedef enum {
  KEY_DIR_INACTIVE = 0,
  KEY_DIR_DOWN,
  KEY_DIR_UP,
} key_dir_t;

// Key state
typedef struct {
  // Filtered ADC value
  uint16_t adc_filtered;
  // ADC value when the key is fully released
  uint16_t adc_rest_value;
  // ADC value when the key is fully pressed
  uint16_t adc_bottom_out_value;

  // Key travel distance in micrometers (0.001mm)
  uint16_t distance;
  // Last extremum point of the key travel distance in micrometers (0.001mm)
  uint16_t extremum;
  // Current key travel direction
  uint8_t key_dir;
  // Whether the key is pressed
  bool is_pressed;
  // Timestamp of the last key state change for 0ms-latency lockout debounce
  uint32_t last_state_change_time;
} key_state_t;

// Key matrix
extern key_state_t key_matrix[NUM_KEYS];

typedef enum {
  CALIB_STATE_IDLE = 0,
  CALIB_STATE_WAITING,
  CALIB_STATE_RECORDING,
  CALIB_STATE_COMPLETED,
} key_calib_status_t;

//--------------------------------------------------------------------+
// Key Matrix API
//--------------------------------------------------------------------+

/**
 * @brief Initialize the key matrix module
 *
 * @return None
 */
void matrix_init(void);

/**
 * @brief Restart the calibration process
 *
 * This function will block until the calibration process is complete.
 *
 * @param reset_bottom_out_threshold Whether to reset the saved bottom-out
 * threshold as well
 *
 * @return None
 */
void matrix_recalibrate(bool reset_bottom_out_threshold);

/**
 * @brief Start manual press calibration mode for specified keys
 *
 * @param keys Array of key indices to calibrate (NULL for all keys)
 * @param count Number of keys in array (0 for all keys)
 */
void matrix_start_manual_calibration(const uint8_t *keys, uint8_t count);

/**
 * @brief Finish manual press calibration mode and option to save to EEPROM
 *
 * @param save Whether to save recorded thresholds to EEPROM
 */
void matrix_finish_manual_calibration(bool save);

/**
 * @brief Get current manual calibration status of a key
 *
 * @param key Key index
 * @return Status enum (CALIB_STATE_IDLE, WAITING, RECORDING, COMPLETED)
 */
uint8_t matrix_get_calibration_status(uint8_t key);

/**
 * @brief Update cached calibration data (e.g. switch travel hysteresis)
 *
 * @return None
 */
void matrix_update_calibration(void);

/**
 * @brief Get key switch travel in micrometers (0.001mm)
 *
 * @param key Key index
 * @return Switch travel in micrometers (e.g. 3600 for 3.6mm)
 */
uint16_t matrix_get_travel_um(uint8_t key);

/**
 * @brief Check if a key is connected to an analog Hall-effect sensor
 *
 * @param key Key index
 * @return true if analog Hall key, false if digital/encoder key
 */
bool matrix_is_analog_key(uint8_t key);

/**
 * @brief Check if a key has inverted polarity (North-facing magnet)
 *
 * @param key Key index
 * @return true if inverted, false otherwise
 */
bool matrix_is_key_inverted(uint8_t key);

/**
 * @brief Check if an analog key's calibration range is sane (not stuck at rails or below min span)
 *
 * @param key Key index
 * @return true if valid, false otherwise
 */
bool matrix_is_key_calibration_valid(uint8_t key);

/**
 * @brief Get the unfiltered raw ADC reading of a key
 *
 * @param key Key index
 * @return Raw ADC reading (polarity-corrected)
 */
uint16_t matrix_get_raw_adc(uint8_t key);

/**
 * @brief Update the key matrix to reflect the current state of the keys
 *
 * @return None
 */
void matrix_scan(void);

/**
 * @brief Disable Rapid Trigger of a key
 *
 * This function is used by the advanced keys that override the Rapid Trigger
 * settings e.g. Dynamic Keystroke.
 *
 * @return None
 */
void matrix_disable_rapid_trigger(uint8_t key, bool disable);

/**
 * @brief Trigger a virtual key
 *
 * @param key Key index
 * @param is_pressed Whether the key is pressed
 *
 * @return None
 */
void matrix_trigger_virtual_key(uint8_t key, bool is_pressed);

