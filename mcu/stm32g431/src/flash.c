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
#include <string.h>

#define MCU_FLASH_SIZE FLASH_SIZE
#undef FLASH_SIZE

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "stm32g4xx_hal.h"
#pragma GCC diagnostic pop

void flash_init(void) {}

bool flash_erase(uint32_t sector) {
  if (sector >= FLASH_NUM_SECTORS)
    return false;

  FLASH_EraseInitTypeDef erase = {0};
  uint32_t error = 0;
  bool success = true;

  HAL_FLASH_Unlock();
  
  // Clear any existing flash flags before erasing
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_OPTVERR | FLASH_FLAG_FASTERR | FLASH_FLAG_MISERR |
                         FLASH_FLAG_PGSERR | FLASH_FLAG_SIZERR | FLASH_FLAG_PGAERR |
                         FLASH_FLAG_WRPERR | FLASH_FLAG_PROGERR | FLASH_FLAG_OPERR);

  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.Banks = FLASH_BANK_1;
  erase.Page = sector;
  erase.NbPages = 1;
  
  success = (HAL_FLASHEx_Erase(&erase, &error) == HAL_OK);
  HAL_FLASH_Lock();

  return success;
}

bool flash_read(uint32_t addr, void *buf, uint32_t len) {
  if (addr + len * 4 > MCU_FLASH_SIZE)
    return false;

  memcpy(buf, (void *)(FLASH_BASE + addr), len * 4);

  return true;
}

bool flash_write(uint32_t addr, const void *buf, uint32_t len) {
  if (addr + len * 4 > MCU_FLASH_SIZE)
    return false;

  const uint32_t *buf32 = buf;

  HAL_FLASH_Unlock();
  
  // Clear any existing flash flags before writing
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_OPTVERR | FLASH_FLAG_FASTERR | FLASH_FLAG_MISERR |
                         FLASH_FLAG_PGSERR | FLASH_FLAG_SIZERR | FLASH_FLAG_PGAERR |
                         FLASH_FLAG_WRPERR | FLASH_FLAG_PROGERR | FLASH_FLAG_OPERR);

  for (uint32_t i = 0; i < len; i += 2) {
    uint64_t data = 0;
    if (i + 1 < len) {
      data = ((uint64_t)buf32[i + 1] << 32) | buf32[i];
    } else {
      data = ((uint64_t)FLASH_EMPTY_VAL << 32) | buf32[i];
    }
    
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, FLASH_BASE + addr + i * 4,
                          data) != HAL_OK) {
      HAL_FLASH_Lock();
      return false;
    }
  }
  HAL_FLASH_Lock();

  return true;
}
