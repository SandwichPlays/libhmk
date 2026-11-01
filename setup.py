import sys

sys.path.append("scripts")

import argparse
import configparser
import os
import scripts.utils as utils
import scripts.validate as validate


def create_env_section(keyboard: str) -> dict[str, str] | None:
    driver = utils.get_driver(keyboard)
    if driver.platformio is None:
        return None

    build_flags = ["${env.build_flags}"]
    build_src_flags = [
        "${env.build_src_flags}",
        "-Werror",
        "-Wall",
        "-Wextra",
        "-Wsign-conversion",
        "-Wswitch-default",
        "-Wswitch",
        "-Wdouble-promotion",
        "-Wstrict-prototypes",
        "-Wno-unused-parameter",
    ]
    extra_scripts = [
        "pre:scripts/get_deps.py",
        "pre:scripts/validate.py",
        "pre:scripts/make.py",
        "pre:scripts/metadata.py",
    ]
    lib_deps = ["https://github.com/hathach/tinyusb.git#0.20.0"]

    return {
        "board": driver.platformio.board,
        "board_build.ldscript": driver.platformio.ldscript,
        "build_flags": "\n".join(build_flags),
        "build_src_filter": "-<*> +<core/>",
        "build_src_flags": "\n".join(build_src_flags),
        "extra_scripts": "\n".join(extra_scripts),
        "framework": driver.platformio.framework,
        "lib_deps": "\n".join(lib_deps),
        "platform": driver.platformio.platform,
        "upload_protocol": "dfu",
    }


if __name__ == "__main__":
    keyboards = sorted(
        keyboard
        for keyboard in os.listdir("keyboards")
        if os.path.isdir(os.path.join("keyboards", keyboard))
        and os.path.isfile(os.path.join("keyboards", keyboard, "keyboard.json"))
    )

    parser = argparse.ArgumentParser(description="PlatformIO Project Setup & Validator")
    parser.add_argument(
        "--keyboard", "-k", choices=keyboards, help="Select a single keyboard"
    )
    parser.add_argument(
        "--all", "-a", action="store_true", help="Generate platformio.ini for all PlatformIO keyboards"
    )
    parser.add_argument(
        "--check", "-c", action="store_true", help="Validate keyboard JSON(s) without modifying platformio.ini"
    )
    args = parser.parse_args()

    target_keyboards = keyboards if (args.all or not args.keyboard) else [args.keyboard]

    for kb in target_keyboards:
        validate.validate_keyboard(kb)
        print(f"[OK] Validated keyboards/{kb}/keyboard.json")

    if not args.check:
        pio_config = configparser.ConfigParser()
        pio_config["platformio"] = {
            "src_dir": ".",
            "include_dir": "core/include",
        }
        for kb in target_keyboards:
            sec = create_env_section(kb)
            if sec is not None:
                pio_config[f"env:{kb}"] = sec

        with open("platformio.ini", "w") as f:
            pio_config.write(f)
        print(f"Generated platformio.ini with {len(pio_config.sections()) - 1} environment(s).")
