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

#include "crc32.h"
#include "hardware/hardware.h"

#include "hpm_crc_drv.h"
#include "hpm_clock_drv.h"

void crc32_init(void) {
  clock_add_to_group(clock_crc0, 0);
}

uint32_t crc32_compute(const void *buf, uint32_t len, uint32_t crc) {
  crc_channel_config_t config;
  crc_get_default_channel_config(&config);
  // CRC-32/ISO-HDLC preset (same polynomial as standard CRC32)
  config.preset = crc_preset_crc32;
  // Feed in the initial value from the caller (enables chaining)
  config.init = crc;

  crc_setup_channel(HPM_CRC, 0, &config);
  crc_calc_block(HPM_CRC, 0, (const uint8_t *)buf, len);
  return crc_get_result(HPM_CRC, 0);
}
