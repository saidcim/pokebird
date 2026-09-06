cmake_minimum_required(VERSION 3.13)

# ── Derleme zinciri ────────────────────────────────────────────────────────
# Pico SDK third_party/ altına vendor edildi; ARM GCC 14.3 PlatformIO
# paketlerinden geliyor. Her ikisi de cmake/toolchain.cmake içinde ayarlanır,
# böylece "cmake -B build" ek ortam değişkeni gerektirmeden çalışır.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain.cmake)

set(PICO_BOARD pokebird_rp2350b)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/firmware/boards)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake)

project(pokebird C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# pico_sdk_init() ÖNCESİNDE olmalı — SDK kendi picotool'unu derlemeye
# kalkmadan çalışan bir tanesini tanıtıyoruz. Gerekçe dosyanın içinde.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/picotool.cmake)

pico_sdk_init()

add_executable(pokebird
    firmware/src/main.c
    firmware/src/hal/audio_i2s.c
    firmware/src/hal/es8311.c
    firmware/src/hal/i2c_bus.c
    # Satıcının Touch.c'si BİLEREK alınmadı: I2C çağrıları zaman aşımsız,
    # dokunmatik ACK vermezse tüm cihazı kilitliyor. Protokolü korunarak
    # zaman aşımlı hâli firmware/src/hal/touch.c'ye taşındı.
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
    # Yazı tipleri ÜRETİLMİŞ (tools/generate_fonts.py). LVGL'in gömülü
    # Montserrat'ında Türkçe harfler yok; bunlarda var (üretimde doğrulanıyor).
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
# `tflm` statik kütüphanesini tanımlıyor. pico_sdk_init() SONRASINDA olmalı:
# SDK'nın hedeflerinden (pico_base_headers) derleyici bayraklarını miras alıyor.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/tflm.cmake)

# ── LVGL v9 ────────────────────────────────────────────────────────────────
# third_party/lvgl altına klonlu (git'e girmiyor, .gitignore'da). Yoksa:
#   git clone -b v9.3.0 --depth 1 https://github.com/lvgl/lvgl.git third_party/lvgl
# LVGL kendi CMakeLists'inde `lvgl` hedefini tanımlıyor; ayarları
# firmware/src/ui/lv_conf.h'den okuması için mutlak yol veriliyor.
# DİKKAT: CMake değişkeninin adı LV_BUILD_CONF_PATH. Belgelerde ve örneklerde
# geçen LV_CONF_PATH, LVGL'in kendi ürettiği DERLEYİCİ makrosunun adı; CMake
# tarafında karşılığı yok ve verilirse sessizce yok sayılır.
set(LV_BUILD_CONF_PATH ${CMAKE_CURRENT_LIST_DIR}/firmware/src/ui/lv_conf.h CACHE PATH "" FORCE)

# Demo ve örnekler derlenmesin. lv_conf.h'deki LV_BUILD_EXAMPLES yetmiyor:
# CMake tarafı bunları ayrı hedefler olarak kuruyor ve `lvgl`e PUBLIC
# bağlıyor. Ayrıca kapattığımız bileşenleri kullandıkları için (lv_spinbox
# -> LV_USE_TEXTAREA) derleme hatası veriyorlar.
option(CONFIG_LV_BUILD_DEMOS "" OFF)
option(CONFIG_LV_BUILD_EXAMPLES "" OFF)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/third_party/lvgl lvgl)

target_include_directories(pokebird PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/firmware/src
    ${CMAKE_CURRENT_LIST_DIR}/firmware/src/hal/display
)

# PIO programlarını derle. Üretilen audio_i2s.pio.h build dizinine yazılır.
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

# Hata ayıklama çıktısı USB CDC üzerinden. Kartta USB dışında kolay erişilebilir
# bir seri port yok (UART pinleri P3 başlığında, lehim gerektirir).
pico_enable_stdio_usb(pokebird 1)
pico_enable_stdio_uart(pokebird 0)

# .uf2 üret — BOOT+RESET ile açılan sürücüye sürükle-bırak.
pico_add_extra_outputs(pokebird)

target_compile_options(pokebird PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

# TFLM tensor arena boyutu. Boş bırakılırsa firmware/src/ai/species_net.cc'deki ölçülmüş
# varsayılan (120 KB) geçerli. Buradan ezmek iki işe yarıyor: yeni bir model
# ölçülürken geçici olarak büyütmek, ve bss boyutunu tek değişken olarak
# oynatıp yerleşim kaynaklı gerilemeleri A/B ile ayırmak (§5.17).
#   cmake -S . -B build -DPB_TFLM_ARENA_BAYT=4096
set(PB_TFLM_ARENA_BYTES "" CACHE STRING "TFLM tensor arena boyutu (bayt)")
if(PB_TFLM_ARENA_BYTES)
    target_compile_definitions(pokebird PRIVATE
        PB_TFLM_ARENA_BYTES=${PB_TFLM_ARENA_BYTES})
endif()

# Asama-1 ikili ag arena'si — ayni gerekce, firmware/src/ai/binary_net.cc'de.
set(PB_BINARY_ARENA_BYTES "" CACHE STRING "Ikili ag TFLM tensor arena boyutu (bayt)")
if(PB_BINARY_ARENA_BYTES)
    target_compile_definitions(pokebird PRIVATE
        PB_BINARY_ARENA_BYTES=${PB_BINARY_ARENA_BYTES})
endif()
