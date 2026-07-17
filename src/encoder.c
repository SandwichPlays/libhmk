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
#include "matrix.h"

#if defined(ENCODER_ENABLE)

#include "at32f402_405.h"

static tmr_type *encoder_timers[] = ENCODER_TIMERS;
static gpio_type *encoder_ports[][2] = ENCODER_PORTS;
static const uint16_t encoder_pins[][2] = ENCODER_PINS;
static const uint8_t encoder_keys[][2] = ENCODER_KEYS;
static const uint8_t encoder_resolutions[] = ENCODER_RESOLUTIONS;
static gpio_type *encoder_button_ports[] = ENCODER_BUTTON_PORTS;
static const uint16_t encoder_button_pins[] = ENCODER_BUTTON_PINS;
static const uint8_t encoder_button_keys[] = ENCODER_BUTTON_KEYS;

static int16_t last_encoder_values[ENCODER_COUNT];
static bool last_button_states[ENCODER_COUNT];

void encoder_init(void) {
  for (uint32_t i = 0; i < ENCODER_COUNT; i++) {
    tmr_type *tmr = encoder_timers[i];

    // Enable timer and GPIO clocks
    if (tmr == TMR1) crm_periph_clock_enable(CRM_TMR1_PERIPH_CLOCK, TRUE);
    else if (tmr == TMR2) crm_periph_clock_enable(CRM_TMR2_PERIPH_CLOCK, TRUE);
    else if (tmr == TMR3) crm_periph_clock_enable(CRM_TMR3_PERIPH_CLOCK, TRUE);
    else if (tmr == TMR4) crm_periph_clock_enable(CRM_TMR4_PERIPH_CLOCK, TRUE);

    // Initialize GPIOs
    for (uint32_t j = 0; j < 2; j++) {
      gpio_init_type gpio_init_struct;
      gpio_default_para_init(&gpio_init_struct);
      gpio_init_struct.gpio_pins = encoder_pins[i][j];
      gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
      gpio_init_struct.gpio_pull = GPIO_PULL_UP;
      gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
      gpio_init(encoder_ports[i][j], &gpio_init_struct);

      // Configure muxing - This is chip specific, assuming TMR4 on PB6/PB7 for now
      // A more robust implementation would use a lookup table for muxing
      if (tmr == TMR4 && encoder_ports[i][j] == GPIOB) {
          gpio_pin_mux_config(GPIOB, (encoder_pins[i][j] == GPIO_PINS_6 ? GPIO_PINS_SOURCE6 : GPIO_PINS_SOURCE7), GPIO_MUX_2);
      } else if (tmr == TMR3 && encoder_ports[i][j] == GPIOC) {
          gpio_pin_mux_config(GPIOC, (encoder_pins[i][j] == GPIO_PINS_6 ? GPIO_PINS_SOURCE6 : GPIO_PINS_SOURCE7), GPIO_MUX_2);
      }
    }

    // Initialize button
    if (encoder_button_ports[i] != NULL) {
      gpio_init_type gpio_init_struct;
      gpio_default_para_init(&gpio_init_struct);
      gpio_init_struct.gpio_pins = encoder_button_pins[i];
      gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
      gpio_init_struct.gpio_pull = GPIO_PULL_UP;
      gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
      gpio_init(encoder_button_ports[i], &gpio_init_struct);
      last_button_states[i] = gpio_input_data_bit_read(encoder_button_ports[i], encoder_button_pins[i]);
    }

    // Initialize timer in encoder mode
    tmr_base_init(tmr, 0xFFFF, 0);
    tmr_cnt_dir_set(tmr, TMR_COUNT_UP);
    tmr_encoder_mode_config(tmr, TMR_ENCODER_MODE_C, TMR_INPUT_RISING_EDGE, TMR_INPUT_RISING_EDGE);
    tmr_counter_enable(tmr, TRUE);

    last_encoder_values[i] = tmr_counter_value_get(tmr);
  }
}

void encoder_task(void) {
  for (uint32_t i = 0; i < ENCODER_COUNT; i++) {
    tmr_type *tmr = encoder_timers[i];
    int16_t current_value = tmr_counter_value_get(tmr);
    int16_t delta = current_value - last_encoder_values[i];

    if (delta >= encoder_resolutions[i]) {
      matrix_trigger_virtual_key(encoder_keys[i][1], true);
      matrix_trigger_virtual_key(encoder_keys[i][1], false);
      last_encoder_values[i] = current_value;
    } else if (delta <= -encoder_resolutions[i]) {
      matrix_trigger_virtual_key(encoder_keys[i][0], true);
      matrix_trigger_virtual_key(encoder_keys[i][0], false);
      last_encoder_values[i] = current_value;
    }

    if (encoder_button_ports[i] != NULL) {
      bool current_button_state = gpio_input_data_bit_read(encoder_button_ports[i], encoder_button_pins[i]);
      if (current_button_state != last_button_states[i]) {
        matrix_trigger_virtual_key(encoder_button_keys[i], !current_button_state); // Low = Pressed
        last_button_states[i] = current_button_state;
      }
    }
  }
}

#endif
