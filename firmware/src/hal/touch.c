#include "hal/touch.h"

#include <string.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include "board_config.h"
#include "hal/i2c_bus.h"

/* The read request. The meaning of most bytes is undocumented (we have no
 * datasheet), but byte 8 (index 7) is the NUMBER OF BYTES TO READ and must
 * match the packet length exactly — otherwise the chip loses sync and returns
 * a constant 0xDB. Source: rsvpnano/src/input/TouchHandler.cpp (same panel,
 * a working driver). */
static const uint8_t TP_READ_CMD[11] = {
    0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, PB_TOUCH_PACKET_LEN, 0x00, 0x00, 0x00
};

#define TP_TIMEOUT_US 5000

/* This is read from more than one place in the same frame (LVGL's input
 * driver and the UI code). Within a short window we return the same result
 * and touch I2C from a single point; an interleaved command+read sequence
 * corrupts the chip. */
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

    /* Write the command (without STOP), then read the packet immediately. */
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

    /* The finger count is in byte 1. Five or more is invalid — the chip
     * occasionally emits a garbage value, and we treat that as no touch (the
     * working driver does the same). */
    uint8_t n = s_raw[1];
    st.fingers = (n == 0 || n >= 5) ? 0 : n;

    /* The coordinates are 12-bit; the top nibble carries flags. */
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
