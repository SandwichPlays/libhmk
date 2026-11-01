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

#if defined(DIGITAL_ENABLE)

#include "at32f402_405.h"

static void enable_gpio_clock(gpio_type *port) {
  if (port == GPIOA) crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  else if (port == GPIOB) crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  else if (port == GPIOC) crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
  else if (port == GPIOD) crm_periph_clock_enable(CRM_GPIOD_PERIPH_CLOCK, TRUE);
  else if (port == GPIOF) crm_periph_clock_enable(CRM_GPIOF_PERIPH_CLOCK, TRUE);
}

#if defined(DIGITAL_DIRECT_COUNT) && DIGITAL_DIRECT_COUNT > 0
static gpio_type *direct_ports[] = DIGITAL_DIRECT_PORTS;
static const uint16_t direct_pins[] = DIGITAL_DIRECT_PINS;
#endif

#if defined(DIGITAL_MATRIX_ROWS) && DIGITAL_MATRIX_ROWS > 0
static gpio_type *matrix_row_ports[] = DIGITAL_MATRIX_ROW_PORTS;
static const uint16_t matrix_row_pins[] = DIGITAL_MATRIX_ROW_PINS;
static gpio_type *matrix_col_ports[] = DIGITAL_MATRIX_COL_PORTS;
static const uint16_t matrix_col_pins[] = DIGITAL_MATRIX_COL_PINS;
#endif

void digital_gpio_init(void) {
#if defined(DIGITAL_DIRECT_COUNT) && DIGITAL_DIRECT_COUNT > 0
  for (uint32_t i = 0; i < DIGITAL_DIRECT_COUNT; i++) {
    enable_gpio_clock(direct_ports[i]);
    gpio_init_type gpio_init_struct;
    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_pins = direct_pins[i];
    gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
#if DIGITAL_DIRECT_ACTIVE_LOW
    gpio_init_struct.gpio_pull = GPIO_PULL_UP;
#else
    gpio_init_struct.gpio_pull = GPIO_PULL_DOWN;
#endif
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(direct_ports[i], &gpio_init_struct);
  }
#endif

#if defined(DIGITAL_MATRIX_ROWS) && DIGITAL_MATRIX_ROWS > 0
#if DIGITAL_MATRIX_COL2ROW
  for (uint32_t r = 0; r < DIGITAL_MATRIX_ROWS; r++) {
    enable_gpio_clock(matrix_row_ports[r]);
    gpio_init_type gpio_init_struct;
    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_pins = matrix_row_pins[r];
    gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(matrix_row_ports[r], &gpio_init_struct);
    gpio_bits_set(matrix_row_ports[r], matrix_row_pins[r]);
  }
  for (uint32_t c = 0; c < DIGITAL_MATRIX_COLS; c++) {
    enable_gpio_clock(matrix_col_ports[c]);
    gpio_init_type gpio_init_struct;
    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_pins = matrix_col_pins[c];
    gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
    gpio_init_struct.gpio_pull = GPIO_PULL_UP;
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(matrix_col_ports[c], &gpio_init_struct);
  }
#else
  for (uint32_t c = 0; c < DIGITAL_MATRIX_COLS; c++) {
    enable_gpio_clock(matrix_col_ports[c]);
    gpio_init_type gpio_init_struct;
    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_pins = matrix_col_pins[c];
    gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(matrix_col_ports[c], &gpio_init_struct);
    gpio_bits_set(matrix_col_ports[c], matrix_col_pins[c]);
  }
  for (uint32_t r = 0; r < DIGITAL_MATRIX_ROWS; r++) {
    enable_gpio_clock(matrix_row_ports[r]);
    gpio_init_type gpio_init_struct;
    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_pins = matrix_row_pins[r];
    gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
    gpio_init_struct.gpio_pull = GPIO_PULL_UP;
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(matrix_row_ports[r], &gpio_init_struct);
  }
#endif
#endif
}

void digital_gpio_scan(bool *raw_states) {
  uint32_t idx = 0;

#if defined(DIGITAL_DIRECT_COUNT) && DIGITAL_DIRECT_COUNT > 0
  for (uint32_t i = 0; i < DIGITAL_DIRECT_COUNT; i++) {
    bool pin_high =
        gpio_input_data_bit_read(direct_ports[i], direct_pins[i]) != RESET;
#if DIGITAL_DIRECT_ACTIVE_LOW
    raw_states[idx++] = !pin_high;
#else
    raw_states[idx++] = pin_high;
#endif
  }
#endif

#if defined(DIGITAL_MATRIX_ROWS) && DIGITAL_MATRIX_ROWS > 0
#if DIGITAL_MATRIX_COL2ROW
  for (uint32_t r = 0; r < DIGITAL_MATRIX_ROWS; r++) {
    gpio_bits_reset(matrix_row_ports[r], matrix_row_pins[r]);
    for (volatile uint32_t d = 0; d < 16; d++) {
      __NOP();
    }
    for (uint32_t c = 0; c < DIGITAL_MATRIX_COLS; c++) {
      bool pin_high =
          gpio_input_data_bit_read(matrix_col_ports[c], matrix_col_pins[c]) !=
          RESET;
      raw_states[idx++] = !pin_high;
    }
    gpio_bits_set(matrix_row_ports[r], matrix_row_pins[r]);
  }
#else
  for (uint32_t c = 0; c < DIGITAL_MATRIX_COLS; c++) {
    gpio_bits_reset(matrix_col_ports[c], matrix_col_pins[c]);
    for (volatile uint32_t d = 0; d < 16; d++) {
      __NOP();
    }
    for (uint32_t r = 0; r < DIGITAL_MATRIX_ROWS; r++) {
      bool pin_high =
          gpio_input_data_bit_read(matrix_row_ports[r], matrix_row_pins[r]) !=
          RESET;
      raw_states[idx + r * DIGITAL_MATRIX_COLS + c] = !pin_high;
    }
    gpio_bits_set(matrix_col_ports[c], matrix_col_pins[c]);
  }
#endif
#endif
}

#endif

