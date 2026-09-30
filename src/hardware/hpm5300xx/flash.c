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

#include "hpm_romapi.h"
#include "hpm_l1c_drv.h"

// XPI0 flash is memory-mapped at this base address for XIP reads.
// Verify this address for HPM5361 in the memory map section of the user manual.
#define HPM_XPI0_FLASH_BASE 0x80000000UL

static xpi_nor_config_t nor_config;
static bool flash_initialized = false;

void flash_init(void) {
  // Auto-configure the XPI NOR flash using ROM API.
  // The config option header values depend on the flash chip connected.
  // For HPM5300 on-chip embedded flash, use the default option (all zeros
  // = auto-detect). Adjust BOARD_APP_XPI_NOR_CFG_OPT_HDR if needed.
  xpi_nor_config_option_t config_option = {
      .header.U = 0xFCF90001, // Auto-detect probe
      .option0.U = 0x00000007,
      .option1.U = 0,
  };

  hpm_stat_t s = ROM_API_TABLE_ROOT->xpi_driver_if->auto_config(
      HPM_XPI0, &nor_config, &config_option);

  flash_initialized = (s == status_success);
}

bool flash_erase(uint32_t sector) {
  if (!flash_initialized || sector >= FLASH_NUM_SECTORS)
    return false;

  uint32_t addr = sector * FLASH_SECTOR_SIZE;

  hpm_stat_t s = ROM_API_TABLE_ROOT->xpi_driver_if->erase(
      HPM_XPI0, xpi_xfer_channel_auto, &nor_config, addr, FLASH_SECTOR_SIZE);

  // Invalidate D-cache for the erased region so subsequent reads are fresh
  l1c_dc_invalidate(HPM_XPI0_FLASH_BASE + addr, FLASH_SECTOR_SIZE);

  return s == status_success;
}

bool flash_read(uint32_t addr, void *buf, uint32_t len) {
  if (addr + len * 4 > FLASH_SIZE)
    return false;

  // XIP: direct memory-mapped read from flash address space
  memcpy(buf, (const void *)(HPM_XPI0_FLASH_BASE + addr), len * 4);
  return true;
}

bool flash_write(uint32_t addr, const void *buf, uint32_t len) {
  if (!flash_initialized || addr + len * 4 > FLASH_SIZE)
    return false;

  hpm_stat_t s = ROM_API_TABLE_ROOT->xpi_driver_if->program(
      HPM_XPI0, xpi_xfer_channel_auto, &nor_config, buf, addr, len * 4);

  // Invalidate D-cache for the written region
  l1c_dc_invalidate(HPM_XPI0_FLASH_BASE + addr, len * 4);

  return s == status_success;
}
