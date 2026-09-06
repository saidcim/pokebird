/*
 * pokebird_rp2350b.h - Pico SDK board definition for the Waveshare
 * RP2350-Touch-LCD-3.49
 *
 * The SDK has no ready-made header for this board. We define our own board
 * file so the RP2350B (48 GPIO) variant and the 16 MB of flash are selected
 * correctly.
 *
 * The pin details live in firmware/src/board_config.h; only the definitions
 * the SDK's own startup code needs are here.
 *
 * -----------------------------------------------------
 * NOTE: THIS HEADER IS ALSO READ BY THE ASSEMBLER,
 *       SO IT MAY CONTAIN PREPROCESSOR DIRECTIVES ONLY.
 * -----------------------------------------------------
 */
#ifndef _BOARDS_POKEBIRD_RP2350B_H
#define _BOARDS_POKEBIRD_RP2350B_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection
#define POKEBIRD_RP2350B

// --- RP2350 VARYANTI ---
// 0 = RP2350B (QFN-80, 48 GPIO). This board uses the B variant; GPIO40+
// (BAT_ADC) exists only on the B, so this line is mandatory.
#define PICO_RP2350A 0

// --- FLASH: PY25Q128HA, 128 Mbit = 16 MB ---
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

// --- LED YOK ---
// The board's only LED (LED1) is the ETA6098 charge status LED; it is not
// wired to a GPIO and cannot be controlled from software.
// PICO_DEFAULT_LED_PIN is deliberately left undefined - the display backlight
// serves as the liveness indicator instead (see firmware/src/main.c).

// --- UART: on the free P3 header pins, for debugging ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 12
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 13
#endif

// --- I2C: the ES8311 codec control bus (GPIO6/7 -> I2C1) ---
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 1
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 6
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 7
#endif

// --- SPI: SD kart (GPIO26/27/28 -> SPI1) ---
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 1
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 26
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 27
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN 28
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 31
#endif

// --- Dahili flash (QSPI) ---
#ifndef PICO_BOOT_STAGE2_CHOOSE_W25Q080
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#endif
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

// The SDK's default RAM/stack settings suit 520 KB of SRAM.

#endif /* _BOARDS_POKEBIRD_RP2350B_H */
