#!/usr/bin/env python3
# make_cmake.py - CMake equivalent of make.py for non-PlatformIO targets (e.g. HPM5300)

import sys
import os

sys.path.insert(0, os.path.dirname(__file__))

import utils
from drivers import *
from schema.keyboard import KeyboardUSBPort


def to_cmake_string(flags: list[str]) -> str:
    return "\n".join(f'    "{f}"' for f in flags)


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <keyboard_name> <output.cmake>", file=sys.stderr)
        sys.exit(1)

    keyboard = sys.argv[1]
    output_path = sys.argv[2]

    kb_json = utils.get_kb_json(keyboard)
    sw_json = utils.get_switch_json(kb_json.switch)
    driver = utils.get_driver(keyboard)
    mcu_dir = driver.mcu_dir or f"mcu/{kb_json.mcu.lower()}"

    build_flags = utils.CompilerFlags()

    # Bootloader configuration
    build_flags.define("BOOTLOADER_ADDR", driver.metadata.bootloader.address)
    build_flags.define("BOOTLOADER_MAGIC", driver.metadata.bootloader.magic)

    # Flash configuration
    flash_size = driver.metadata.flash.get_flash_size()
    flash_num_sectors = driver.metadata.flash.get_num_sectors()
    build_flags.define("FLASH_SIZE", flash_size)
    build_flags.define("FLASH_NUM_SECTORS", flash_num_sectors)
    build_flags.define("FLASH_EMPTY_VAL", driver.metadata.flash.empty_value)

    match driver.metadata.flash.sector_sizes:
        case NonUniformSectors(sizes):
            build_flags.define("FLASH_SECTOR_SIZES", utils.to_c_array(sizes))
        case UniformSectors(size, _):
            build_flags.define("FLASH_SECTOR_SIZE", size)

    # TinyUSB
    build_flags.define("CFG_TUSB_MCU", f"OPT_MCU_{driver.tinyusb.mcu.upper()}")

    # Clock
    build_flags.define("BOARD_HSE_VALUE", kb_json.crystal_hz)
    build_flags.define("HSE_VALUE", kb_json.crystal_hz)

    # USB
    if kb_json.usb.port == KeyboardUSBPort.FULL_SPEED:
        build_flags.define("BOARD_USB_FS")
    else:
        build_flags.define("BOARD_USB_HS")
    build_flags.define("USB_MANUFACTURER_NAME", f'"{kb_json.manufacturer}"')
    build_flags.define("USB_PRODUCT_NAME", f'"{kb_json.name}"')
    build_flags.define("USB_VENDOR_ID", kb_json.usb.vid)
    build_flags.define("USB_PRODUCT_ID", kb_json.usb.pid)

    # Switch LUT
    build_flags.define("DEFAULT_DISTANCE_LUT", utils.to_c_array(sw_json.lut))

    # ADC
    build_flags.define("ADC_NUM_CHANNELS", len(driver.metadata.adc.input_pins))
    build_flags.define("ADC_RESOLUTION", utils.get_adc_resolution(kb_json, driver))

    if kb_json.hall.invert_adc:
        build_flags.define("MATRIX_INVERT_ADC_VALUES")

    if kb_json.hall.mux_delay_us is not None:
        build_flags.define("ADC_SAMPLE_DELAY", kb_json.hall.mux_delay_us)

    # Direct ADC inputs
    if kb_json.hall.pins is not None:
        assert kb_json.hall.keys is not None
        c_raw_vector = [0 if k is None else k + 1 for k in kb_json.hall.keys]
        build_flags.define("ADC_NUM_RAW_INPUTS", len(kb_json.hall.pins))
        build_flags.define(
            "ADC_RAW_INPUT_CHANNELS",
            utils.to_c_array(driver.metadata.adc.to_adc_inputs(kb_json.hall.pins)),
        )
        build_flags.define("ADC_RAW_INPUT_VECTOR", utils.to_c_array(c_raw_vector))

    # MUX inputs
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
            "ADC_MUX_INPUT_MATRIX",
            utils.to_c_array(list(map(list, zip(*c_mux_matrix)))),
        )

    # Calibration
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

    # Wear leveling
    wear_leveling = kb_json.wear_leveling
    wl_virtual_size = (
        wear_leveling and wear_leveling.virtual_size
    ) or utils.compute_min_wl_virtual_size(kb_json)
    wl_write_log_size = (wear_leveling and wear_leveling.write_log_size) or 65536
    wl_backing_store_size = wl_virtual_size + wl_write_log_size
    build_flags.define("WL_VIRTUAL_SIZE", wl_virtual_size)
    build_flags.define("WL_WRITE_LOG_SIZE", wl_write_log_size)

    wl_base_address = flash_size - driver.metadata.flash.round_up_to_flash_sectors(
        wl_backing_store_size
    )
    build_flags.define("WL_BASE_ADDRESS", wl_base_address)

    # Keyboard layout
    build_flags.define("NUM_PROFILES", kb_json.num_profiles)
    build_flags.define("NUM_LAYERS", kb_json.num_layers)
    build_flags.define("NUM_KEYS", kb_json.num_keys)
    build_flags.define("NUM_ADVANCED_KEYS", kb_json.num_advanced_keys)

    default_keymaps = utils.resolve_default_keymaps(kb_json)
    build_flags.define("DEFAULT_KEYMAPS", utils.to_c_array(default_keymaps))

    # Actuation
    if kb_json.actuation is not None:
        act = kb_json.actuation
        if act.actuation_point_um is not None:
            build_flags.define("DEFAULT_ACTUATION_POINT", act.actuation_point_um)
        if act.rt_sensitivity_um is not None:
            build_flags.define("DEFAULT_RT_SENSITIVITY", act.rt_sensitivity_um)
        if act.rt_deadzone_um is not None:
            build_flags.define("DEFAULT_RT_DEADZONE", act.rt_deadzone_um)

    # Encoders
    if kb_json.encoders is not None:
        encoders = kb_json.encoders
        build_flags.define("ENCODER_ENABLE")
        build_flags.define("ENCODER_COUNT", len(encoders))

        encoder_timers, encoder_ports, encoder_pin_nums, encoder_pin_muxes = [], [], [], []
        encoder_keys, encoder_resolutions = [], []
        encoder_button_ports, encoder_button_pins, encoder_button_keys = [], [], []

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

    # Output CMake file
    flags = build_flags.get_flags()
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w") as f:
        f.write("# Auto-generated by make_cmake.py - DO NOT EDIT\n")
        f.write(f"# Keyboard: {keyboard}  MCU: {kb_json.mcu}\n\n")
        f.write("set(BOARD_BUILD_FLAGS\n")
        f.write(to_cmake_string(flags))
        f.write("\n)\n\n")
        f.write(f'set(BOARD_DRIVER_DIR "{mcu_dir}")\n')
        f.write(f'set(BOARD_KEYBOARD_DIR "keyboards/{keyboard}")\n')

    print(f"Generated {output_path} with {len(flags)} defines.")


if __name__ == "__main__":
    main()
