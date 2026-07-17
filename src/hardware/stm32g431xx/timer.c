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
#pragma GCC diagnostic pop

void timer_init(void) {}

uint32_t timer_read(void) { return HAL_GetTick(); }
