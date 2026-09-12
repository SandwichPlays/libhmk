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

#include "hpm_gptmr_drv.h"
#include "hpm_clock_drv.h"
#include "hpm_interrupt.h"
#include "board.h" // HPM SDK board header for BOARD_APP_GPTMR etc.

static volatile uint32_t counter = 0;

void timer_init(void) {
  // Use GPTMR0 channel 0 for 1ms periodic interrupt
  clock_add_to_group(clock_gptmr0, 0);

  gptmr_channel_config_t config;
  gptmr_channel_get_default_config(HPM_GPTMR0, &config);

  // Set reload value for 1ms period
  uint32_t freq = clock_get_frequency(clock_gptmr0);
  config.reload = freq / 1000 - 1;
  config.cmp_initial_polarity_high = false;

  gptmr_channel_config(HPM_GPTMR0, 0, &config, false);
  gptmr_channel_reset_count(HPM_GPTMR0, 0);
  gptmr_enable_irq(HPM_GPTMR0, GPTMR_CH_RLD_IRQ_MASK(0));

  intc_m_enable_irq_with_priority(IRQn_GPTMR0, 1);
  gptmr_start_counter(HPM_GPTMR0, 0);
}

uint32_t timer_read(void) { return counter; }

//--------------------------------------------------------------------+
// Interrupt Handlers
//--------------------------------------------------------------------+

SDK_DECLARE_EXT_ISR_M(IRQn_GPTMR0, isr_gptmr0)
void isr_gptmr0(void) {
  volatile uint32_t s = HPM_GPTMR0->SR;
  HPM_GPTMR0->SR = s; // Clear all flags

  if (GPTMR_CH_IS_RLD_IRQ(s, 0)) {
    counter++;
  }
}
