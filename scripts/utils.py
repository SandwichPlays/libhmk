# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# this program. If not, see <https://www.gnu.org/licenses/>.

import os
from drivers import *
from schema.keyboard import Keyboard, SwitchDefinition


class CompilerFlags:
    def __init__(self):
        self.flags = []

    # Get the list of compiler flags
    def get_flags(self):
        return self.flags

    # Add a preprocessor definition to the compiler flags
    def define(self, name: str, value: int | str | None = None):
        match value:
            case None:
                self.flags.append(f"-D{name}")
            case int() | str():
                self.flags.append(f"-D{name}='{value}'")

    # Add a linker definition to the compiler flags
    def linker_defsym(self, name: str, value: int | str | None = None):
        match value:
            case None:
                self.flags.append(f"-Wl,--defsym,{name}")
            case int() | str():
                self.flags.append(f"-Wl,--defsym,{name}={value}")

    # Add an include path to the compiler flags
    def include(self, path: str):
        self.flags.append(f"-I{path}")


# Load and validate the keyboard JSON configuration file
def get_kb_json(keyboard: str) -> Keyboard:
    with open(os.path.join("keyboards", keyboard, "keyboard.json"), "r") as f:
        return Keyboard.model_validate_json(f.read())


# Load and validate a switch JSON definition file from switches/<switch_id>.json
def get_switch_json(switch_id: str) -> SwitchDefinition:
    path = os.path.join("switches", f"{switch_id}.json")
    if not os.path.isfile(path):
        raise ValueError(f"Switch definition file not found: {path}")
    with open(path, "r") as f:
        return SwitchDefinition.model_validate_json(f.read())


# Load the driver based on the keyboard MCU
def get_driver(keyboard: str) -> Driver:
    kb_json = get_kb_json(keyboard)
    driver_key = kb_json.mcu.lower()
    if driver_key not in DRIVERS:
        raise ValueError(
            f"Unsupported MCU '{kb_json.mcu}'. Supported MCUs: {list(DRIVERS.keys())}"
        )
    return DRIVERS[driver_key]


# Convert a Python list to a C array initializer
def to_c_array(arr: list | bytes):
    return f"{{{', '.join(to_c_array(x) if isinstance(x, list) else str(x) for x in arr)}}}"


# Convert a Python dictionary to a C struct initializer
def to_c_struct(value: dict):
    def to_c_val(x):
        if isinstance(x, list):
            return to_c_array(x)
        return str(x)
    return f"{{{', '.join(f'.{k} = {to_c_val(v)}' for k, v in value.items())}}}"


# Convert a Python list to a C array slice definition
def to_slice_def(name: str, arr: list | bytes):
    return f"#define {name.upper()} {', '.join(str(x) for x in arr)}"


# Get the ADC resolution, or default to the maximum resolution supported by the MCU
def get_adc_resolution(kb_json: Keyboard, driver: Driver):
    return kb_json.hall.adc_resolution or driver.metadata.adc.max_resolution


# Pad a keymap with KC_TRNS layers if fewer than num_layers are specified
def _pad_keymap_layers(keymap: list[list[str]], num_layers: int, num_keys: int) -> list[list[str]]:
    padded = list(keymap)
    while len(padded) < num_layers:
        padded.append(["KC_TRNS"] * num_keys)
    return padded


# Resolve per-profile default keymaps
def resolve_default_keymaps(kb_json: Keyboard) -> list[list[list[str]]]:
    num_layers = kb_json.num_layers
    num_keys = kb_json.num_keys
    assert num_layers is not None and num_keys is not None

    if kb_json.keymaps is not None:
        return [
            _pad_keymap_layers(km, num_layers, num_keys) for km in kb_json.keymaps
        ]
    else:
        if kb_json.keymap is None:
            raise ValueError(
                "Default keymap must be specified when no per-profile default keymaps are specified"
            )
        padded = _pad_keymap_layers(kb_json.keymap, num_layers, num_keys)
        return [padded] * kb_json.num_profiles


# Compute the set of 0-indexed keys that are connected to Hall-effect analog sensors
def get_analog_key_set(kb_json: Keyboard) -> set[int]:
    analog_keys: set[int] = set()
    if kb_json.hall.pins is not None and kb_json.hall.keys is not None:
        for k in kb_json.hall.keys:
            if k is not None:
                analog_keys.add(k)
    if kb_json.hall.mux is not None:
        for row in kb_json.hall.mux.matrix:
            for k in row:
                if k is not None:
                    analog_keys.add(k)
    return analog_keys


# Resolve per-LED 0-indexed key mapping (None for underglow/unmapped LEDs)
def resolve_rgb_key_map(kb_json: Keyboard) -> list[int | None]:
    if kb_json.rgb is None:
        return []
    if kb_json.rgb.key_map is not None:
        return list(kb_json.rgb.key_map)

    ordered_keys: list[int] = []
    seen: set[int] = set()
    for row in kb_json.layout.keys:
        for k in row:
            if k.option is not None and k.option[1] != 0:
                continue
            if k.key not in seen:
                seen.add(k.key)
                ordered_keys.append(k.key)
    if kb_json.encoders is not None:
        for enc in kb_json.encoders:
            if enc.button_key is not None and enc.button_key not in seen:
                seen.add(enc.button_key)
                ordered_keys.append(enc.button_key)

    return [
        ordered_keys[i] if i < len(ordered_keys) else None
        for i in range(kb_json.rgb.num_leds)
    ]


# Resolve normalized (0..255, 0..255) physical coordinates for each RGB LED
def resolve_rgb_led_positions(kb_json: Keyboard) -> list[tuple[int, int]]:
    if kb_json.rgb is None:
        return []

    num_leds = kb_json.rgb.num_leds
    key_map = resolve_rgb_key_map(kb_json)

    key_centers: dict[int, tuple[float, float]] = {}
    pos_x = 0.0
    pos_y = 0.0
    for row in kb_json.layout.keys:
        for k in row:
            w = k.w if k.w is not None else 1.0
            h = k.h if k.h is not None else 1.0
            pos_x += k.x if k.x is not None else 0.0
            pos_y += k.y if k.y is not None else 0.0
            if k.option is None or k.option[1] == 0:
                if k.key not in key_centers:
                    key_centers[k.key] = (pos_x + w * 0.5, pos_y + h * 0.5)
            pos_x += w
        pos_x = 0.0
        pos_y += 1.0

    if kb_json.encoders is not None:
        for enc in kb_json.encoders:
            cx = enc.x + enc.w * 0.5
            cy = enc.y + enc.h * 0.5
            if enc.button_key is not None:
                key_centers[enc.button_key] = (cx, cy)
            for ek in enc.keys:
                if ek not in key_centers:
                    key_centers[ek] = (cx, cy)

    raw_coords: list[tuple[float, float]] = []
    for i in range(num_leds):
        if kb_json.rgb.led_positions is not None and i < len(kb_json.rgb.led_positions):
            raw_coords.append(kb_json.rgb.led_positions[i])
        else:
            mapped_key = key_map[i] if i < len(key_map) else None
            if mapped_key is not None and mapped_key in key_centers:
                raw_coords.append(key_centers[mapped_key])
            else:
                raw_coords.append((float(i), 0.0))

    xs = [c[0] for c in raw_coords]
    ys = [c[1] for c in raw_coords]
    min_x, max_x = min(xs), max(xs)
    min_y, max_y = min(ys), max(ys)
    span_x = (max_x - min_x) if max_x > min_x else 1.0
    span_y = (max_y - min_y) if max_y > min_y else 1.0

    normalized: list[tuple[int, int]] = []
    for cx, cy in raw_coords:
        nx = int(round(((cx - min_x) / span_x) * 255.0)) if max_x > min_x else 128
        ny = int(round(((cy - min_y) / span_y) * 255.0)) if max_y > min_y else 128
        normalized.append((max(0, min(255, nx)), max(0, min(255, ny))))
    return normalized


# Estimate minimum wear-leveling virtual storage size in bytes for eeconfig_t
def compute_min_wl_virtual_size(kb_json: Keyboard) -> int:
    num_keys = kb_json.num_keys or 1
    num_layers = kb_json.num_layers or 4
    num_profiles = kb_json.num_profiles or 4
    num_adv = kb_json.num_advanced_keys or 1
    rgb_storage_leds = max(num_keys, kb_json.rgb.num_leds if kb_json.rgb else 0)

    # Header + calibration + bottom_out_threshold + distance_lut(2048) + per-profile
    global_bytes = 32 + (4 + num_keys) + (2 * num_keys) + 2048
    per_profile = (
        (num_layers * num_keys)
        + (num_keys * 11)
        + (num_adv * 13)
        + num_keys
        + 10
        + 10
        + (rgb_storage_leds * 3)
        + 16
    )
    est_bytes = global_bytes + (num_profiles * per_profile)
    # Round up to next power of 2, minimum 8192
    size = 8192
    while size < est_bytes:
        size *= 2
    return size
