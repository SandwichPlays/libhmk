#!/usr/bin/env python3
# make_cmake.py - CMake equivalent of make.py for non-PlatformIO targets (e.g. HPM5300)
#
# Usage:
#   python scripts/make_cmake.py <keyboard_name> <output_cmake_file>
#
# Example:
#   python scripts/make_cmake.py sandowo_plus build/board_config.cmake
#
# This generates a CMake include file with all the board-specific compile
# definitions and source list, equivalent to what make.py does for PlatformIO.

import sys
import os

# Add scripts directory to path so we can import our modules
sys.path.insert(0, os.path.dirname(__file__))

import utils
from drivers import *
from schema.keyboard import KeyboardUSBPort


def to_cmake_string(flags: list[str]) -> str:
    """Convert a list of compiler flags to a quoted CMake list string."""
    return "\n".join(f'    "{f}"' for f in flags)


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <keyboard_name> <output.cmake>", file=sys.stderr)
        sys.exit(1)

    keyboard = sys.argv[1]
    output_path = sys.argv[2]

    kb_json = utils.get_kb_json(keyboard)
    driver = utils.get_driver(keyboard)
    driver_name = kb_json.hardware.driver

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
    build_flags.define("BOARD_HSE_VALUE", kb_json.hardware.hse_value)
    build_flags.define("HSE_VALUE", kb_json.hardware.hse_value)

    # USB
    if kb_json.usb.port == KeyboardUSBPort.FULL_SPEED:
        build_flags.define("BOARD_USB_FS")
    else:
        build_flags.define("BOARD_USB_HS")
    build_flags.define("USB_MANUFACTURER_NAME", f'"{kb_json.manufacturer}"')
    build_flags.define("USB_PRODUCT_NAME", f'"{kb_json.name}"')
    build_flags.define("USB_VENDOR_ID", kb_json.usb.vid)
    build_flags.define("USB_PRODUCT_ID", kb_json.usb.pid)

    # ADC
    build_flags.define("ADC_NUM_CHANNELS", len(driver.metadata.adc.input_pins))
    build_flags.define("ADC_RESOLUTION", utils.get_adc_resolution(kb_json, driver))

    if kb_json.analog.invert_adc:
        build_flags.define("MATRIX_INVERT_ADC_VALUES")

    if kb_json.analog.delay is not None:
        build_flags.define("ADC_SAMPLE_DELAY", kb_json.analog.delay)

    # Raw ADC inputs
    if kb_json.analog.raw is not None:
        raw = kb_json.analog.raw
        build_flags.define("ADC_NUM_RAW_INPUTS", len(raw.input))
        build_flags.define(
            "ADC_RAW_INPUT_CHANNELS",
            utils.to_c_array(driver.metadata.adc.to_adc_inputs(raw.input)),
        )
        build_flags.define("ADC_RAW_INPUT_VECTOR", utils.to_c_array(raw.vector))

    # MUX inputs
    if kb_json.analog.mux is not None:
        mux = kb_json.analog.mux
        build_flags.define("ADC_NUM_MUX_INPUTS", len(mux.input))
        build_flags.define(
            "ADC_MUX_INPUT_CHANNELS",
            utils.to_c_array(driver.metadata.adc.to_adc_inputs(mux.input)),
        )
        build_flags.define("ADC_NUM_MUX_SELECT_PINS", len(mux.select))
        ports, pin_nums = driver.metadata.adc.to_gpio_array(mux.select)
        build_flags.define("ADC_MUX_SELECT_PORTS", utils.to_c_array(ports))
        build_flags.define("ADC_MUX_SELECT_PINS", utils.to_c_array(pin_nums))
        build_flags.define(
            "ADC_MUX_INPUT_MATRIX",
            utils.to_c_array(list(map(list, zip(*mux.matrix)))),
        )

    # Calibration
    cal_dict = kb_json.calibration.model_dump()
    travel_val = int(cal_dict.pop("switch_travel") * 10)
    cal_dict["switch_travel"] = [travel_val] * kb_json.keyboard.num_keys
    build_flags.define("DEFAULT_CALIBRATION", utils.to_c_struct(cal_dict))

    # Wear leveling
    wear_leveling = kb_json.wear_leveling
    wl_virtual_size = (wear_leveling and wear_leveling.virtual_size) or 8192
    wl_write_log_size = (wear_leveling and wear_leveling.write_log_size) or 65536
    wl_backing_store_size = wl_virtual_size + wl_write_log_size
    build_flags.define("WL_VIRTUAL_SIZE", wl_virtual_size)
    build_flags.define("WL_WRITE_LOG_SIZE", wl_write_log_size)

    wl_base_address = flash_size - driver.metadata.flash.round_up_to_flash_sectors(
        wl_backing_store_size
    )
    build_flags.define("WL_BASE_ADDRESS", wl_base_address)

    # Keyboard layout
    kb = kb_json.keyboard
    build_flags.define("NUM_PROFILES", kb.num_profiles)
    build_flags.define("NUM_LAYERS", kb.num_layers)
    build_flags.define("NUM_KEYS", kb.num_keys)
    build_flags.define("NUM_ADVANCED_KEYS", kb.num_advanced_keys)

    default_keymaps = utils.resolve_default_keymaps(kb_json)
    build_flags.define("DEFAULT_KEYMAPS", utils.to_c_array(default_keymaps))

    # Actuation
    if kb_json.actuation is not None:
        actuation = kb_json.actuation
        if actuation.actuation_point is not None:
            build_flags.define("ACTUATION_POINT", actuation.actuation_point)

    # Encoders
    if kb_json.encoders is not None:
        encoders = kb_json.encoders
        build_flags.define("ENCODER_ENABLE")
        build_flags.define("ENCODER_COUNT", len(encoders))

        encoder_timers, encoder_ports, encoder_pin_nums = [], [], []
        encoder_keys, encoder_resolutions = [], []
        encoder_button_ports, encoder_button_pins, encoder_button_keys = [], [], []

        for encoder in encoders:
            encoder_timers.append(encoder.timer)
            ports, pin_nums = driver.metadata.adc.to_gpio_array(encoder.pins)
            encoder_ports.append(ports)
            encoder_pin_nums.append(pin_nums)
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
        f.write(f"# Keyboard: {keyboard}  Driver: {driver_name}\n\n")
        f.write("set(BOARD_BUILD_FLAGS\n")
        f.write(to_cmake_string(flags))
        f.write("\n)\n\n")
        f.write(f'set(BOARD_DRIVER_DIR "src/hardware/{driver_name}")\n')
        f.write(f'set(BOARD_KEYBOARD_DIR "keyboards/{keyboard}")\n')

    print(f"Generated {output_path} with {len(flags)} defines.")


if __name__ == "__main__":
    main()
