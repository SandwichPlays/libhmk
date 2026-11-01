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

#pragma once

#include "common.h"

/**
 * @brief Initialize the RGB subsystem (no-op when RGB_ENABLE is not defined)
 */
void rgb_init(void);

/**
 * @brief Set an individual LED color in RGB888
 */
void rgb_set_led(uint32_t index, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Run periodic RGB animations & non-blocking DMA transmission (~120 Hz)
 */
void rgb_task(void);

