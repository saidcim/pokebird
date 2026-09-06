/**
 * touch.h — AXS15231B capacitive touch (I2C0, GPIO32/33, address 0x3B)
 *
 * WHY NOT THE VENDOR'S `Touch.c` — two separate reasons:
 *
 * 1. Waveshare's Touch.c uses `i2c_write_blocking` / `i2c_read_blocking` with
 *    no timeout. If the touch controller fails to ACK, the call never returns
 *    and the WHOLE device locks up. The audio pipeline is real-time; that is
 *    not a risk worth taking.
 *
 * 2. More importantly: Waveshare's read protocol is WRONG. Byte 7 of the
 *    command sequence is the number of bytes to read; Waveshare writes 0x0E
 *    there and then reads 32 bytes. That mismatch desynchronises the chip —
 *    measured on the board, every read after the first returned 0xDB. The
 *    working driver for the same panel
 *    (rsvpnano/src/input/TouchHandler.cpp) writes 0x08 and reads 8 bytes.
 *
 * COORDINATES: the chip already reports in landscape — raw x is the long axis
 * (0..640) and raw y the short axis (0..172). Whether mirroring is needed is
 * MEASURED with the `t` command, not assumed.
 */
#ifndef POKEBIRD_TOUCH_H
#define POKEBIRD_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

/** The packet length the chip returns in one read. It must MATCH the length
 *  byte in the command; a mismatch desynchronises the chip. */
#define PB_TOUCH_PACKET_LEN 8

typedef struct {
    uint16_t raw_x;     /**< long axis, 0..640 */
    uint16_t raw_y;     /**< short axis, 0..172 */
} pb_touch_point_t;

typedef struct {
    bool             ok;        /**< did the I2C transaction succeed */
    uint8_t          fingers;   /**< 0 = not being touched */
    pb_touch_point_t p;         /**< the 8-byte packet carries a single point */
} pb_touch_state_t;

/** Start I2C0 and confirm the chip responds. */
bool pb_touch_init(void);

/** Read the current state. Returns .ok = false if the bus does not respond;
 *  never blocks. */
pb_touch_state_t pb_touch_read(void);

/** The raw packet from the last read — for diagnostics. */
void pb_touch_last_raw(uint8_t out[PB_TOUCH_PACKET_LEN]);

#endif /* POKEBIRD_TOUCH_H */
