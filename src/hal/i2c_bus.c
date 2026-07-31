#include "hal/i2c_bus.h"

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "board_config.h"

static bool s_inited = false;

void pb_i2c_init(void) {
    if (s_inited) return;

    i2c_init(PB_I2C_INST, PB_I2C_BAUD);
    gpio_set_function(PB_PIN_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PB_PIN_I2C_SCL, GPIO_FUNC_I2C);
    /* Kartta hat çekme dirençleri var; dahili pull-up'lar yine de zarar vermez
     * ve kart dışı bir sorunu maskelemeden hattı belirsizlikten kurtarır. */
    gpio_pull_up(PB_PIN_I2C_SDA);
    gpio_pull_up(PB_PIN_I2C_SCL);

    s_inited = true;
}

bool pb_i2c_probe(uint8_t addr) {
    uint8_t dummy;
    int r = i2c_read_timeout_us(PB_I2C_INST, addr, &dummy, 1, false, 10000);
    return r >= 0;
}

void DEV_I2C_Write(uint8_t addr, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    i2c_write_timeout_us(PB_I2C_INST, addr, buf, 2, false, 10000);
}

uint8_t DEV_I2C_ReadByte(uint8_t addr, uint8_t reg) {
    uint8_t value = 0;
    /* Register adresini yaz, STOP gönderme (true = hattı tut), sonra oku. */
    if (i2c_write_timeout_us(PB_I2C_INST, addr, &reg, 1, true, 10000) < 0) {
        return 0;
    }
    if (i2c_read_timeout_us(PB_I2C_INST, addr, &value, 1, false, 10000) < 0) {
        return 0;
    }
    return value;
}
