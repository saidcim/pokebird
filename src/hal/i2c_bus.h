/**
 * i2c_bus.h — Paylaşımlı I2C hattı (GPIO6/7 -> I2C1)
 *
 * Bu hatta ES8311 codec, QMI8658 IMU ve PCF85063 RTC var; biz yalnızca
 * codec'i kullanıyoruz (bkz. docs/ARCHITECTURE.md §1).
 *
 * Fonksiyon adları Waveshare'in DEV_* API'siyle bilerek aynı; böylece
 * es8311.c neredeyse değiştirilmeden kullanılabiliyor.
 */
#ifndef POKEBIRD_I2C_BUS_H
#define POKEBIRD_I2C_BUS_H

#include <stdbool.h>
#include <stdint.h>

/** I2C1'i board_config.h'deki pin ve hızla başlat. Birden fazla çağrı zararsız. */
void pb_i2c_init(void);

/** Cihazın hatta yanıt verip vermediğini sınar (adres taraması). */
bool pb_i2c_probe(uint8_t addr);

/** Dokunmatiğin AYRI hattı (GPIO32/33 -> I2C0). Birden fazla çağrı zararsız. */
void pb_tp_i2c_init(void);

/** Dokunmatik hattında adres taraması. */
bool pb_tp_i2c_probe(uint8_t addr);

/* es8311.c'nin beklediği API */
void    DEV_I2C_Write(uint8_t addr, uint8_t reg, uint8_t value);
uint8_t DEV_I2C_ReadByte(uint8_t addr, uint8_t reg);

#endif /* POKEBIRD_I2C_BUS_H */
