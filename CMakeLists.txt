cmake_minimum_required(VERSION 3.13)

# ── Derleme zinciri ────────────────────────────────────────────────────────
# Pico SDK third_party/ altına vendor edildi; ARM GCC 14.3 PlatformIO
# paketlerinden geliyor. Her ikisi de cmake/toolchain.cmake içinde ayarlanır,
# böylece "cmake -B build" ek ortam değişkeni gerektirmeden çalışır.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain.cmake)

set(PICO_BOARD pokebird_rp2350b)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake)

project(pokebird C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# pico_sdk_init() ÖNCESİNDE olmalı — SDK kendi picotool'unu derlemeye
# kalkmadan çalışan bir tanesini tanıtıyoruz. Gerekçe dosyanın içinde.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/picotool.cmake)

pico_sdk_init()

add_executable(pokebird
    src/main.c
    src/hal/audio_i2s.c
    src/hal/es8311.c
    src/hal/i2c_bus.c
    # Satıcının Touch.c'si BİLEREK alınmadı: I2C çağrıları zaman aşımsız,
    # dokunmatik ACK vermezse tüm cihazı kilitliyor. Protokolü korunarak
    # zaman aşımlı hâli src/hal/touch.c'ye taşındı.
    src/hal/touch.c
    src/hal/display/qspi_pio.c
    src/hal/display/dev_config.c
    src/hal/display/lcd_blit.c
    src/hal/display/LCD_3in49.c
    src/dsp/fft.c
    src/dsp/mel.c
    src/dsp/gate.c
    src/ui/spectrogram.c
    src/ui/lv_port.c
    src/ai/tur_agi.cc
    src/ai/tanima.c
    src/ai/tflm_port.cc
)

# ── TFLM + CMSIS-NN ────────────────────────────────────────────────────────
# `tflm` statik kütüphanesini tanımlıyor. pico_sdk_init() SONRASINDA olmalı:
# SDK'nın hedeflerinden (pico_base_headers) derleyici bayraklarını miras alıyor.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/tflm.cmake)

# ── LVGL v9 ────────────────────────────────────────────────────────────────
# third_party/lvgl altına klonlu (git'e girmiyor, .gitignore'da). Yoksa:
#   git clone -b v9.3.0 --depth 1 https://github.com/lvgl/lvgl.git third_party/lvgl
# LVGL kendi CMakeLists'inde `lvgl` hedefini tanımlıyor; ayarları
# src/ui/lv_conf.h'den okuması için mutlak yol veriliyor.
# DİKKAT: CMake değişkeninin adı LV_BUILD_CONF_PATH. Belgelerde ve örneklerde
# geçen LV_CONF_PATH, LVGL'in kendi ürettiği DERLEYİCİ makrosunun adı; CMake
# tarafında karşılığı yok ve verilirse sessizce yok sayılır.
set(LV_BUILD_CONF_PATH ${CMAKE_CURRENT_LIST_DIR}/src/ui/lv_conf.h CACHE PATH "" FORCE)

# Demo ve örnekler derlenmesin. lv_conf.h'deki LV_BUILD_EXAMPLES yetmiyor:
# CMake tarafı bunları ayrı hedefler olarak kuruyor ve `lvgl`e PUBLIC
# bağlıyor. Ayrıca kapattığımız bileşenleri kullandıkları için (lv_spinbox
# -> LV_USE_TEXTAREA) derleme hatası veriyorlar.
option(CONFIG_LV_BUILD_DEMOS "" OFF)
option(CONFIG_LV_BUILD_EXAMPLES "" OFF)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/third_party/lvgl lvgl)

target_include_directories(pokebird PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/hal/display
)

# PIO programlarını derle. Üretilen audio_i2s.pio.h build dizinine yazılır.
pico_generate_pio_header(pokebird ${CMAKE_CURRENT_LIST_DIR}/src/hal/audio_i2s.pio)
pico_generate_pio_header(pokebird ${CMAKE_CURRENT_LIST_DIR}/src/hal/display/qspi.pio)

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

# TFLM tensor arena boyutu. Boş bırakılırsa src/ai/tur_agi.cc'deki ölçülmüş
# varsayılan (120 KB) geçerli. Buradan ezmek iki işe yarıyor: yeni bir model
# ölçülürken geçici olarak büyütmek, ve bss boyutunu tek değişken olarak
# oynatıp yerleşim kaynaklı gerilemeleri A/B ile ayırmak (§5.17).
#   cmake -S . -B build -DPB_TFLM_ARENA_BAYT=4096
set(PB_TFLM_ARENA_BAYT "" CACHE STRING "TFLM tensor arena boyutu (bayt)")
if(PB_TFLM_ARENA_BAYT)
    target_compile_definitions(pokebird PRIVATE
        PB_TFLM_ARENA_BAYT=${PB_TFLM_ARENA_BAYT})
endif()
