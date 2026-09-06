/**
 * i2c_bus.h — the shared I2C bus (GPIO6/7 -> I2C1)
 *
 * The ES8311 codec, the QMI8658 IMU and the PCF85063 RTC all sit on this bus;
 * we use only the codec (see docs/ARCHITECTURE.md).
 *
 * The function names deliberately match Waveshare's DEV_* API, so es8311.c
 * can be used almost unmodified.
 */
#ifndef POKEBIRD_I2C_BUS_H
#define POKEBIRD_I2C_BUS_H

#include <stdbool.h>
#include <stdint.h>

/** Start I2C1 with the pins and speed from board_config.h. Calling it more
 *  than once is harmless. */
void pb_i2c_init(void);

/** Test whether a device answers on the bus (an address probe). */
bool pb_i2c_probe(uint8_t addr);

/** Touch's SEPARATE bus (GPIO32/33 -> I2C0). Calling it more than once is
 *  harmless. */
void pb_tp_i2c_init(void);

/** Address probe on the touch bus. */
bool pb_tp_i2c_probe(uint8_t addr);

/* The API es8311.c expects */
void    DEV_I2C_Write(uint8_t addr, uint8_t reg, uint8_t value);
uint8_t DEV_I2C_ReadByte(uint8_t addr, uint8_t reg);

#endif /* POKEBIRD_I2C_BUS_H */
