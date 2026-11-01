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
 * @brief Initialize hardware timer quadrature or polled GPIO encoders
 */
void hardware_encoder_init(void);

/**
 * @brief High-frequency poll hook for GPIO-polled encoders (no-op for timer encoders)
 */
void hardware_encoder_poll(void);

/**
 * @brief Read current accumulated raw quadrature count for encoder `index`
 */
int16_t hardware_encoder_read(uint32_t index);

/**
 * @brief Check if encoder `index` has a configured push button
 */
bool hardware_encoder_has_button(uint32_t index);

/**
 * @brief Read raw pin state of encoder `index` button (false = pressed / active-low)
 */
bool hardware_encoder_button_read(uint32_t index);

