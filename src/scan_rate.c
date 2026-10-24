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

#include "scan_rate.h"

#include "hardware/hardware.h"

#define CYCLES_PER_US (F_CPU / 1000000u)
#define WINDOW_CYCLES ((F_CPU / 1000u) * SCAN_RATE_WINDOW_MS)

static uint32_t window_start_cycles;
static uint32_t window_start_sweeps;
static uint32_t last_iteration_cycles;
static uint32_t iterations;
static uint32_t min_cycles;
static uint32_t max_cycles;

static uint32_t eval_rate_hz;
static uint32_t sweep_rate_hz;
static uint16_t sweep_period_us;
static uint16_t eval_min_us;
static uint16_t eval_max_us;
static uint16_t eval_avg_us;

static void scan_rate_reset_window(uint32_t now) {
  window_start_cycles = now;
  window_start_sweeps = analog_get_sweep_count();
  last_iteration_cycles = now;
  iterations = 0;
  min_cycles = UINT32_MAX;
  max_cycles = 0;
}

static uint32_t active_until_ms = 0;

void scan_rate_init(void) {
  active_until_ms = 0;
  eval_rate_hz = 0;
  sweep_rate_hz = 0;
  sweep_period_us = 0;
  eval_min_us = 0;
  eval_max_us = 0;
  eval_avg_us = 0;
  scan_rate_reset_window(board_cycle_count());
}

void scan_rate_task(void) {
  if (timer_read() >= active_until_ms) {
    return;
  }

  const uint32_t now = board_cycle_count();
  const uint32_t elapsed = now - last_iteration_cycles;
  last_iteration_cycles = now;

  if (iterations > 0) {
    if (elapsed < min_cycles)
      min_cycles = elapsed;
    if (elapsed > max_cycles)
      max_cycles = elapsed;
  }
  iterations++;

  const uint32_t window = now - window_start_cycles;
  if (window < WINDOW_CYCLES)
    return;

  eval_rate_hz = (uint32_t)(((uint64_t)iterations * F_CPU) / window);

  const uint32_t current_sweeps = analog_get_sweep_count();
  const uint32_t sweeps = current_sweeps - window_start_sweeps;
  sweep_rate_hz = (uint32_t)(((uint64_t)sweeps * F_CPU) / window);
  sweep_period_us = sweep_rate_hz > 0 ? (uint16_t)(1000000u / sweep_rate_hz) : 0;

  eval_min_us =
      (uint16_t)(min_cycles == UINT32_MAX
                     ? 0
                     : M_MIN(min_cycles / CYCLES_PER_US, UINT16_MAX));
  eval_max_us = (uint16_t)M_MIN(max_cycles / CYCLES_PER_US, UINT16_MAX);
  eval_avg_us =
      (uint16_t)(iterations > 0
                     ? M_MIN(window / (iterations * CYCLES_PER_US), UINT16_MAX)
                     : 0);

  scan_rate_reset_window(now);
}

static volatile uint16_t latest_press_latency_us = 0;
static volatile uint16_t latest_release_latency_us = 0;

void scan_rate_get(scan_rate_t *out) {
  active_until_ms = timer_read() + 2000;
  out->eval_rate_hz = eval_rate_hz;
  out->sweep_rate_hz = sweep_rate_hz;
  out->sweep_period_us = sweep_period_us;
  out->eval_min_us = eval_min_us;
  out->eval_max_us = eval_max_us;
  out->eval_avg_us = eval_avg_us;
  out->press_latency_us = latest_press_latency_us;
  out->release_latency_us = latest_release_latency_us;
}

uint16_t scan_rate_get_sweep_period_us(void) {
  return sweep_period_us > 0 ? sweep_period_us : 33;
}

void scan_rate_record_press_latency(uint16_t latency_us) {
  latest_press_latency_us = latency_us;
}

void scan_rate_record_release_latency(uint16_t latency_us) {
  latest_release_latency_us = latency_us;
}
