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
    src/hal/display/qspi_pio.c
    src/hal/display/dev_config.c
    src/hal/display/LCD_3in49.c
    src/hal/display/Touch.c
    src/dsp/fft.c
    src/ui/spectrogram.c
)

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
