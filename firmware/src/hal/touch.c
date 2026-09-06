#include "hal/touch.h"

#include <string.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include "board_config.h"
#include "hal/i2c_bus.h"

/* Okuma isteği. Baytların çoğunun anlamı belgesiz (veri sayfası elimizde yok)
 * ama 8. bayt (indeks 7) OKUNACAK BAYT SAYISI ve paketin uzunluğuyla birebir
 * uyuşmak zorunda — uyuşmazsa çip senkronu kaybedip sabit 0xDB döndürüyor.
 * Kaynak: rsvpnano/src/input/TouchHandler.cpp (aynı panel, çalışan sürücü). */
static const uint8_t TP_READ_CMD[11] = {
    0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, PB_TOUCH_PACKET_LEN, 0x00, 0x00, 0x00
};

#define TP_TIMEOUT_US 5000

/* Aynı karede birden fazla yerden okunuyor (LVGL giriş sürücüsü + arayüz
 * kodu). Kısa bir pencerede aynı sonucu döndürüp I2C'ye tek noktadan
 * gidiyoruz; iç içe geçen komut+okuma dizisi çipi bozar. */
#define TP_CACHE_US 8000

static uint8_t          s_raw[PB_TOUCH_PACKET_LEN];
static bool             s_ready = false;
static pb_touch_state_t s_last;
static uint32_t         s_last_us = 0;
static bool             s_have_last = false;

bool pb_touch_init(void) {
    pb_tp_i2c_init();
    memset(s_raw, 0, sizeof(s_raw));
    memset(&s_last, 0, sizeof(s_last));
    s_have_last = false;
    s_ready = pb_tp_i2c_probe(PB_TP_I2C_ADDR);
    return s_ready;
}

pb_touch_state_t pb_touch_read(void) {
    uint32_t now = time_us_32();
    if (s_have_last && (uint32_t)(now - s_last_us) < TP_CACHE_US) {
        return s_last;
    }

    pb_touch_state_t st;
    memset(&st, 0, sizeof(st));

    /* Komutu yaz (STOP gönderme), hemen ardından paketi oku. */
    int w = i2c_write_timeout_us(PB_TP_I2C_INST, PB_TP_I2C_ADDR,
                                 TP_READ_CMD, sizeof(TP_READ_CMD),
                                 true, TP_TIMEOUT_US);
    int r = (w < 0) ? -1
                    : i2c_read_timeout_us(PB_TP_I2C_INST, PB_TP_I2C_ADDR,
                                          s_raw, PB_TOUCH_PACKET_LEN,
                                          false, TP_TIMEOUT_US);
    if (w < 0 || r < 0) {
        s_last = st; s_last_us = now; s_have_last = true;
        return st;
    }

    st.ok = true;

    /* Parmak sayısı bayt 1'de. 5 ve üstü geçersiz — çip ara sıra çöp
     * değer veriyor, dokunulmadı sayıyoruz (rsvpnano da öyle yapıyor). */
    uint8_t n = s_raw[1];
    st.fingers = (n == 0 || n >= 5) ? 0 : n;

    /* Koordinatlar 12 bit; üst nibble bayrak taşıyor. */
    st.p.raw_x = (uint16_t)(((s_raw[2] & 0x0F) << 8) | s_raw[3]);
    st.p.raw_y = (uint16_t)(((s_raw[4] & 0x0F) << 8) | s_raw[5]);

    s_last = st;
    s_last_us = now;
    s_have_last = true;
    return st;
}

void pb_touch_last_raw(uint8_t out[PB_TOUCH_PACKET_LEN]) {
    memcpy(out, s_raw, PB_TOUCH_PACKET_LEN);
}
