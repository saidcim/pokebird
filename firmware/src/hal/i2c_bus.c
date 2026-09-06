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

/* Dokunmatik AYRI bir I2C hattında (i2c0, GPIO32/33). Aynı çip (AXS15231B)
 * ekranı da sürüyor ama ekran QSPI'de; bu hat yalnızca dokunmatik için.
 * Kartta ES8311/IMU/RTC'nin bulunduğu i2c1'den tamamen bağımsız. */
static bool s_tp_inited = false;

/**
 * I2C hattını takılı kalmış bir slave'den kurtar.
 *
 * Bir slave, aktarımın ortasında master kesilirse SDA'yı aşağıda tutmaya
 * devam edebilir; o andan sonra hattaki her okuma çöp döner. Kurtarma yolu
 * standart: pinleri elle sürüp SDA serbest kalana kadar SCL'e darbe
 * göndermek, sonra düzgün bir STOP üretmek.
 *
 * NEDEN GEREKLİ: dokunmatik (AXS15231B) tam bu duruma giriyordu. İlk okuma
 * gerçek veri veriyor, sonrasında sabit 0xDB dönüyor ve yeniden yükleme
 * kurtarmıyordu — RP2350 resetleniyor ama dokunmatik çipi resetlenmiyor
 * (LCD_RST/GPIO34 aşağı çekilemiyor, bkz. lastsession.md §5.9b).
 */
static void i2c_bus_recover(uint sda, uint scl) {
    gpio_set_function(sda, GPIO_FUNC_SIO);
    gpio_set_function(scl, GPIO_FUNC_SIO);
    gpio_set_dir(sda, GPIO_IN);          /* SDA'yı bırak, slave'i dinle */
    gpio_pull_up(sda);
    gpio_set_dir(scl, GPIO_OUT);
    gpio_put(scl, 1);
    sleep_us(10);

    /* SDA serbest kalana kadar darbe. Bir bayt + ACK = 9 çevrim yeter,
     * pay bırakıyoruz. */
    for (int i = 0; i < 16 && !gpio_get(sda); i++) {
        gpio_put(scl, 0); sleep_us(5);
        gpio_put(scl, 1); sleep_us(5);
    }

    /* STOP: SCL yüksekken SDA düşükten yükseğe. */
    gpio_set_dir(sda, GPIO_OUT);
    gpio_put(sda, 0); sleep_us(5);
    gpio_put(scl, 1); sleep_us(5);
    gpio_set_dir(sda, GPIO_IN);          /* pull-up SDA'yı yukarı çeker */
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
    /* Register adresini yaz, STOP gönderme (true = hattı tut), sonra oku. */
    if (i2c_write_timeout_us(PB_I2C_INST, addr, &reg, 1, true, 10000) < 0) {
        return 0;
    }
    if (i2c_read_timeout_us(PB_I2C_INST, addr, &value, 1, false, 10000) < 0) {
        return 0;
    }
    return value;
}
