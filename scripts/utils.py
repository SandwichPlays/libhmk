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

import math
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


# Load the driver based on the keyboard configuration
def get_driver(keyboard: str) -> Driver:
    kb_json = get_kb_json(keyboard)
    driver_key = kb_json.hardware.driver.lower()
    if driver_key not in DRIVERS:
        raise ValueError(
            f"Unsupported driver '{kb_json.hardware.driver}'. Supported drivers/MCUs: {list(DRIVERS.keys())}"
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
    return kb_json.analog.adc_resolution or driver.metadata.adc.max_resolution


# Pad a keymap with KC_TRNS layers if fewer than num_layers are specified
def _pad_keymap_layers(keymap: list[list[str]], num_layers: int, num_keys: int) -> list[list[str]]:
    padded = list(keymap)
    while len(padded) < num_layers:
        padded.append(["KC_TRNS"] * num_keys)
    return padded


# Resolve per-profile default keymaps
def resolve_default_keymaps(kb_json: Keyboard) -> list[list[list[str]]]:
    num_layers = kb_json.keyboard.num_layers
    num_keys = kb_json.keyboard.num_keys
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
        return [padded] * kb_json.keyboard.num_profiles


# Compute the set of 1-indexed keys that are connected to Hall-effect analog sensors
def get_analog_key_set(kb_json: Keyboard) -> set[int]:
    analog_keys: set[int] = set()
    if kb_json.analog.raw is not None and kb_json.analog.raw.vector is not None:
        for k in kb_json.analog.raw.vector:
            if k > 0:
                analog_keys.add(k)
    if kb_json.analog.mux is not None:
        for row in kb_json.analog.mux.matrix:
            for k in row:
                if k > 0:
                    analog_keys.add(k)
    return analog_keys


# Estimate minimum wear-leveling virtual storage size in bytes for eeconfig_t
def compute_min_wl_virtual_size(kb_json: Keyboard) -> int:
    num_keys = kb_json.keyboard.num_keys or 1
    num_layers = kb_json.keyboard.num_layers or 4
    num_profiles = kb_json.keyboard.num_profiles or 4
    num_adv = kb_json.keyboard.num_advanced_keys or 1

    # Header + calibration + bottom_out_threshold + distance_lut(2048) + per-profile
    global_bytes = 32 + (4 + num_keys) + (2 * num_keys) + 2048
    per_profile = (
        (num_layers * num_keys)
        + (num_keys * 11)
        + (num_adv * 13)
        + num_keys
        + 16
    )
    est_bytes = global_bytes + (num_profiles * per_profile)
    # Round up to next power of 2, minimum 8192
    size = 8192
    while size < est_bytes:
        size *= 2
    return size
