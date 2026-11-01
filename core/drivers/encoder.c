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

#include "encoder.h"
#include "hardware/hardware.h"
#include "keyboard_hooks.h"
#include "matrix.h"

#if defined(ENCODER_ENABLE)

static const uint8_t encoder_keys[][2] = ENCODER_KEYS;
static const uint8_t encoder_resolutions[] = ENCODER_RESOLUTIONS;
static const uint8_t encoder_button_keys[] = ENCODER_BUTTON_KEYS;

static int16_t last_encoder_values[ENCODER_COUNT];
static bool debounced_button_states[ENCODER_COUNT];
static uint32_t last_button_change_times[ENCODER_COUNT];

static uint16_t encoder_pulse_queue[NUM_KEYS];
static uint32_t encoder_key_release_times[NUM_KEYS];
static uint32_t encoder_key_next_press_times[NUM_KEYS];

void encoder_init(void) {
  hardware_encoder_init();

  uint32_t now = timer_read();
  for (uint32_t i = 0; i < ENCODER_COUNT; i++) {
    last_encoder_values[i] = hardware_encoder_read(i);
    if (hardware_encoder_has_button(i)) {
      debounced_button_states[i] = hardware_encoder_button_read(i);
      last_button_change_times[i] = now;
    }
  }
}

void encoder_task(void) {
  // Poll GPIO quadrature state at full main-loop rate (~30 kHz+) for non-timer encoders
  hardware_encoder_poll();

  static uint32_t last_encoder_ms = 0;
  uint32_t now = timer_read();
  if (now == last_encoder_ms) {
    return;
  }
  last_encoder_ms = now;

  // 1. Accumulate rotation delta and enqueue pulses
  for (uint32_t i = 0; i < ENCODER_COUNT; i++) {
    int16_t current_value = hardware_encoder_read(i);
    int16_t delta = current_value - last_encoder_values[i];
    int16_t res = encoder_resolutions[i] > 0 ? encoder_resolutions[i] : 4;
    int16_t steps = delta / res;

    if (steps != 0) {
      last_encoder_values[i] += steps * res;
      if (steps > 0) {
        if (encoder_event_user((uint8_t)i, true)) {
          uint8_t key = encoder_keys[i][1]; // CW
          if (key < NUM_KEYS) {
            encoder_pulse_queue[key] += (uint16_t)steps;
          }
        }
      } else {
        if (encoder_event_user((uint8_t)i, false)) {
          uint8_t key = encoder_keys[i][0]; // CCW
          if (key < NUM_KEYS) {
            encoder_pulse_queue[key] += (uint16_t)(-steps);
          }
        }
      }
    }
  }

  // 2. Dispatch queued pulses with discrete press/release intervals
  for (uint32_t i = 0; i < ENCODER_COUNT; i++) {
    for (uint32_t j = 0; j < 2; j++) {
      const uint8_t k = encoder_keys[i][j];
      if (k >= NUM_KEYS) continue;

      if (encoder_key_release_times[k] != 0) {
        if (now >= encoder_key_release_times[k]) {
          matrix_trigger_virtual_key(k, false);
          encoder_key_release_times[k] = 0;
          encoder_key_next_press_times[k] = now + 4; // 4ms gap before next pulse
        }
      } else if (encoder_pulse_queue[k] > 0 && now >= encoder_key_next_press_times[k]) {
        encoder_pulse_queue[k]--;
        matrix_trigger_virtual_key(k, true);
        encoder_key_release_times[k] = now + 6; // 6ms pulse hold
      }
    }
  }

  // 3. Process encoder button with debounce
  for (uint32_t i = 0; i < ENCODER_COUNT; i++) {
    if (hardware_encoder_has_button(i)) {
      bool current_pin_state = hardware_encoder_button_read(i);
      if (current_pin_state != debounced_button_states[i]) {
        if (now - last_button_change_times[i] >= 5) { // 5 ms debounce
          debounced_button_states[i] = current_pin_state;
          matrix_trigger_virtual_key(encoder_button_keys[i], !current_pin_state);
        }
      } else {
        last_button_change_times[i] = now;
      }
    }
  }
}

#endif
