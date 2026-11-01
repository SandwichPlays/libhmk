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

import utils


def validate_keyboard(keyboard: str):
    driver = utils.get_driver(keyboard)
    kb_json = utils.get_kb_json(keyboard)
    sw_json = utils.get_switch_json(kb_json.switch)

    num_keys = kb_json.keyboard.num_keys
    num_layers = kb_json.keyboard.num_layers
    num_profiles = kb_json.keyboard.num_profiles
    assert num_keys is not None and num_layers is not None

    # 1. Validate Switch LUT
    if len(sw_json.lut) != 1024:
        raise ValueError(
            f"[{keyboard}] Switch '{kb_json.switch}' LUT must have 1024 entries, got {len(sw_json.lut)}"
        )
    if sw_json.lut[0] != 0 or sw_json.lut[-1] != 10000:
        raise ValueError(
            f"[{keyboard}] Switch '{kb_json.switch}' LUT must start at 0 and end at 10000"
        )

    # 2. Validate ADC oversample & sample_cycles
    if kb_json.analog.oversample is not None:
        if kb_json.analog.oversample not in driver.metadata.adc.oversample_map:
            raise ValueError(
                f"[{keyboard}] Unsupported analog.oversample={kb_json.analog.oversample} for driver '{kb_json.hardware.driver}'. "
                f"Supported: {list(driver.metadata.adc.oversample_map.keys())}"
            )
    if kb_json.analog.sample_cycles is not None:
        if kb_json.analog.sample_cycles not in driver.metadata.adc.sample_cycles_map:
            raise ValueError(
                f"[{keyboard}] Unsupported analog.sample_cycles={kb_json.analog.sample_cycles} for driver '{kb_json.hardware.driver}'. "
                f"Supported: {list(driver.metadata.adc.sample_cycles_map.keys())}"
            )

    # 3. Validate GPIO pins & duplicate pin usage across subsystems
    used_pins: dict[str, str] = {}

    def claim_pin(pin: str, owner: str, require_adc: bool = False):
        if not driver.metadata.is_valid_gpio_pin(pin):
            raise ValueError(
                f"[{keyboard}] Invalid GPIO pin '{pin}' in {owner} for driver '{kb_json.hardware.driver}'"
            )
        if require_adc and pin not in driver.metadata.adc.input_pins:
            raise ValueError(
                f"[{keyboard}] Pin '{pin}' in {owner} is not an ADC-capable pin on '{kb_json.hardware.driver}'. "
                f"Valid ADC pins: {driver.metadata.adc.input_pins}"
            )
        if pin in used_pins:
            raise ValueError(
                f"[{keyboard}] Duplicate GPIO pin '{pin}' used by both '{used_pins[pin]}' and '{owner}'"
            )
        used_pins[pin] = owner

    def check_key_index(k: int, owner: str, allow_zero: bool = True):
        low = 0 if allow_zero else 1
        if k < low or k > num_keys:
            raise ValueError(
                f"[{keyboard}] Key index {k} in {owner} is out of range ({low}..{num_keys})"
            )

    # Analog Raw
    if kb_json.analog.raw is not None:
        raw = kb_json.analog.raw
        if raw.vector is None or len(raw.input) != len(raw.vector):
            raise ValueError(
                f"[{keyboard}] analog.raw.input length ({len(raw.input)}) must match analog.raw.vector length"
            )
        for ch_or_pin in raw.input:
            pin = (
                ch_or_pin
                if isinstance(ch_or_pin, str)
                else driver.metadata.adc.input_pins[ch_or_pin]
            )
            claim_pin(pin, "analog.raw.input", require_adc=True)
        for k in raw.vector:
            check_key_index(k, "analog.raw.vector")

    # Analog Mux
    if kb_json.analog.mux is not None:
        mux = kb_json.analog.mux
        for pin in mux.select:
            claim_pin(pin, "analog.mux.select")
        for ch_or_pin in mux.input:
            pin = (
                ch_or_pin
                if isinstance(ch_or_pin, str)
                else driver.metadata.adc.input_pins[ch_or_pin]
            )
            claim_pin(pin, "analog.mux.input", require_adc=True)
        expected_mux_channels = 1 << len(mux.select)
        if len(mux.matrix) != len(mux.input):
            raise ValueError(
                f"[{keyboard}] analog.mux.matrix must have {len(mux.input)} rows (one per mux input)"
            )
        for row in mux.matrix:
            if len(row) != expected_mux_channels:
                raise ValueError(
                    f"[{keyboard}] Each row in analog.mux.matrix must have {expected_mux_channels} entries (2^{len(mux.select)})"
                )
            for k in row:
                check_key_index(k, "analog.mux.matrix")

    # Digital Keys
    if kb_json.digital is not None:
        if kb_json.digital.direct is not None:
            direct = kb_json.digital.direct
            if len(direct.pins) != len(direct.keys):
                raise ValueError(
                    f"[{keyboard}] digital.direct.pins length ({len(direct.pins)}) must match digital.direct.keys length ({len(direct.keys)})"
                )
            for pin in direct.pins:
                claim_pin(pin, "digital.direct.pins")
            for k in direct.keys:
                check_key_index(k, "digital.direct.keys", allow_zero=False)
        if kb_json.digital.matrix is not None:
            dmat = kb_json.digital.matrix
            for pin in dmat.rows:
                claim_pin(pin, "digital.matrix.rows")
            for pin in dmat.cols:
                claim_pin(pin, "digital.matrix.cols")
            if len(dmat.matrix) != len(dmat.rows):
                raise ValueError(
                    f"[{keyboard}] digital.matrix.matrix must have {len(dmat.rows)} rows"
                )
            for row in dmat.matrix:
                if len(row) != len(dmat.cols):
                    raise ValueError(
                        f"[{keyboard}] Each row in digital.matrix.matrix must have {len(dmat.cols)} columns"
                    )
                for k in row:
                    check_key_index(k, "digital.matrix.matrix")

    # Encoders & Timers
    used_timers: dict[str, str] = {}
    if kb_json.analog.mux is not None and driver.metadata.mux_timer:
        used_timers[driver.metadata.mux_timer] = "analog.mux settle timer"

    if kb_json.encoders is not None:
        for idx, enc in enumerate(kb_json.encoders):
            owner = f"encoders[{idx}]"
            for pin in enc.pins:
                claim_pin(pin, f"{owner}.pins")
            for k in enc.keys:
                if k < 0 or k >= num_keys:
                    raise ValueError(
                        f"[{keyboard}] Encoder key index {k} in {owner}.keys is out of range (0..{num_keys - 1})"
                    )
            if enc.button_pin is not None:
                claim_pin(enc.button_pin, f"{owner}.button_pin")
                if enc.button_key is None:
                    raise ValueError(
                        f"[{keyboard}] {owner} specifies button_pin='{enc.button_pin}' without button_key"
                    )
                if enc.button_key < 0 or enc.button_key >= num_keys:
                    raise ValueError(
                        f"[{keyboard}] Encoder button_key {enc.button_key} in {owner}.button_key is out of range (0..{num_keys - 1})"
                    )
            tmr, _, _ = driver.metadata.resolve_encoder_timer(
                enc.pins[0], enc.pins[1], enc.timer
            )
            if tmr != "NULL":
                if tmr in used_timers:
                    raise ValueError(
                        f"[{keyboard}] Timer '{tmr}' conflict between '{used_timers[tmr]}' and '{owner}'"
                    )
                used_timers[tmr] = owner

    # RGB
    if kb_json.rgb is not None:
        claim_pin(kb_json.rgb.pin, "rgb.pin")
        if kb_json.rgb.timer in used_timers:
            raise ValueError(
                f"[{keyboard}] Timer '{kb_json.rgb.timer}' conflict between '{used_timers[kb_json.rgb.timer]}' and 'rgb'"
            )
        used_timers[kb_json.rgb.timer] = "rgb"

    # 4. Validate Default Keymaps
    default_keymaps = utils.resolve_default_keymaps(kb_json)
    if len(default_keymaps) != num_profiles:
        raise ValueError(
            f"[{keyboard}] Expected default keymaps to have {num_profiles} profiles, got {len(default_keymaps)}"
        )
    for keymap in default_keymaps:
        if len(keymap) != num_layers:
            raise ValueError(
                f"[{keyboard}] Expected default keymaps to have {num_layers} layers, got {len(keymap)}"
            )
        for layer in keymap:
            if len(layer) != num_keys:
                raise ValueError(
                    f"[{keyboard}] Expected each keymap layer to have {num_keys} keys, got {len(layer)}"
                )


try:
    Import("env")  # type: ignore
    validate_keyboard(env["PIOENV"])  # type: ignore
except NameError:
    pass
