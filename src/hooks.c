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

#include "keyboard_hooks.h"

__attribute__((weak)) void board_init_user(void) {}

__attribute__((weak)) void matrix_scan_user(void) {}

__attribute__((weak)) bool key_event_user(uint8_t key, bool keydown) {
  (void)key;
  (void)keydown;
  return true;
}

__attribute__((weak)) void layout_task_user(void) {}

__attribute__((weak)) bool encoder_event_user(uint8_t index, bool clockwise) {
  (void)index;
  (void)clockwise;
  return true;
}

