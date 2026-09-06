/**
 * lcd_blit.h — transferring a rectangle from a small buffer to the display
 *
 * WHY WE HAVE OUR OWN FUNCTION:
 * Waveshare's LCD_3IN49_DisplayWindows() assumes the `Image` pointer it is
 * given is a FULL-SCREEN framebuffer:
 *     pixel_offset = (i * LCD_3IN49.WIDTH + Xstart) * 2;
 * So handing it a small buffer makes it read out of bounds. Our design
 * deliberately rejects a full framebuffer (220 KB, 42% of SRAM), so we need
 * our own function that treats the buffer as a flat, contiguous pixel array.
 *
 * COORDINATES — NOTE:
 * This function works in the panel's NATIVE orientation: X 0..171, Y 0..639.
 * The UI's landscape (640x172) coordinates are converted in the ui/ layer.
 *
 * BYTE ORDER:
 * The panel expects big-endian RGB565. The function does the swap itself; the
 * caller passes normal (little-endian) uint16_t values.
 */
#ifndef POKEBIRD_LCD_BLIT_H
#define POKEBIRD_LCD_BLIT_H

#include <stdint.h>

#define PB_PANEL_W 172
#define PB_PANEL_H 640

/**
 * THE PANEL'S CONTRACT — RASET (0x2B) IS IGNORED (measured)
 *
 * There is no such thing as a row window on this panel. The write cursor's
 * row is determined by two commands and nothing else:
 *   0x2C RAMWR   -> the cursor returns to the TOP row of the column window
 *   0x3C RAMWRC  -> the cursor CONTINUES from where the last write ended
 * The column range is set with CASET (0x2A), and that does work.
 *
 * The consequence: **random access to a rectangle with y>0 is not free.** The
 * driver tracks the cursor; if it is already on the target row it continues
 * for free with RAMWRC, and otherwise it starts from RAMWR and crosses the
 * intervening rows IN BLACK — which means **everything above is erased within
 * that column range.** The correct usage is to draw a frame in order, top to
 * bottom. Code that violates this breaks visibly rather than silently.
 *
 * Writes the w*h pixels in `buf` to the panel at (x,y).
 * `buf` must be row-major and contiguous (w pixels, then the next row).
 * Pixel format: normal RGB565 (little-endian uint16_t).
 */
void pb_lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 const uint16_t *buf);

/**
 * Strided blit — a row and column step can be given while walking the source.
 *
 * WHY: the LVGL UI is LANDSCAPE (640x172) while the panel is PORTRAIT
 * (172x640). The 90-degree rotation between them would normally mean
 * transposing the buffer, i.e. as much RAM again for a second buffer — RAM we
 * do not have.
 *
 * Instead we read the source in TRANSPOSED ORDER: one horizontal row of the
 * panel is one vertical column of the LVGL buffer. Negative steps are valid
 * too, which is how mirroring is handled. The extra buffer cost is ZERO.
 *
 * pixel(row r, column c) = buf[r*row_step + c*col_step]
 *
 * `x/y/w/h` are in the panel's native coordinates. An out-of-bounds request
 * is REJECTED rather than silently clipped: with negative steps, clipping
 * would also require shifting the source origin, and doing that without the
 * caller knowing leads to silent errors.
 */
void pb_lcd_blit_strided(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         const uint16_t *buf, int32_t col_step, int32_t row_step);

/** Fill the whole panel with one colour. */
void pb_lcd_fill(uint16_t color);

/**
 * Stream flat-colour pixels WITHOUT setting the window — for the hybrid-path
 * test.
 *
 * WHY SEPARATE: `pb_lcd_blit` sets the window itself, so the "window command"
 * and the "pixel data" both travel the same path (PIO). Diagnosing a display
 * fault means separating the two: sending the window by bit-bang and the
 * pixels by PIO (or the other way round) to see which one is failing. Here
 * the caller sets the window and this function sends only RAMWR plus data.
 */
void pb_lcd_stream_flat(uint16_t color, uint32_t pixel);

/**
 * Raw pixel streaming — the trio that expresses the panel's REAL contract.
 *
 * On this panel (AXS15231B) the row window (RASET, 0x2B) is IGNORED; the
 * write cursor's row is set only by RAMWR/RAMWRC:
 *   0x2C (RAMWR)  — moves the cursor to the TOP of the column window
 *   0x3C (RAMWRC) — CONTINUES from where the previous write ended
 * The column range is set separately with CASET (0x2A). The source for this:
 * both independent working drivers for this panel (rsvpnano's ESP32 one and
 * the RP2350-PIO one) send no RASET at all.
 *
 * `begin` lowers CS and sends the command, `color` streams a flat colour (as
 * many times as it is called), and `end` raises CS. The caller sets the
 * window.
 */
void pb_lcd_stream_begin(uint8_t ramwr);
void pb_lcd_stream_color(uint16_t color, uint32_t pixel);
/** Stream one row (n pixels, normal RGB565); it swaps the byte order itself. */
void pb_lcd_stream_row(const uint16_t *src, uint32_t n);
void pb_lcd_stream_end(void);

/** Column range (CASET, 0x2A). Inclusive: both x1 and x2 are included. */
void pb_lcd_column_window(uint32_t x1, uint32_t x2);

/**
 * Any code outside this file that sends a command or data to the panel must
 * call this. The driver tracks where the cursor is, and if someone else
 * writes to the panel that knowledge goes stale — the next blit then lands
 * silently in the wrong place.
 */
void pb_lcd_cursor_invalidate(void);

/** Dump the next `count` rows to the serial console as ASCII on their way to
 *  DMA (diagnostic). */
void pb_lcd_request_row_dump(int count);

#endif /* POKEBIRD_LCD_BLIT_H */
