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

#if defined(RGB_ENABLE)

#include "at32f402_405.h"

// 800 kHz bit period at 216 MHz = 270 ticks (0..269)
#define WS2812_PWM_PERIOD ((F_CPU / 800000u) - 1u)
#define WS2812_DUTY_0 ((F_CPU / 800000u) * 32u / 100u)
#define WS2812_DUTY_1 ((F_CPU / 800000u) * 68u / 100u)
// 64 trailing zero-duty slots (~80us) for WS2812/SK6812 latch reset
#define WS2812_RESET_SLOTS 64u
#define WS2812_TOTAL_SLOTS ((RGB_NUM_LEDS * 24u) + WS2812_RESET_SLOTS)

__attribute__((aligned(4))) static uint16_t pwm_dma_buf[WS2812_TOTAL_SLOTS];

static gpio_pins_source_type get_pin_source(uint16_t pin_mask) {
  for (uint32_t i = 0; i < 16; i++) {
    if (pin_mask & (1u << i)) {
      return (gpio_pins_source_type)i;
    }
  }
  return GPIO_PINS_SOURCE0;
}

void hardware_rgb_init(void) {
  crm_periph_clock_enable(CRM_SCFG_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);

  gpio_type *port = RGB_PORT;
  if (port == GPIOA) crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  else if (port == GPIOB) crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  else if (port == GPIOC) crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);

  tmr_type *tmr = RGB_TIMER;
  if (tmr == TMR1) crm_periph_clock_enable(CRM_TMR1_PERIPH_CLOCK, TRUE);
  else if (tmr == TMR2) crm_periph_clock_enable(CRM_TMR2_PERIPH_CLOCK, TRUE);
  else if (tmr == TMR3) crm_periph_clock_enable(CRM_TMR3_PERIPH_CLOCK, TRUE);
  else if (tmr == TMR4) crm_periph_clock_enable(CRM_TMR4_PERIPH_CLOCK, TRUE);

  gpio_init_type gpio_init_struct;
  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_pins = RGB_PIN;
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(port, &gpio_init_struct);

  gpio_mux_sel_type mux = (tmr == TMR1 || tmr == TMR2) ? GPIO_MUX_1 : GPIO_MUX_2;
  gpio_pin_mux_config(port, get_pin_source(RGB_PIN), mux);

  tmr_base_init(tmr, WS2812_PWM_PERIOD, 0);
  tmr_cnt_dir_set(tmr, TMR_COUNT_UP);

  tmr_output_config_type tmr_oc_struct;
  tmr_output_default_para_init(&tmr_oc_struct);
  tmr_oc_struct.oc_mode = TMR_OUTPUT_CONTROL_PWM_MODE_A;
  tmr_oc_struct.oc_idle_state = FALSE;
  tmr_oc_struct.oc_polarity = TMR_OUTPUT_ACTIVE_HIGH;
  tmr_oc_struct.oc_output_state = TRUE;

#if RGB_TIMER_CHANNEL == 1
  tmr_output_channel_config(tmr, TMR_SELECT_CHANNEL_1, &tmr_oc_struct);
  tmr_channel_value_set(tmr, TMR_SELECT_CHANNEL_1, 0);
  tmr_output_channel_buffer_enable(tmr, TMR_SELECT_CHANNEL_1, TRUE);
#elif RGB_TIMER_CHANNEL == 2
  tmr_output_channel_config(tmr, TMR_SELECT_CHANNEL_2, &tmr_oc_struct);
  tmr_channel_value_set(tmr, TMR_SELECT_CHANNEL_2, 0);
  tmr_output_channel_buffer_enable(tmr, TMR_SELECT_CHANNEL_2, TRUE);
#elif RGB_TIMER_CHANNEL == 3
  tmr_output_channel_config(tmr, TMR_SELECT_CHANNEL_3, &tmr_oc_struct);
  tmr_channel_value_set(tmr, TMR_SELECT_CHANNEL_3, 0);
  tmr_output_channel_buffer_enable(tmr, TMR_SELECT_CHANNEL_3, TRUE);
#else
  tmr_output_channel_config(tmr, TMR_SELECT_CHANNEL_4, &tmr_oc_struct);
  tmr_channel_value_set(tmr, TMR_SELECT_CHANNEL_4, 0);
  tmr_output_channel_buffer_enable(tmr, TMR_SELECT_CHANNEL_4, TRUE);
#endif

  if (tmr == TMR1) {
    tmr_output_enable(tmr, TRUE);
  }
  tmr_dma_control_enable(tmr, TMR_DMA_REQUEST_OVF, TRUE);

  dmamux_enable(DMA1, TRUE);
  if (tmr == TMR1) dmamux_init(DMA1MUX_CHANNEL2, DMAMUX_DMAREQ_ID_TMR1_OVERFLOW);
  else if (tmr == TMR2) dmamux_init(DMA1MUX_CHANNEL2, DMAMUX_DMAREQ_ID_TMR2_OVERFLOW);
  else if (tmr == TMR3) dmamux_init(DMA1MUX_CHANNEL2, DMAMUX_DMAREQ_ID_TMR3_OVERFLOW);
  else if (tmr == TMR4) dmamux_init(DMA1MUX_CHANNEL2, DMAMUX_DMAREQ_ID_TMR4_OVERFLOW);

  tmr_counter_enable(tmr, TRUE);
}

bool hardware_rgb_is_ready(void) {
  return dma_data_number_get(DMA1_CHANNEL2) == 0;
}

void hardware_rgb_show(const uint8_t *grb_buf, uint32_t num_leds) {
  if (!hardware_rgb_is_ready()) {
    return;
  }

  uint32_t slot = 0;
  uint32_t total_bytes = num_leds * 3u;
  for (uint32_t b = 0; b < total_bytes; b++) {
    uint8_t byte_val = grb_buf[b];
    for (int bit = 7; bit >= 0; bit--) {
      pwm_dma_buf[slot++] =
          (byte_val & (1u << bit)) ? (uint16_t)WS2812_DUTY_1 : (uint16_t)WS2812_DUTY_0;
    }
  }
  for (uint32_t r = 0; r < WS2812_RESET_SLOTS; r++) {
    pwm_dma_buf[slot++] = 0;
  }

  tmr_type *tmr = RGB_TIMER;
#if RGB_TIMER_CHANNEL == 1
  volatile uint32_t *cval_reg = (volatile uint32_t *)&tmr->c1dt;
#elif RGB_TIMER_CHANNEL == 2
  volatile uint32_t *cval_reg = (volatile uint32_t *)&tmr->c2dt;
#elif RGB_TIMER_CHANNEL == 3
  volatile uint32_t *cval_reg = (volatile uint32_t *)&tmr->c3dt;
#else
  volatile uint32_t *cval_reg = (volatile uint32_t *)&tmr->c4dt;
#endif

  dma_channel_enable(DMA1_CHANNEL2, FALSE);
  dma_flag_clear(DMA1_FDT2_FLAG);

  dma_init_type dma_init_struct;
  dma_default_para_init(&dma_init_struct);
  dma_init_struct.buffer_size = slot;
  dma_init_struct.direction = DMA_DIR_MEMORY_TO_PERIPHERAL;
  dma_init_struct.memory_base_addr = (uint32_t)pwm_dma_buf;
  dma_init_struct.memory_data_width = DMA_MEMORY_DATA_WIDTH_HALFWORD;
  dma_init_struct.memory_inc_enable = TRUE;
  dma_init_struct.peripheral_base_addr = (uint32_t)cval_reg;
  dma_init_struct.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_HALFWORD;
  dma_init_struct.peripheral_inc_enable = FALSE;
  dma_init_struct.priority = DMA_PRIORITY_LOW;
  dma_init_struct.loop_mode_enable = FALSE;
  dma_init(DMA1_CHANNEL2, &dma_init_struct);

  dma_channel_enable(DMA1_CHANNEL2, TRUE);
}

#endif

