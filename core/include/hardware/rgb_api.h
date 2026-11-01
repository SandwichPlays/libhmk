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
 * @brief Initialize the hardware Timer-PWM + DMA RGB driver
 */
void hardware_rgb_init(void);

/**
 * @brief Check if the previous DMA frame + reset latch has finished transmitting
 */
bool hardware_rgb_is_ready(void);

/**
 * @brief Transmit GRB pixel buffer (`num_leds * 3` bytes) via non-blocking DMA
 */
void hardware_rgb_show(const uint8_t *grb_buf, uint32_t num_leds);

