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
 * @brief Optional board-specific initialization hook called at the end of init
 */
void board_init_user(void);

/**
 * @brief Optional board-specific hook called at the end of each matrix scan
 */
void matrix_scan_user(void);

/**
 * @brief Optional board-specific hook called before processing a key press/release.
 * Return false to intercept/suppress default key processing.
 */
bool key_event_user(uint8_t key, bool keydown);

/**
 * @brief Optional board-specific hook called each iteration of layout_task
 */
void layout_task_user(void);

/**
 * @brief Optional board-specific hook called when an encoder detent triggers.
 * Return false to intercept/suppress default encoder key pulse.
 */
bool encoder_event_user(uint8_t index, bool clockwise);

