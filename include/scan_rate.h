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
// Scan Rate & Latency Measurement
//--------------------------------------------------------------------+

// Window the evaluation and sweep rates are averaged over (in ms)
#define SCAN_RATE_WINDOW_MS 250

typedef struct __attribute__((packed)) {
  // Main loop iterations per second (actuation scan rate)
  uint32_t eval_rate_hz;
  // Full analog sweeps per second (hardware ADC refresh rate)
  uint32_t sweep_rate_hz;
  // Sweep period in microseconds
  uint16_t sweep_period_us;
  // Minimum loop iteration latency in microseconds
  uint16_t eval_min_us;
  // Maximum loop iteration latency in microseconds
  uint16_t eval_max_us;
  // Average loop iteration latency in microseconds
  uint16_t eval_avg_us;
  // Last switch press latency in microseconds (from raw ADC actuation crossing to input finalization)
  uint16_t press_latency_us;
  // Last switch release / Rapid Trigger latency in microseconds
  uint16_t release_latency_us;
} scan_rate_t;

//--------------------------------------------------------------------+
// Scan Rate API
//--------------------------------------------------------------------+

/**
 * @brief Initialize the scan rate module
 */
void scan_rate_init(void);

/**
 * @brief Account for one pass of the scan loop
 */
void scan_rate_task(void);

/**
 * @brief Read the current scan rates and loop latencies
 *
 * @param out Buffer to write the rates to
 */
void scan_rate_get(scan_rate_t *out);

/**
 * @brief Get the current ADC sweep period in microseconds
 */
uint16_t scan_rate_get_sweep_period_us(void);

/**
 * @brief Record a key press latency event
 */
void scan_rate_record_press_latency(uint16_t latency_us);

/**
 * @brief Record a key release / Rapid Trigger latency event
 */
void scan_rate_record_release_latency(uint16_t latency_us);
