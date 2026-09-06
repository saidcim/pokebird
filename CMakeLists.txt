cmake_minimum_required(VERSION 3.13)

# ── Toolchain ──────────────────────────────────────────────────────────────
# The Pico SDK is vendored under third_party/; ARM GCC 14.3 comes from the
# PlatformIO packages. Both are configured in cmake/toolchain.cmake so that
# "cmake -B build" works without any extra environment variables.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain.cmake)

set(PICO_BOARD pokebird_rp2350b)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/firmware/boards)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake)

project(pokebird C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Must come BEFORE pico_sdk_init() — we point the SDK at a working picotool
# before it tries to build its own. The reasoning is inside the file.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/picotool.cmake)

pico_sdk_init()

add_executable(pokebird
    firmware/src/main.c
    firmware/src/hal/audio_i2s.c
    firmware/src/hal/es8311.c
    firmware/src/hal/i2c_bus.c
    # The vendor's Touch.c is DELIBERATELY not used: its I2C calls have no
    # timeout, so a touch controller that fails to ACK locks up the whole
    # device. firmware/src/hal/touch.c keeps the protocol but adds timeouts.
    firmware/src/hal/touch.c
    firmware/src/hal/display/qspi_pio.c
    firmware/src/hal/display/dev_config.c
    firmware/src/hal/display/lcd_blit.c
    firmware/src/hal/display/LCD_3in49.c
    firmware/src/dsp/fft.c
    firmware/src/dsp/mel.c
    firmware/src/dsp/gate.c
    firmware/src/ui/spectrogram.c
    firmware/src/ui/lv_port.c
    firmware/src/ui/theme.c
    firmware/src/ui/text.c
    firmware/src/ui/screen_listen.c
    firmware/src/ui/screen_log.c
    firmware/src/ui/interface.c
    # The fonts are GENERATED (tools/generate_fonts.py). LVGL's bundled
    # Montserrat has no accented Latin letters; these do (verified during
    # generation).
    firmware/src/ui/fonts/pb_font_name_18.c
    firmware/src/ui/fonts/pb_font_bold_13.c
    firmware/src/ui/fonts/pb_font_narrow_11.c
    firmware/src/ui/fonts/pb_font_mono_10.c
    firmware/src/ai/species_net.cc
    firmware/src/ai/binary_net.cc
    firmware/src/ai/recognizer.c
    firmware/src/ai/decision.c
    firmware/src/ai/tflm_port.cc
)

# ── TFLM + CMSIS-NN ────────────────────────────────────────────────────────
# Defines the `tflm` static library. Must come AFTER pico_sdk_init(): it
# inherits compiler flags from the SDK's targets (pico_base_headers).
include(${CMAKE_CURRENT_LIST_DIR}/cmake/tflm.cmake)

# ── LVGL v9 ────────────────────────────────────────────────────────────────
# Cloned under third_party/lvgl (not committed, see .gitignore). If missing:
#   git clone -b v9.3.0 --depth 1 https://github.com/lvgl/lvgl.git third_party/lvgl
# LVGL defines the `lvgl` target in its own CMakeLists; it is given an
# absolute path so it reads its settings from firmware/src/ui/lv_conf.h.
# NOTE: the CMake variable is LV_BUILD_CONF_PATH. The LV_CONF_PATH seen in the
# docs and examples is the name of a COMPILER macro LVGL generates itself; it
# has no CMake-side equivalent and is silently ignored if passed.
set(LV_BUILD_CONF_PATH ${CMAKE_CURRENT_LIST_DIR}/firmware/src/ui/lv_conf.h CACHE PATH "" FORCE)

# Do not build the demos and examples. LV_BUILD_EXAMPLES in lv_conf.h is not
# enough: the CMake side sets them up as separate targets and links them
# PUBLIC to `lvgl`. They also fail to compile because they use widgets we
# disabled (lv_spinbox -> LV_USE_TEXTAREA).
option(CONFIG_LV_BUILD_DEMOS "" OFF)
option(CONFIG_LV_BUILD_EXAMPLES "" OFF)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/third_party/lvgl lvgl)

target_include_directories(pokebird PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/firmware/src
    ${CMAKE_CURRENT_LIST_DIR}/firmware/src/hal/display
)

# Build the PIO programs. The generated audio_i2s.pio.h goes to the build dir.
pico_generate_pio_header(pokebird ${CMAKE_CURRENT_LIST_DIR}/firmware/src/hal/audio_i2s.pio)
pico_generate_pio_header(pokebird ${CMAKE_CURRENT_LIST_DIR}/firmware/src/hal/display/qspi.pio)

target_link_libraries(pokebird PRIVATE
    pico_stdlib
    pico_unique_id
    hardware_pwm
    hardware_i2c
    hardware_pio
    hardware_dma
    pico_multicore
    lvgl
    tflm
)

# Debug output over USB CDC. The board has no easily accessible serial port
# besides USB (the UART pins are on the P3 header and need soldering).
pico_enable_stdio_usb(pokebird 1)
pico_enable_stdio_uart(pokebird 0)

# Produce the .uf2 — drag and drop onto the drive that appears after
# BOOT+RESET.
pico_add_extra_outputs(pokebird)

target_compile_options(pokebird PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

# TFLM tensor arena size. Left empty, the measured default in
# firmware/src/ai/species_net.cc (120 KB) applies. Overriding it here serves
# two purposes: growing it temporarily while measuring a new model, and moving
# the bss size as a single variable to separate layout-induced regressions
# with an A/B comparison.
#   cmake -S . -B build -DPB_TFLM_ARENA_BYTES=4096
set(PB_TFLM_ARENA_BYTES "" CACHE STRING "TFLM tensor arena size (bytes)")
if(PB_TFLM_ARENA_BYTES)
    target_compile_definitions(pokebird PRIVATE
        PB_TFLM_ARENA_BYTES=${PB_TFLM_ARENA_BYTES})
endif()

# The stage-1 binary net's arena — same reasoning, in
# firmware/src/ai/binary_net.cc.
set(PB_BINARY_ARENA_BYTES "" CACHE STRING "Binary net TFLM arena size (bytes)")
if(PB_BINARY_ARENA_BYTES)
    target_compile_definitions(pokebird PRIVATE
        PB_BINARY_ARENA_BYTES=${PB_BINARY_ARENA_BYTES})
endif()
