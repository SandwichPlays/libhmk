# libhmk CMakeLists.txt for HPM5300 series (HPM5361ICF1)
#
# Prerequisites:
#   1. Install HPM SDK from https://github.com/hpmicro/sdk_env
#   2. Set HPM_SDK_BASE environment variable:
#        set HPM_SDK_BASE=C:\path\to\hpm_sdk
#   3. Set BOARD to the target board name in the HPM SDK:
#        cmake -DBOARD=hpm5300evklite -DKEYBOARD=sandowo_plus -B build
#      (or whatever HPM5361 board definition is appropriate)
#   4. Build:
#        cmake --build build
#
# To add support for a new keyboard, create keyboards/<name>/keyboard.json
# following the existing examples, set KEYBOARD=<name> in the CMake command.

cmake_minimum_required(VERSION 3.13)

# --- HPM SDK Integration ---
find_package(hpm-sdk REQUIRED HINTS $ENV{HPM_SDK_BASE})

# --- Project ---
set(KEYBOARD "sandowo_plus" CACHE STRING "Keyboard to build (keyboard.json name)")
project(libhmk_${KEYBOARD} C)

# --- Pre-build: Generate board defines from keyboard.json ---
set(BOARD_CONFIG_CMAKE "${CMAKE_BINARY_DIR}/board_config.cmake")

execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/make_cmake.py"
            "${KEYBOARD}" "${BOARD_CONFIG_CMAKE}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE MAKE_CMAKE_RESULT
)

if(NOT MAKE_CMAKE_RESULT EQUAL 0)
    message(FATAL_ERROR "make_cmake.py failed for keyboard '${KEYBOARD}'")
endif()

include("${BOARD_CONFIG_CMAKE}")

# --- Source Files ---
sdk_app_src(
    src/main.c
    src/layout.c
    src/matrix.c
    src/migration.c
    src/usb_descriptors.c
    src/wear_leveling.c
    src/commands.c
    src/encoder.c

    # HPM5300 hardware abstraction layer
    ${BOARD_DRIVER_DIR}/analog.c
    ${BOARD_DRIVER_DIR}/board.c
    ${BOARD_DRIVER_DIR}/crc32.c
    ${BOARD_DRIVER_DIR}/flash.c
    ${BOARD_DRIVER_DIR}/timer.c
)

# --- Include Paths ---
sdk_inc(
    include
    ${BOARD_DRIVER_DIR}
    ${BOARD_KEYBOARD_DIR}
)

# --- Compile Definitions from keyboard.json ---
foreach(flag IN LISTS BOARD_BUILD_FLAGS)
    sdk_compile_options(${flag})
endforeach()

# --- HPM SDK Dependencies ---
sdk_link_libraries(
    hpm_sdk::adc16
    hpm_sdk::crc
    hpm_sdk::dma
    hpm_sdk::gptmr
    hpm_sdk::gpio
    hpm_sdk::clock
    hpm_sdk::pllctlv2
    hpm_sdk::interrupt
    hpm_sdk::l1c
    hpm_sdk::romapi
)

# --- TinyUSB ---
# TinyUSB is included via HPM SDK dependency or added as a subdirectory
# If not bundled with HPM SDK, add it:
# add_subdirectory(lib/tinyusb)
# sdk_link_libraries(tinyusb_device)

generate_ide_projects()
