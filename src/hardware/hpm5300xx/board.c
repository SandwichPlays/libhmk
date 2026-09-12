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

#include "hpm_pllctlv2_drv.h"
#include "hpm_clock_drv.h"
#include "hpm_gpio_drv.h"
#include "hpm_ioc_regs.h"
#include "hpm_interrupt.h"
#include "hpm_misc.h"
#include "tusb.h"

// --------------------------------------------------------------------
// Bootloader Flag
//
// BPOR_GPR0 is a battery-backed general purpose register that
// survives warm resets. We use it to store the bootloader flag.
// --------------------------------------------------------------------

#define BOARD_BOOTLOADER_FLAG HPM_BPOR->BPOR_GPR[0]

// --------------------------------------------------------------------
// Clock Initialization: 480MHz CPU0
// --------------------------------------------------------------------

static void board_clock_init(void) {
  // Configure PLL0 to 480MHz (XTAL 24MHz * 20)
  pllctlv2_init_pll_with_freq(HPM_PLLCTLV2, 0, BOARD_CPU_FREQ);

  // CPU0: PLL0 CLK0 / 1 = 480MHz
  clock_set_source_divider(clock_cpu0, clk_src_pll0_clk0, 1);

  // AHB bus: PLL0 CLK0 / 2 = 240MHz
  clock_set_source_divider(clock_ahb, clk_src_pll0_clk0, 2);

  // Short delay for PLL to stabilize
  for (volatile int i = 0; i < 1000; i++) {
    __asm__("nop");
  }
}

// --------------------------------------------------------------------
// USB Initialization: USB0 HS OTG (DWC2 with embedded HS PHY)
// --------------------------------------------------------------------

static void board_usb_init(void) {
  // Enable USB0 clock
  clock_add_to_group(clock_usb0, 0);

  // Enable USB0 VBUS sense bypass (VBUS always present via USB connector)
  // HPM USB0 uses the built-in HS PHY - no external PHY config needed.
  HPM_USB0->GCCFG |= USB_GCCFG_VBUSIG_MASK;

  // Set NVIC-equivalent (PLIC) priority for USB0 interrupt
  intc_m_enable_irq_with_priority(IRQn_USB0, 2);
}

// --------------------------------------------------------------------
// GPIO Helper: configure IOC pad and GPIO direction
// --------------------------------------------------------------------

void board_gpio_input_pullup(uint32_t ioc_pad, uint8_t gpio_port, uint8_t pin) {
  // Set IOC pad to GPIO function
  HPM_IOC->PAD[ioc_pad].FUNC_CTL = IOC_PAD_FUNC_CTL_ALT_SELECT_SET(0); // GPIO
  // Enable pull-up
  HPM_IOC->PAD[ioc_pad].PAD_CTL =
      IOC_PAD_PAD_CTL_PU_SET(1) | IOC_PAD_PAD_CTL_PE_SET(1);
  gpio_set_pin_input(HPM_GPIO0, gpio_port, pin);
}

// --------------------------------------------------------------------
// Encoder Initialization
// --------------------------------------------------------------------

#if defined(ENCODER_ENABLE)
static void board_encoder_init(void) {
  // Use GPTMR in quadrature mode for each encoder.
  // ENCODER_TIMERS is a macro defined by make_cmake.py as a list of timer
  // instance pointers (e.g., {HPM_GPTMR1}). Each timer handles one encoder.
  //
  // TODO: Map ENCODER_TIMERS string names to HPM GPTMR instances.
  // TODO: Configure encoder GPIO pins via ENCODER_PORTS and ENCODER_PINS
  //       using board_gpio_input_pullup() for each pin.
  //
  // For HPM quadrature encoding, use the QEI peripheral for best accuracy:
  //   qei_config_t cfg;
  //   qei_get_default_config(HPM_QEI0, &cfg);
  //   cfg.phcnt_max = ENCODER_RESOLUTION;
  //   qei_init(HPM_QEI0, &cfg);
}
#endif

// --------------------------------------------------------------------
// Board API
// --------------------------------------------------------------------

void board_init(void) {
  // Check bootloader flag before any peripheral init
  if (BOARD_BOOTLOADER_FLAG == BOOTLOADER_MAGIC) {
    BOARD_BOOTLOADER_FLAG = 0;
    // Jump to HPM ROM bootloader at address 0x00000000
    // The ROM runs USB/UART ISP based on boot pin state at this point.
    // We force execution there by disabling interrupts and jumping.
    __asm__ volatile(
        "csrci mstatus, 8\n" // Disable machine interrupts (MIE bit)
        "fence.i\n"
        "li t0, 0x0\n"       // HPM ROM start
        "jr t0\n"
        : : : "t0");
    while (1);
  }

  board_clock_init();
  board_usb_init();

#if defined(ENCODER_ENABLE)
  board_encoder_init();
#endif
}

void board_error_handler(void) {
  __asm__ volatile("csrci mstatus, 8"); // Disable interrupts
  while (1);
}

void board_reset(void) {
  // Trigger system reset via WDT (watchdog timer instant timeout)
  clock_add_to_group(clock_wdog0, 0);
  HPM_WDG0->CTRL = WDG_CTRL_CLKSEL_SET(1) | WDG_CTRL_RSTEN_MASK;
  HPM_WDG0->WREN = 0x31415926; // Unlock write
  HPM_WDG0->CNT  = 0;          // Zero count forces immediate WDT reset
  while (1);
}

void board_enter_bootloader(void) {
  BOARD_BOOTLOADER_FLAG = BOOTLOADER_MAGIC;
  board_reset();
}

uint32_t board_serial(char *buf) {
  // HPM5361 Unique ID: 96-bit fuse value at OTP address
  // Address from HPM5300 user manual - verify offset
  const volatile uint8_t *uid = (const volatile uint8_t *)(0xF0020000UL);
  for (uint32_t i = 0; i < 12; i++) {
    buf[i * 2]     = M_HEX(uid[i] >> 4);
    buf[i * 2 + 1] = M_HEX(uid[i] & 0x0F);
  }
  return 24;
}

uint32_t board_cycle_count(void) {
  // RISC-V machine cycle counter (equivalent to ARM DWT->CYCCNT)
  uint32_t count;
  __asm__ volatile("csrr %0, mcycle" : "=r"(count));
  return count;
}

// --------------------------------------------------------------------
// Interrupt Handlers
// --------------------------------------------------------------------

SDK_DECLARE_EXT_ISR_M(IRQn_USB0, isr_usb0)
void isr_usb0(void) { tud_int_handler(0); }

// --------------------------------------------------------------------
// TinyUSB Callbacks
// --------------------------------------------------------------------

void tud_suspend_cb(bool remote_wakeup_en) {
  (void)remote_wakeup_en;
  // Optionally gate USB clock here to reduce power
}

void tud_resume_cb(void) {
  // Restore USB clock if gated
}
