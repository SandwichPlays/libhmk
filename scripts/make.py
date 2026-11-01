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
import utils
from drivers import *
from schema.keyboard import KeyboardUSBPort

Import("env")

# Throttle concurrent compile jobs to prevent freezing the machine
SetOption("num_jobs", 4)

keyboard = env["PIOENV"]

# Load JSON files, switch curve, and MCU driver
kb_json = utils.get_kb_json(keyboard)
sw_json = utils.get_switch_json(kb_json.switch)
driver = utils.get_driver(keyboard)
mcu_dir = driver.mcu_dir or f"mcu/{kb_json.mcu.lower()}"

# Add source filter for MCU port source files
env.Append(SRC_FILTER=[f"+<{mcu_dir}/src/>"])

# Compile optional per-keyboard custom hook file keyboards/<board>/keyboard.c if present
kb_custom_src = os.path.join("keyboards", keyboard, "keyboard.c")
if os.path.isfile(kb_custom_src):
    env.BuildSources(
        os.path.join("$BUILD_DIR", "keyboard_user"),
        os.path.join("$PROJECT_DIR", "keyboards", keyboard),
        src_filter=["-<*>", "+<keyboard.c>"],
    )

# Build with -O3 for maximum performance
env.ProcessUnFlags("-Os")
env.Append(
    CCFLAGS=[
        "-O3",
        "-fomit-frame-pointer",
        "-funroll-loops",
        "-falign-functions=4",
        "-falign-loops=4",
    ]
)
env.Append(LINKFLAGS=["-O3"])

# Build Flags
build_flags = utils.CompilerFlags()

# Include headers. We prioritize including MCU port, keyboard, and core headers.
build_flags.include(f"{mcu_dir}/include")
build_flags.include(f"keyboards/{keyboard}")
build_flags.include("core/include")

# Bootloader Configuration
build_flags.define("BOOTLOADER_ADDR", driver.metadata.bootloader.address)
build_flags.define("BOOTLOADER_MAGIC", driver.metadata.bootloader.magic)

# Flash Configuration
flash_size = driver.metadata.flash.get_flash_size()
flash_num_sectors = driver.metadata.flash.get_num_sectors()
build_flags.define("FLASH_SIZE", flash_size)
build_flags.linker_defsym("FLASH_SIZE", flash_size)
build_flags.define("FLASH_NUM_SECTORS", flash_num_sectors)
build_flags.define("FLASH_EMPTY_VAL", driver.metadata.flash.empty_value)

match driver.metadata.flash.sector_sizes:
    case NonUniformSectors(sizes):
        build_flags.define("FLASH_SECTOR_SIZES", utils.to_c_array(sizes))
    case UniformSectors(size, _):
        build_flags.define("FLASH_SECTOR_SIZE", size)

# TinyUSB Configuration
build_flags.define("CFG_TUSB_MCU", f"OPT_MCU_{driver.tinyusb.mcu.upper()}")

# Clock Configuration
build_flags.define("BOARD_HSE_VALUE", kb_json.crystal_hz)
build_flags.define("HSE_VALUE", kb_json.crystal_hz)

# USB Configuration
if kb_json.usb.port == KeyboardUSBPort.FULL_SPEED:
    build_flags.define("BOARD_USB_FS")
else:
    build_flags.define("BOARD_USB_HS")
build_flags.define("USB_MANUFACTURER_NAME", f'"{kb_json.manufacturer}"')
build_flags.define("USB_PRODUCT_NAME", f'"{kb_json.name}"')
build_flags.define("USB_VENDOR_ID", kb_json.usb.vid)
build_flags.define("USB_PRODUCT_ID", kb_json.usb.pid)

# Switch LUT Configuration
build_flags.define("DEFAULT_DISTANCE_LUT", utils.to_c_array(sw_json.lut))

# Hall / Analog Configuration
build_flags.define("ADC_NUM_CHANNELS", len(driver.metadata.adc.input_pins))
build_flags.define("ADC_RESOLUTION", utils.get_adc_resolution(kb_json, driver))

if kb_json.hall.invert_adc:
    build_flags.define("MATRIX_INVERT_ADC_VALUES")

if kb_json.hall.mux_delay_us is not None:
    build_flags.define("ADC_SAMPLE_DELAY", kb_json.hall.mux_delay_us)

if kb_json.hall.oversample is not None:
    os_pair = driver.metadata.adc.oversample_map[kb_json.hall.oversample]
    if os_pair is None:
        build_flags.define("ADC_OVERSAMPLE_DISABLE")
    else:
        build_flags.define("ADC_OVERSAMPLE_RATIO", os_pair[0])
        build_flags.define("ADC_OVERSAMPLE_SHIFT", os_pair[1])

if kb_json.hall.sample_cycles is not None:
    sc_macro = driver.metadata.adc.sample_cycles_map[kb_json.hall.sample_cycles]
    build_flags.define("ADC_NUM_SAMPLE_CYCLES", sc_macro)

# Direct ADC Input Configuration (convert 0-indexed key to 1-indexed C key where 0 = unconnected)
if kb_json.hall.pins is not None:
    assert kb_json.hall.keys is not None
    c_raw_vector = [0 if k is None else k + 1 for k in kb_json.hall.keys]

    build_flags.define("ADC_NUM_RAW_INPUTS", len(kb_json.hall.pins))
    build_flags.define(
        "ADC_RAW_INPUT_CHANNELS",
        utils.to_c_array(driver.metadata.adc.to_adc_inputs(kb_json.hall.pins)),
    )
    build_flags.define("ADC_RAW_INPUT_VECTOR", utils.to_c_array(c_raw_vector))

# Analog Multiplexer ADC Input Configuration (convert 0-indexed key to 1-indexed C key where 0 = unconnected)
if kb_json.hall.mux is not None:
    mux = kb_json.hall.mux

    build_flags.define("ADC_NUM_MUX_INPUTS", len(mux.pins))
    build_flags.define(
        "ADC_MUX_INPUT_CHANNELS",
        utils.to_c_array(driver.metadata.adc.to_adc_inputs(mux.pins)),
    )
    build_flags.define("ADC_NUM_MUX_SELECT_PINS", len(mux.select))

    ports, pin_nums = driver.metadata.adc.to_gpio_array(mux.select)
    build_flags.define("ADC_MUX_SELECT_PORTS", utils.to_c_array(ports))
    build_flags.define("ADC_MUX_SELECT_PINS", utils.to_c_array(pin_nums))

    c_mux_matrix = [
        [0 if k is None else k + 1 for k in row] for row in mux.matrix
    ]
    build_flags.define(
        "ADC_MUX_INPUT_MATRIX", utils.to_c_array(list(map(list, zip(*c_mux_matrix))))
    )

# Exact list of 0-indexed analog Hall keys so mux/hybrid boards never calibrate digital/encoder keys
analog_keys_0idx = sorted(utils.get_analog_key_set(kb_json))
if len(analog_keys_0idx) > 0:
    build_flags.define("ANALOG_KEY_COUNT", len(analog_keys_0idx))
    build_flags.define("ANALOG_KEY_LIST", utils.to_c_array(analog_keys_0idx))

# Digital / Mechanical Key Configuration
if kb_json.digital is not None:
    digital = kb_json.digital
    digital_key_map: list[int] = []
    debounce_ms = 5

    if digital.direct is not None and len(digital.direct.pins) > 0:
        direct = digital.direct
        debounce_ms = direct.debounce_ms
        d_ports, d_pins = driver.metadata.adc.to_gpio_array(direct.pins)
        build_flags.define("DIGITAL_DIRECT_COUNT", len(direct.pins))
        build_flags.define("DIGITAL_DIRECT_PORTS", utils.to_c_array(d_ports))
        build_flags.define("DIGITAL_DIRECT_PINS", utils.to_c_array(d_pins))
        build_flags.define(
            "DIGITAL_DIRECT_ACTIVE_LOW", 1 if direct.active_low else 0
        )
        digital_key_map.extend(k + 1 for k in direct.keys)

    if digital.matrix is not None and len(digital.matrix.rows) > 0:
        dmat = digital.matrix
        debounce_ms = dmat.debounce_ms
        r_ports, r_pins = driver.metadata.adc.to_gpio_array(dmat.rows)
        c_ports, c_pins = driver.metadata.adc.to_gpio_array(dmat.cols)
        build_flags.define("DIGITAL_MATRIX_ROWS", len(dmat.rows))
        build_flags.define("DIGITAL_MATRIX_COLS", len(dmat.cols))
        build_flags.define("DIGITAL_MATRIX_ROW_PORTS", utils.to_c_array(r_ports))
        build_flags.define("DIGITAL_MATRIX_ROW_PINS", utils.to_c_array(r_pins))
        build_flags.define("DIGITAL_MATRIX_COL_PORTS", utils.to_c_array(c_ports))
        build_flags.define("DIGITAL_MATRIX_COL_PINS", utils.to_c_array(c_pins))
        build_flags.define(
            "DIGITAL_MATRIX_COL2ROW", 1 if dmat.diode_direction == "col2row" else 0
        )
        for row in dmat.matrix:
            digital_key_map.extend(0 if k is None else k + 1 for k in row)

    if len(digital_key_map) > 0:
        build_flags.define("DIGITAL_ENABLE")
        build_flags.define("DIGITAL_KEY_COUNT", len(digital_key_map))
        build_flags.define("DIGITAL_KEY_MAP", utils.to_c_array(digital_key_map))
        build_flags.define("DIGITAL_DEBOUNCE_MS", debounce_ms)

# Calibration Configuration (merge board overrides with switch file defaults)
switch_travel_mm = kb_json.hall.travel_mm or sw_json.travel_mm
travel_val = int(round(switch_travel_mm * 10))
bottom_out_threshold = (
    kb_json.hall.bottom_out_threshold
    if kb_json.hall.bottom_out_threshold is not None
    else sw_json.initial_bottom_out_threshold
)
assert kb_json.num_keys is not None
cal_dict = {
    "initial_rest_value": kb_json.hall.rest_adc,
    "initial_bottom_out_threshold": bottom_out_threshold,
    "switch_travel": [travel_val] * kb_json.num_keys,
}
build_flags.define("DEFAULT_CALIBRATION", utils.to_c_struct(cal_dict))

# Tuning Configuration
if kb_json.tuning is not None:
    t = kb_json.tuning
    if t.ema_alpha_numerator is not None:
        build_flags.define("MATRIX_EMA_ALPHA_NUMERATOR", t.ema_alpha_numerator)
    if t.ema_alpha_shift is not None:
        build_flags.define("MATRIX_EMA_ALPHA_SHIFT", t.ema_alpha_shift)
    if t.hysteresis_gap_um is not None:
        build_flags.define("MATRIX_HYST_GAP", t.hysteresis_gap_um)
    if t.calibration_epsilon is not None:
        build_flags.define("MATRIX_CALIBRATION_EPSILON", t.calibration_epsilon)
    if t.baseline_down_ms is not None:
        build_flags.define("MATRIX_BASELINE_DOWN_TIME_MS", t.baseline_down_ms)
    if t.rest_lenience_um is not None:
        build_flags.define("DISTANCE_REST_LENIENCE", t.rest_lenience_um)
    if t.bottom_lenience_um is not None:
        build_flags.define("DISTANCE_BOTTOM_LENIENCE", t.bottom_lenience_um)

# Wear leveling configuration (auto-scales WL_VIRTUAL_SIZE for large boards)
wear_leveling = kb_json.wear_leveling
wl_virtual_size = (
    wear_leveling and wear_leveling.virtual_size
) or utils.compute_min_wl_virtual_size(kb_json)
wl_write_log_size = (wear_leveling and wear_leveling.write_log_size) or 65536
wl_backing_store_size = wl_virtual_size + wl_write_log_size

build_flags.define("WL_VIRTUAL_SIZE", wl_virtual_size)
build_flags.define("WL_WRITE_LOG_SIZE", wl_write_log_size)

# Reserve flash for wear leveling (round up to whole sectors from the end)
wl_base_address = flash_size - driver.metadata.flash.round_up_to_flash_sectors(
    wl_backing_store_size
)
build_flags.define("WL_BASE_ADDRESS", wl_base_address)
build_flags.linker_defsym("WL_BASE_ADDRESS", wl_base_address)

# Keyboard Configuration
assert kb_json.num_layers is not None and kb_json.num_keys is not None and kb_json.num_advanced_keys is not None
build_flags.define("NUM_PROFILES", kb_json.num_profiles)
build_flags.define("NUM_LAYERS", kb_json.num_layers)
build_flags.define("NUM_KEYS", kb_json.num_keys)
build_flags.define("NUM_ADVANCED_KEYS", kb_json.num_advanced_keys)

# Default Keymaps (per profile)
default_keymaps = utils.resolve_default_keymaps(kb_json)
build_flags.define("DEFAULT_KEYMAPS", utils.to_c_array(default_keymaps))

# Actuation Configuration
if kb_json.actuation is not None:
    act = kb_json.actuation
    if act.actuation_point_um is not None:
        build_flags.define("DEFAULT_ACTUATION_POINT", act.actuation_point_um)
    if act.rt_sensitivity_um is not None:
        build_flags.define("DEFAULT_RT_SENSITIVITY", act.rt_sensitivity_um)
    if act.rt_deadzone_um is not None:
        build_flags.define("DEFAULT_RT_DEADZONE", act.rt_deadzone_um)

# Encoder Configuration
if kb_json.encoders is not None:
    encoders = kb_json.encoders

    build_flags.define("ENCODER_ENABLE")
    build_flags.define("ENCODER_COUNT", len(encoders))

    encoder_timers = []
    encoder_ports = []
    encoder_pin_nums = []
    encoder_pin_muxes = []
    encoder_keys = []
    encoder_resolutions = []
    encoder_button_ports = []
    encoder_button_pins = []
    encoder_button_keys = []

    for encoder in encoders:
        tmr, mux_a, mux_b = driver.metadata.resolve_encoder_timer(
            encoder.pins[0], encoder.pins[1], encoder.timer
        )
        encoder_timers.append(tmr)
        ports, pin_nums = driver.metadata.adc.to_gpio_array(encoder.pins)
        encoder_ports.append(ports)
        encoder_pin_nums.append(pin_nums)
        encoder_pin_muxes.append([mux_a, mux_b])
        encoder_keys.append(encoder.keys)
        encoder_resolutions.append(encoder.resolution)

        if encoder.button_pin is not None:
            port, pin_num = driver.metadata.adc.to_gpio(encoder.button_pin)
            encoder_button_ports.append(port)
            encoder_button_pins.append(pin_num)
            encoder_button_keys.append(encoder.button_key)
        else:
            encoder_button_ports.append("NULL")
            encoder_button_pins.append(0)
            encoder_button_keys.append(255)

    build_flags.define("ENCODER_TIMERS", utils.to_c_array(encoder_timers))
    build_flags.define("ENCODER_PORTS", utils.to_c_array(encoder_ports))
    build_flags.define("ENCODER_PINS", utils.to_c_array(encoder_pin_nums))
    build_flags.define("ENCODER_PIN_MUXES", utils.to_c_array(encoder_pin_muxes))
    build_flags.define("ENCODER_KEYS", utils.to_c_array(encoder_keys))
    build_flags.define("ENCODER_RESOLUTIONS", utils.to_c_array(encoder_resolutions))
    build_flags.define("ENCODER_BUTTON_PORTS", utils.to_c_array(encoder_button_ports))
    build_flags.define("ENCODER_BUTTON_PINS", utils.to_c_array(encoder_button_pins))
    build_flags.define("ENCODER_BUTTON_KEYS", utils.to_c_array(encoder_button_keys))

# RGB Configuration
if kb_json.rgb is not None:
    rgb = kb_json.rgb
    rgb_port, rgb_pin = driver.metadata.adc.to_gpio(rgb.pin)
    build_flags.define("RGB_ENABLE")
    build_flags.define("RGB_PORT", rgb_port)
    build_flags.define("RGB_PIN", rgb_pin)
    build_flags.define("RGB_NUM_LEDS", rgb.num_leds)
    build_flags.define("RGB_TIMER", rgb.timer)
    build_flags.define("RGB_TIMER_CHANNEL", rgb.channel)
    build_flags.define("RGB_MAX_CURRENT_MA", rgb.max_current_ma)

# Add source build flags
env.Append(BUILD_FLAGS=build_flags.get_flags())
