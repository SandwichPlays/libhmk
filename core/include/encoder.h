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

#if defined(ENCODER_ENABLE)

/**
 * @brief Initialize the rotary encoder module
 *
 * @return None
 */
void encoder_init(void);

/**
 * @brief Encoder task
 *
 * This function should be called in the main loop to check for encoder rotation.
 *
 * @return None
 */
void encoder_task(void);

#endif
