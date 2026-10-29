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
static bool debounced_button_states[ENCODER_COUNT];
static uint32_t last_button_change_times[ENCODER_COUNT];

static gpio_pins_source_type get_pin_source(uint16_t pin_mask) {
  for (uint32_t i = 0; i < 16; i++) {
    if (pin_mask & (1 << i)) {
      return (gpio_pins_source_type)i;
    }
  }
  return GPIO_PINS_SOURCE0;
}

void encoder_init(void) {
  // Enable SCFG clock in case it is needed for pin muxing
  crm_periph_clock_enable(CRM_SCFG_PERIPH_CLOCK, TRUE);

  for (uint32_t i = 0; i < ENCODER_COUNT; i++) {
    tmr_type *tmr = encoder_timers[i];
    gpio_type *port_a = encoder_ports[i][0];
    gpio_type *port_b = encoder_ports[i][1];

    // Enable timer peripheral clock
    if (tmr == TMR1) crm_periph_clock_enable(CRM_TMR1_PERIPH_CLOCK, TRUE);
    else if (tmr == TMR2) crm_periph_clock_enable(CRM_TMR2_PERIPH_CLOCK, TRUE);
    else if (tmr == TMR3) crm_periph_clock_enable(CRM_TMR3_PERIPH_CLOCK, TRUE);
    else if (tmr == TMR4) crm_periph_clock_enable(CRM_TMR4_PERIPH_CLOCK, TRUE);

    // Enable GPIO clocks dynamically
    if (port_a == GPIOA || port_b == GPIOA) crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    if (port_a == GPIOB || port_b == GPIOB) crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    if (port_a == GPIOC || port_b == GPIOC) crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
    if (port_a == GPIOD || port_b == GPIOD) crm_periph_clock_enable(CRM_GPIOD_PERIPH_CLOCK, TRUE);
    if (port_a == GPIOF || port_b == GPIOF) crm_periph_clock_enable(CRM_GPIOF_PERIPH_CLOCK, TRUE);

    // Initialize GPIOs in MUX mode with pull-ups
    for (uint32_t j = 0; j < 2; j++) {
      gpio_init_type gpio_init_struct;
      gpio_default_para_init(&gpio_init_struct);
      gpio_init_struct.gpio_pins = encoder_pins[i][j];
      gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
      gpio_init_struct.gpio_pull = GPIO_PULL_UP;
      gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
      gpio_init(encoder_ports[i][j], &gpio_init_struct);

      // Configure alternate function (AF2 / GPIO_MUX_2 is correct for TMR3/4 on PC6/PC7 and PB6/PB7)
      gpio_pins_source_type pin_source = get_pin_source(encoder_pins[i][j]);
      gpio_pin_mux_config(encoder_ports[i][j], pin_source, GPIO_MUX_2);
    }

    // Initialize button
    if (encoder_button_ports[i] != NULL) {
      gpio_type *btn_port = encoder_button_ports[i];
      if (btn_port == GPIOA) crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
      if (btn_port == GPIOB) crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
      if (btn_port == GPIOC) crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
      if (btn_port == GPIOD) crm_periph_clock_enable(CRM_GPIOD_PERIPH_CLOCK, TRUE);
      if (btn_port == GPIOF) crm_periph_clock_enable(CRM_GPIOF_PERIPH_CLOCK, TRUE);

      gpio_init_type gpio_init_struct;
      gpio_default_para_init(&gpio_init_struct);
      gpio_init_struct.gpio_pins = encoder_button_pins[i];
      gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
      gpio_init_struct.gpio_pull = GPIO_PULL_UP;
      gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
      gpio_init(encoder_button_ports[i], &gpio_init_struct);
      
      debounced_button_states[i] = gpio_input_data_bit_read(encoder_button_ports[i], encoder_button_pins[i]);
      last_button_change_times[i] = timer_read();
    }

    // Initialize timer in encoder mode
    tmr_base_init(tmr, 0xFFFF, 0);
    tmr_cnt_dir_set(tmr, TMR_COUNT_UP);
    
    // Configure hardware quadrature decoding (both channels)
    tmr_encoder_mode_config(tmr, TMR_ENCODER_MODE_C, TMR_INPUT_RISING_EDGE, TMR_INPUT_RISING_EDGE);

    // Enable hardware digital filter on both channels to filter out mechanical contact bounce
    tmr->cm1_input_bit.c1df = 0x0F;
    tmr->cm1_input_bit.c2df = 0x0F;

    // Explicitly enable input capture channels
    tmr_channel_enable(tmr, TMR_SELECT_CHANNEL_1, TRUE);
    tmr_channel_enable(tmr, TMR_SELECT_CHANNEL_2, TRUE);
    
    // Start counting
    tmr_counter_enable(tmr, TRUE);

    last_encoder_values[i] = tmr_counter_value_get(tmr);
  }
}

static uint16_t encoder_pulse_queue[NUM_KEYS];
static uint32_t encoder_key_release_times[NUM_KEYS];
static uint32_t encoder_key_next_press_times[NUM_KEYS];

void encoder_task(void) {
  static uint32_t last_encoder_ms = 0;
  uint32_t now = timer_read();
  if (now == last_encoder_ms) {
    return;
  }
  last_encoder_ms = now;

  // 1. Accumulate rotation delta and enqueue pulses
  for (uint32_t i = 0; i < ENCODER_COUNT; i++) {
    tmr_type *tmr = encoder_timers[i];
    int16_t current_value = (int16_t)tmr_counter_value_get(tmr);
    int16_t delta = current_value - last_encoder_values[i];
    int16_t res = encoder_resolutions[i] > 0 ? encoder_resolutions[i] : 4;
    int16_t steps = delta / res;

    if (steps != 0) {
      last_encoder_values[i] += steps * res;
      if (steps > 0) {
        uint8_t key = encoder_keys[i][1]; // CW
        if (key < NUM_KEYS) {
          encoder_pulse_queue[key] += steps;
        }
      } else {
        uint8_t key = encoder_keys[i][0]; // CCW
        if (key < NUM_KEYS) {
          encoder_pulse_queue[key] += (uint16_t)(-steps);
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
    if (encoder_button_ports[i] != NULL) {
      bool current_pin_state = gpio_input_data_bit_read(encoder_button_ports[i], encoder_button_pins[i]);
      if (current_pin_state != debounced_button_states[i]) {
        if (now - last_button_change_times[i] >= 5) { // 5 ms debounce
          debounced_button_states[i] = current_pin_state;
          matrix_trigger_virtual_key(encoder_button_keys[i], !current_pin_state); // Low = Pressed
        }
      } else {
        last_button_change_times[i] = now;
      }
    }
  }
}

#endif
