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

/* Avoid conflict with STM32G4 HAL's own FLASH_SIZE definition */
#define HMK_FLASH_SIZE FLASH_SIZE
#undef FLASH_SIZE

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "stm32g4xx_hal.h"
#include "tusb.h"
#pragma GCC diagnostic pop

/* 170 MHz from HSI (16 MHz): PLLM=4 -> 4 MHz VCO in, PLLN=85 -> 340 MHz, PLLR=2 -> 170 MHz */
static void board_clock_init(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_HSI48;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV4;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    board_error_handler();

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
    board_error_handler();
}

/* Enable CRS for crystal-less USB using HSI48 trimmed by USB SOF */
static void board_crs_init(void) {
  RCC_CRSInitTypeDef crs_init = {0};

  __HAL_RCC_CRS_CLK_ENABLE();

  crs_init.Prescaler = RCC_CRS_SYNC_DIV1;
  crs_init.Source = RCC_CRS_SYNC_SOURCE_USB;
  crs_init.ErrorLimitValue = RCC_CRS_ERRORLIMIT_DEFAULT;
  crs_init.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000, 1000);

  HAL_RCCEx_CRSConfig(&crs_init);
}

static void board_usb_init(void) {
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USB;
  PeriphClkInit.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    board_error_handler();

  __HAL_RCC_GPIOA_CLK_ENABLE();

  /* PA11=DM, PA12=DP - set to analog so USB peripheral controls them */
  GPIO_InitStruct.Pin = GPIO_PIN_11 | GPIO_PIN_12;
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  __HAL_RCC_USB_CLK_ENABLE();

  HAL_NVIC_SetPriority(USB_LP_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(USB_LP_IRQn);
}

static void board_bootloader_jump(void) {
  volatile const uint32_t *bootloader_vector =
      (volatile const uint32_t *)BOOTLOADER_ADDR;
  uint32_t sp = bootloader_vector[0];
  uint32_t bootloader_entry = bootloader_vector[1];

  SysTick->CTRL = 0;
  SysTick->LOAD = 0;
  SysTick->VAL = 0;

  NVIC->ICER[0] = 0xFFFFFFFF;
  NVIC->ICER[1] = 0xFFFFFFFF;
  NVIC->ICER[2] = 0xFFFFFFFF;
  NVIC->ICER[3] = 0xFFFFFFFF;

  SCB->VTOR = (uint32_t)BOOTLOADER_ADDR;
  __set_MSP(sp);
  __set_PSP(sp);

  ((void (*)(void))bootloader_entry)();
  while (1)
    ;
}

extern uint32_t _board_bootloader_flag[];
#define BOARD_BOOTLOADER_FLAG _board_bootloader_flag[0]

void board_init(void) {
  if (BOARD_BOOTLOADER_FLAG == BOOTLOADER_MAGIC) {
    BOARD_BOOTLOADER_FLAG = 0;
    board_bootloader_jump();
  }

  HAL_Init();
  board_clock_init();
  board_crs_init();
  board_usb_init();

  /* Enable DWT cycle counter for board_cycle_count() */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  DWT->CYCCNT = 0;
}

void board_error_handler(void) {
  __disable_irq();
  while (1)
    ;
}

void board_reset(void) { NVIC_SystemReset(); }

void board_enter_bootloader(void) {
  BOARD_BOOTLOADER_FLAG = BOOTLOADER_MAGIC;
  NVIC_SystemReset();
}

uint32_t board_serial(char *buf) {
  const volatile uint8_t *uid = (const volatile uint8_t *)UID_BASE;
  for (uint32_t i = 0; i < 12; i++) {
    buf[i * 2]     = M_HEX(uid[i] >> 4);
    buf[i * 2 + 1] = M_HEX(uid[i] & 0x0F);
  }
  return 24;
}

uint32_t board_cycle_count(void) { return DWT->CYCCNT; }

//--------------------------------------------------------------------+
// Interrupt Handlers
//--------------------------------------------------------------------+

void SysTick_Handler(void) { HAL_IncTick(); }

void USB_LP_IRQHandler(void) { tud_int_handler(0); }
