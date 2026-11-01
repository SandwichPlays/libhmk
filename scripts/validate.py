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

    num_keys = kb_json.num_keys
    num_layers = kb_json.num_layers
    num_profiles = kb_json.num_profiles
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
    if kb_json.hall.oversample is not None:
        if kb_json.hall.oversample not in driver.metadata.adc.oversample_map:
            raise ValueError(
                f"[{keyboard}] Unsupported hall.oversample={kb_json.hall.oversample} for MCU '{kb_json.mcu}'. "
                f"Supported: {list(driver.metadata.adc.oversample_map.keys())}"
            )
    if kb_json.hall.sample_cycles is not None:
        if kb_json.hall.sample_cycles not in driver.metadata.adc.sample_cycles_map:
            raise ValueError(
                f"[{keyboard}] Unsupported hall.sample_cycles={kb_json.hall.sample_cycles} for MCU '{kb_json.mcu}'. "
                f"Supported: {list(driver.metadata.adc.sample_cycles_map.keys())}"
            )

    # 3. Validate GPIO pins & duplicate pin usage across subsystems
    used_pins: dict[str, str] = {}

    def claim_pin(pin: str, owner: str, require_adc: bool = False):
        if not driver.metadata.is_valid_gpio_pin(pin):
            raise ValueError(
                f"[{keyboard}] Invalid GPIO pin '{pin}' in {owner} for MCU '{kb_json.mcu}'"
            )
        if require_adc and pin not in driver.metadata.adc.input_pins:
            raise ValueError(
                f"[{keyboard}] Pin '{pin}' in {owner} is not an ADC-capable pin on '{kb_json.mcu}'. "
                f"Valid ADC pins: {driver.metadata.adc.input_pins}"
            )
        if pin in used_pins:
            raise ValueError(
                f"[{keyboard}] Duplicate GPIO pin '{pin}' used by both '{used_pins[pin]}' and '{owner}'"
            )
        used_pins[pin] = owner

    def check_key_index(k: int | None, owner: str):
        if k is None:
            return
        if k < 0 or k >= num_keys:
            raise ValueError(
                f"[{keyboard}] 0-indexed key {k} in {owner} is out of range (0..{num_keys - 1})"
            )

    # Hall Direct ADC Pins
    if kb_json.hall.pins is not None:
        pins = kb_json.hall.pins
        keys = kb_json.hall.keys
        if keys is None or len(pins) != len(keys):
            raise ValueError(
                f"[{keyboard}] hall.pins length ({len(pins)}) must match hall.keys length"
            )
        for pin in pins:
            claim_pin(pin, "hall.pins", require_adc=True)
        for k in keys:
            check_key_index(k, "hall.keys")

    # Hall Analog Multiplexer
    if kb_json.hall.mux is not None:
        mux = kb_json.hall.mux
        for pin in mux.select:
            claim_pin(pin, "hall.mux.select")
        for pin in mux.pins:
            claim_pin(pin, "hall.mux.pins", require_adc=True)
        expected_mux_channels = 1 << len(mux.select)
        if len(mux.matrix) != len(mux.pins):
            raise ValueError(
                f"[{keyboard}] hall.mux.matrix must have {len(mux.pins)} rows (one per mux ADC pin)"
            )
        for row in mux.matrix:
            if len(row) != expected_mux_channels:
                raise ValueError(
                    f"[{keyboard}] Each row in hall.mux.matrix must have {expected_mux_channels} entries (2^{len(mux.select)})"
                )
            for k in row:
                check_key_index(k, "hall.mux.matrix")

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
                check_key_index(k, "digital.direct.keys")
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
    if kb_json.hall.mux is not None and driver.metadata.mux_timer:
        used_timers[driver.metadata.mux_timer] = "hall.mux settle timer"

    if kb_json.encoders is not None:
        for idx, enc in enumerate(kb_json.encoders):
            owner = f"encoders[{idx}]"
            for pin in enc.pins:
                claim_pin(pin, f"{owner}.pins")
            for k in enc.keys:
                check_key_index(k, f"{owner}.keys")
            if enc.button_pin is not None:
                claim_pin(enc.button_pin, f"{owner}.button_pin")
                if enc.button_key is None:
                    raise ValueError(
                        f"[{keyboard}] {owner} specifies button_pin='{enc.button_pin}' without button_key"
                    )
                check_key_index(enc.button_key, f"{owner}.button_key")
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
