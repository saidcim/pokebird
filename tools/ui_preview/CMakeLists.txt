# Arayüz önizlemesi — CİHAZDAKİ ekran kodunu host'ta derleyip PNG üretir.
#
# NEDEN AYRI PROJE: ana CMakeLists ARM çapraz derleyicisini zorluyor. Bu
# hedef PC'de çalışır ve arayüzün nasıl göründüğünü karta yüklemeden,
# kullanıcıdan ekrana bakmasını istemeden gösterir.
#
#   cmake -S tools/ui_preview -B tools/ui_preview/build -G Ninja
#   cmake --build tools/ui_preview/build
#   ./tools/ui_preview/build/ui_preview && python tools/ui_preview/ppm_png.py
cmake_minimum_required(VERSION 3.16)
project(pokebird_ui_preview C)

set(CMAKE_C_STANDARD 11)
set(KOK ${CMAKE_CURRENT_LIST_DIR}/../..)

# Cihazla AYNI lv_conf.h — yazı tipi, renk derinliği ve çizim ayarları
# birebir aynı olsun, yoksa önizleme yanlış şeyi gösterir.
set(LV_BUILD_CONF_PATH ${KOK}/firmware/src/ui/lv_conf.h CACHE PATH "" FORCE)
option(CONFIG_LV_BUILD_DEMOS "" OFF)
option(CONFIG_LV_BUILD_EXAMPLES "" OFF)
add_subdirectory(${KOK}/third_party/lvgl lvgl)

add_executable(ui_preview
    preview.c
    # Cihazın ekran kodunun TA KENDİSİ — taklit yok.
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

# shim/ ÖNCE gelmeli: pico/stdlib.h sahtesi oradan bulunacak.
target_include_directories(ui_preview PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/shim
    ${KOK}/firmware/src
)
target_link_libraries(ui_preview PRIVATE lvgl)
target_compile_options(ui_preview PRIVATE -Wall -Wextra -Wno-unused-parameter)
