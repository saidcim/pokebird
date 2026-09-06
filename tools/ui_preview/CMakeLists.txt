# UI preview — compiles THE DEVICE'S screen code on the host and renders PNGs.
#
# WHY A SEPARATE PROJECT: the main CMakeLists forces the ARM cross compiler.
# This target runs on the PC and shows what the UI looks like without
# flashing the board or asking anyone to look at the screen.
#
#   cmake -S tools/ui_preview -B tools/ui_preview/build -G Ninja
#   cmake --build tools/ui_preview/build
#   ./tools/ui_preview/build/ui_preview && python tools/ui_preview/ppm_png.py
cmake_minimum_required(VERSION 3.16)
project(pokebird_ui_preview C)

set(CMAKE_C_STANDARD 11)
set(KOK ${CMAKE_CURRENT_LIST_DIR}/../..)

# The SAME lv_conf.h as the device, so the fonts, colour depth and drawing
# settings match exactly; otherwise the preview shows the wrong thing.
set(LV_BUILD_CONF_PATH ${KOK}/firmware/src/ui/lv_conf.h CACHE PATH "" FORCE)
option(CONFIG_LV_BUILD_DEMOS "" OFF)
option(CONFIG_LV_BUILD_EXAMPLES "" OFF)
add_subdirectory(${KOK}/third_party/lvgl lvgl)

add_executable(ui_preview
    preview.c
    # The device's screen code ITSELF — nothing is reimplemented.
    ${KOK}/firmware/src/ui/theme.c
    ${KOK}/firmware/src/ui/text.c
    ${KOK}/firmware/src/ui/screen_listen.c
    ${KOK}/firmware/src/ui/screen_log.c
    # Spektrogram panele DOGRUDAN yaziyor; onizleme pb_lcd_blit yerine
    # kendi framebuffer yazicisini veriyor (preview.c).
    ${KOK}/firmware/src/ui/spectrogram.c
    ${KOK}/firmware/src/ui/fonts/pb_font_name_18.c
    ${KOK}/firmware/src/ui/fonts/pb_font_bold_13.c
    ${KOK}/firmware/src/ui/fonts/pb_font_narrow_11.c
    ${KOK}/firmware/src/ui/fonts/pb_font_mono_10.c
)

# shim/ must come FIRST: that is where the fake pico/stdlib.h is found.
target_include_directories(ui_preview PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/shim
    ${KOK}/firmware/src
)
target_link_libraries(ui_preview PRIVATE lvgl)
target_compile_options(ui_preview PRIVATE -Wall -Wextra -Wno-unused-parameter)
