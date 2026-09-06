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
    /* The board has bus pull-up resistors already; the internal pull-ups do
     * no harm and keep the bus out of an undefined state without masking an
     * off-board problem. */
    gpio_pull_up(PB_PIN_I2C_SDA);
    gpio_pull_up(PB_PIN_I2C_SCL);

    s_inited = true;
}

/* Touch is on a SEPARATE I2C bus (i2c0, GPIO32/33). The same chip
 * (AXS15231B) also drives the display, but the display is on QSPI; this bus
 * is for touch alone. It is completely independent of i2c1, where the
 * ES8311, IMU and RTC live. */
static bool s_tp_inited = false;

/**
 * Recover the I2C bus from a stuck slave.
 *
 * If the master is interrupted mid-transfer, a slave can keep holding SDA
 * low, and from then on every read on the bus returns garbage. The recovery
 * is the standard one: drive the pins by hand, clock SCL until SDA is
 * released, then generate a proper STOP.
 *
 * WHY THIS IS NEEDED: the touch controller (AXS15231B) got into exactly this
 * state. The first read returned real data and every one after it returned a
 * constant 0xDB, and reflashing did not help — the RP2350 resets but the
 * touch chip does not (LCD_RST/GPIO34 cannot be pulled low).
 */
static void i2c_bus_recover(uint sda, uint scl) {
    gpio_set_function(sda, GPIO_FUNC_SIO);
    gpio_set_function(scl, GPIO_FUNC_SIO);
    gpio_set_dir(sda, GPIO_IN);          /* release SDA, listen to the slave */
    gpio_pull_up(sda);
    gpio_set_dir(scl, GPIO_OUT);
    gpio_put(scl, 1);
    sleep_us(10);

    /* Clock until SDA is released. One byte plus ACK is 9 cycles, which
     * would be enough; we leave some margin. */
    for (int i = 0; i < 16 && !gpio_get(sda); i++) {
        gpio_put(scl, 0); sleep_us(5);
        gpio_put(scl, 1); sleep_us(5);
    }

    /* STOP: SDA goes low to high while SCL is high. */
    gpio_set_dir(sda, GPIO_OUT);
    gpio_put(sda, 0); sleep_us(5);
    gpio_put(scl, 1); sleep_us(5);
    gpio_set_dir(sda, GPIO_IN);          /* the pull-up takes SDA high */
    sleep_us(10);
}

void pb_tp_i2c_init(void) {
    if (s_tp_inited) return;

    i2c_bus_recover(PB_PIN_TP_SDA, PB_PIN_TP_SCL);

    i2c_init(PB_TP_I2C_INST, PB_TP_I2C_BAUD);
    gpio_set_function(PB_PIN_TP_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PB_PIN_TP_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PB_PIN_TP_SDA);
    gpio_pull_up(PB_PIN_TP_SCL);

    s_tp_inited = true;
}

bool pb_tp_i2c_probe(uint8_t addr) {
    uint8_t dummy;
    int r = i2c_read_timeout_us(PB_TP_I2C_INST, addr, &dummy, 1, false, 10000);
    return r >= 0;
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
    /* Write the register address, do not send STOP (true = hold the bus),
     * then read. */
    if (i2c_write_timeout_us(PB_I2C_INST, addr, &reg, 1, true, 10000) < 0) {
        return 0;
    }
    if (i2c_read_timeout_us(PB_I2C_INST, addr, &value, 1, false, 10000) < 0) {
        return 0;
    }
    return value;
}
