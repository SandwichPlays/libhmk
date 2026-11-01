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

#include "hardware/hardware.h"

#if defined(ENCODER_ENABLE)

#include "at32f402_405.h"

static tmr_type *encoder_timers[] = ENCODER_TIMERS;
static gpio_type *encoder_ports[][2] = ENCODER_PORTS;
static const uint16_t encoder_pins[][2] = ENCODER_PINS;
static const gpio_mux_sel_type encoder_pin_muxes[][2] = ENCODER_PIN_MUXES;
static gpio_type *encoder_button_ports[] = ENCODER_BUTTON_PORTS;
static const uint16_t encoder_button_pins[] = ENCODER_BUTTON_PINS;

// Polled GPIO quadrature state for encoders with timer == NULL
static int16_t polled_encoder_counts[ENCODER_COUNT];
static uint8_t polled_encoder_ab_state[ENCODER_COUNT];

// 4-bit Gray-code quadrature transition table: (prev_ab << 2) | curr_ab
static const int8_t quadrature_lut[16] = {
    0, -1,  1,  0,
    1,  0,  0, -1,
   -1,  0,  0,  1,
    0,  1, -1,  0,
};

static void enable_gpio_clock(gpio_type *port) {
  if (port == GPIOA) crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  else if (port == GPIOB) crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  else if (port == GPIOC) crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
  else if (port == GPIOD) crm_periph_clock_enable(CRM_GPIOD_PERIPH_CLOCK, TRUE);
  else if (port == GPIOF) crm_periph_clock_enable(CRM_GPIOF_PERIPH_CLOCK, TRUE);
}

static gpio_pins_source_type get_pin_source(uint16_t pin_mask) {
  for (uint32_t i = 0; i < 16; i++) {
    if (pin_mask & (1u << i)) {
      return (gpio_pins_source_type)i;
    }
  }
  return GPIO_PINS_SOURCE0;
}

void hardware_encoder_init(void) {
  crm_periph_clock_enable(CRM_SCFG_PERIPH_CLOCK, TRUE);

  for (uint32_t i = 0; i < ENCODER_COUNT; i++) {
    tmr_type *tmr = encoder_timers[i];
    enable_gpio_clock(encoder_ports[i][0]);
    enable_gpio_clock(encoder_ports[i][1]);

    if (tmr != NULL) {
      if (tmr == TMR1) crm_periph_clock_enable(CRM_TMR1_PERIPH_CLOCK, TRUE);
      else if (tmr == TMR2) crm_periph_clock_enable(CRM_TMR2_PERIPH_CLOCK, TRUE);
      else if (tmr == TMR3) crm_periph_clock_enable(CRM_TMR3_PERIPH_CLOCK, TRUE);
      else if (tmr == TMR4) crm_periph_clock_enable(CRM_TMR4_PERIPH_CLOCK, TRUE);

      for (uint32_t j = 0; j < 2; j++) {
        gpio_init_type gpio_init_struct;
        gpio_default_para_init(&gpio_init_struct);
        gpio_init_struct.gpio_pins = encoder_pins[i][j];
        gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
        gpio_init_struct.gpio_pull = GPIO_PULL_UP;
        gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
        gpio_init(encoder_ports[i][j], &gpio_init_struct);

        gpio_pins_source_type pin_source = get_pin_source(encoder_pins[i][j]);
        gpio_pin_mux_config(encoder_ports[i][j], pin_source,
                            encoder_pin_muxes[i][j]);
      }

      tmr_base_init(tmr, 0xFFFF, 0);
      tmr_cnt_dir_set(tmr, TMR_COUNT_UP);
      tmr_encoder_mode_config(tmr, TMR_ENCODER_MODE_C, TMR_INPUT_RISING_EDGE,
                              TMR_INPUT_RISING_EDGE);
      tmr->cm1_input_bit.c1df = 0x0F;
      tmr->cm1_input_bit.c2df = 0x0F;
      tmr_channel_enable(tmr, TMR_SELECT_CHANNEL_1, TRUE);
      tmr_channel_enable(tmr, TMR_SELECT_CHANNEL_2, TRUE);
      tmr_counter_enable(tmr, TRUE);
    } else {
      // Arbitrary GPIO pins: configure as pulled-up digital inputs for Gray-code polling
      for (uint32_t j = 0; j < 2; j++) {
        gpio_init_type gpio_init_struct;
        gpio_default_para_init(&gpio_init_struct);
        gpio_init_struct.gpio_pins = encoder_pins[i][j];
        gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
        gpio_init_struct.gpio_pull = GPIO_PULL_UP;
        gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
        gpio_init(encoder_ports[i][j], &gpio_init_struct);
      }
      uint8_t a = gpio_input_data_bit_read(encoder_ports[i][0], encoder_pins[i][0]) ? 1 : 0;
      uint8_t b = gpio_input_data_bit_read(encoder_ports[i][1], encoder_pins[i][1]) ? 1 : 0;
      polled_encoder_ab_state[i] = (uint8_t)((a << 1) | b);
      polled_encoder_counts[i] = 0;
    }

    if (encoder_button_ports[i] != NULL) {
      enable_gpio_clock(encoder_button_ports[i]);
      gpio_init_type gpio_init_struct;
      gpio_default_para_init(&gpio_init_struct);
      gpio_init_struct.gpio_pins = encoder_button_pins[i];
      gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
      gpio_init_struct.gpio_pull = GPIO_PULL_UP;
      gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
      gpio_init(encoder_button_ports[i], &gpio_init_struct);
    }
  }
}

void hardware_encoder_poll(void) {
  for (uint32_t i = 0; i < ENCODER_COUNT; i++) {
    if (encoder_timers[i] == NULL) {
      uint8_t a = gpio_input_data_bit_read(encoder_ports[i][0], encoder_pins[i][0]) ? 1 : 0;
      uint8_t b = gpio_input_data_bit_read(encoder_ports[i][1], encoder_pins[i][1]) ? 1 : 0;
      uint8_t curr = (uint8_t)((a << 1) | b);
      uint8_t idx = (uint8_t)(((polled_encoder_ab_state[i] & 0x03) << 2) | curr);
      polled_encoder_counts[i] += quadrature_lut[idx];
      polled_encoder_ab_state[i] = curr;
    }
  }
}

int16_t hardware_encoder_read(uint32_t index) {
  if (encoder_timers[index] != NULL) {
    return (int16_t)tmr_counter_value_get(encoder_timers[index]);
  }
  return polled_encoder_counts[index];
}

bool hardware_encoder_has_button(uint32_t index) {
  return encoder_button_ports[index] != NULL;
}

bool hardware_encoder_button_read(uint32_t index) {
  return gpio_input_data_bit_read(encoder_button_ports[index],
                                  encoder_button_pins[index]) != RESET;
}

#endif

