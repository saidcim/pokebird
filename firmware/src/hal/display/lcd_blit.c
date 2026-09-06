#include "lcd_blit.h"

#include <stdio.h>

#include "DEV_Config.h"
#include "LCD_3in49.h"
#include "qspi_pio.h"
#include "hardware/dma.h"

/* Byte-swapped row buffer. A panel row is at most PB_PANEL_W pixels; the same
 * buffer is used when writing a column too (h=1). */
static uint16_t s_row[PB_PANEL_W];

/* ── The panel's write cursor ─────────────────────────────────────────────
 *
 * On this panel RASET (0x2B) is IGNORED — measured on the board. The row
 * position is set by two commands and nothing else:
 *   0x2C RAMWR   -> the cursor returns to the TOP row of the column window
 *   0x3C RAMWRC  -> the cursor CONTINUES from where the last write ended
 * The column range is set with CASET (0x2A), and that does work.
 *
 * The row advances as pixels are written, at the width of the window. If the
 * window is NARROWED the row can be advanced more cheaply, and when the
 * window is widened again and writing continues with RAMWRC the row is
 * PRESERVED (measured). That is what makes positioning cheap: advancing y
 * rows costs 2*y pixels.
 *
 * 2-PIXEL ALIGNMENT: the panel rounds the column range to 2 pixels. Measured:
 * a 1-pixel window (66..66) is effectively 66..67, so 300 pixels advance 150
 * rows rather than 300, and the next write continues one pixel out of
 * alignment and comes out jagged. That is why EVERY window is widened so x1
 * is even and x2 is odd. */
static bool     s_cursor_valid = false;
static uint32_t s_cursor_x1, s_cursor_x2, s_cursor_row;

/* ── The skip strip ───────────────────────────────────────────────────────
 *
 * Positioning uses the panel's columns 0 and 1: y rows' worth of data is
 * written there to walk the cursor down to y. Done naively that would ERASE
 * those two columns. Instead we keep the REAL contents of the two columns
 * here and write the same data back while skipping — which makes the skip
 * completely INVISIBLE without sacrificing a single pixel of screen.
 *
 * The cost is 640*2*2 = 2,560 bytes. The alternative was to drop two columns
 * from the UI (172 -> 170), which would have meant rebuilding the measured
 * orientation mapping and the spectrogram bands from scratch.
 *
 * Any code outside this file that writes to the panel invalidates the strip;
 * while it is invalid a skip writes black (which only happens after
 * diagnostic commands, and the application redraws immediately afterwards). */
static uint16_t s_strip[PB_PANEL_H][2];   /* big-endian, as it goes to the panel */
static bool     s_strip_valid = false;

void pb_lcd_cursor_invalidate(void) {
    s_cursor_valid = false;
    s_strip_valid = false;
}

static void caset_inner(uint32_t x1, uint32_t x2) {
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x2A);       /* CASET — the only window that works here */
    QSPI_DATA_Write(qspi, (x1 >> 8) & 0xff);
    QSPI_DATA_Write(qspi, x1 & 0xff);
    QSPI_DATA_Write(qspi, (x2 >> 8) & 0xff);
    QSPI_DATA_Write(qspi, x2 & 0xff);
    QSPI_Deselect(qspi);
}

static void stream_begin_inner(uint8_t ramwr) {
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, ramwr);          /* 0x2C from the top, 0x3C continue */
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));
}

/* The public versions invalidate the cursor and the strip AUTOMATICALLY. The
 * diagnostic commands drive the panel by hand, and having to remember to
 * invalidate at every call site was an invitation to silent bugs (the next
 * blit would continue with RAMWRC without knowing the cursor had moved). */
void pb_lcd_column_window(uint32_t x1, uint32_t x2) {
    caset_inner(x1, x2);
    pb_lcd_cursor_invalidate();
}

void pb_lcd_stream_begin(uint8_t ramwr) {
    stream_begin_inner(ramwr);
    pb_lcd_cursor_invalidate();
}

/* A dump of the final software stage: `s_row` itself, as handed to DMA.
 * Everything up to here had been measured (source data, transposed read, row
 * stride, alignment, CS timing); this was the one remaining unverified
 * stage. */
static int s_row_dump = 0;
void pb_lcd_request_row_dump(int count) { s_row_dump = count; }

/* ── The transfer buffer — the reference driver's technique ───────────────
 *
 * The working reference driver (rsvpnano's
 * src/drivers/display/axs15231b_pio/axs15231b_pio.cpp, `pushColors`) does NOT
 * send a rectangle row by row; it gathers it into one contiguous buffer and
 * sends it with a **single DMA**:
 *
 *     const size_t byteCount = width * height * sizeof(uint16_t);
 *     dma_channel_configure(..., data, byteCount, true);
 *
 * Our old path opened a separate DMA per row, and in between the bus sat idle
 * with CS low while the CPU swapped byte order. Measured on the board (the
 * `S` command): writing row by row into a narrow window made the content slip
 * on every row (a staircase), while full-width writes did not. The byte
 * order, the window command and the CS timing already matched the reference;
 * this was the only place we diverged.
 *
 * The buffer is 4096 pixels = 8 KB. LVGL's widest flush is 172 columns, so
 * 4096/172 = 23 rows, and for a typical narrow band (32 columns) 128 rows —
 * meaning almost every rectangle fits in a SINGLE DMA. */
#define PB_STACK_PIXELS 4096
static uint16_t s_stack[PB_STACK_PIXELS];

static void send_stack(uint32_t pixel) {
    dma_channel_configure(dma_tx, &c,
                          &qspi.pio->txf[qspi.sm],
                          s_stack,
                          pixel * 2,
                          true);
    while (dma_channel_is_busy(dma_tx)) tight_loop_contents();
}

/** DMA the n pixels in s_row (already byte-swapped) to the panel. */
static void send_row(uint32_t n) {
    if (s_row_dump > 0) {
        s_row_dump--;
        printf("#ROW %lu ", (unsigned long)n);
        for (uint32_t i = 0; i < n; i++) {
            /* s_row is big-endian; swap back to compute brightness */
            uint16_t px = (uint16_t)((s_row[i] >> 8) | (s_row[i] << 8));
            uint32_t l = ((px >> 11) & 0x1F) + ((px >> 6) & 0x1F) + (px & 0x1F);
            putchar(l < 6 ? '.' : (l < 24 ? '+' : '#'));
        }
        putchar('\n');
    }
    dma_channel_configure(dma_tx, &c,
                          &qspi.pio->txf[qspi.sm],
                          s_row,
                          n * 2,              /* byte count (8-bit transfers) */
                          true);
    while (dma_channel_is_busy(dma_tx)) tight_loop_contents();
}

void pb_lcd_stream_color(uint16_t color, uint32_t pixel) {
    const uint16_t be = (uint16_t)((color >> 8) | (color << 8));
    for (uint32_t i = 0; i < PB_PANEL_W; i++) s_row[i] = be;

    while (pixel) {
        uint32_t n = (pixel > PB_PANEL_W) ? PB_PANEL_W : pixel;
        send_row(n);
        pixel -= n;
    }
}

void pb_lcd_stream_row(const uint16_t *src, uint32_t n) {
    if (!src || n == 0 || n > PB_PANEL_W) return;
    for (uint32_t i = 0; i < n; i++) {
        /* The panel wants big-endian RGB565 */
        s_row[i] = (uint16_t)((src[i] >> 8) | (src[i] << 8));
    }
    send_row(n);
}

void pb_lcd_stream_end(void) {
    QSPI_Deselect(qspi);
}

/**
 * Move the cursor to row `y` — INVISIBLY, using columns 0 and 1.
 *
 * Because the window is 2 pixels wide, advancing y rows costs 2*y pixels
 * (at full width it would be 172*y). The data written is the strip's real
 * content, so nothing on screen changes.
 */
static void skip_with_strip(uint32_t y) {
    caset_inner(0, 1);
    stream_begin_inner(0x2C);                        /* row 0 */

    const uint32_t rows_per_pass = PB_PANEL_W / 2;   /* rows that fit in s_row */
    uint32_t written = 0;
    while (written < y) {
        uint32_t n = y - written;
        if (n > rows_per_pass) n = rows_per_pass;
        for (uint32_t r = 0; r < n; r++) {
            s_row[2 * r]     = s_strip_valid ? s_strip[written + r][0] : 0;
            s_row[2 * r + 1] = s_strip_valid ? s_strip[written + r][1] : 0;
        }
        send_row(n * 2);
        written += n;
    }
    pb_lcd_stream_end();
}

/**
 * Move the cursor to (the aligned window x1..x2, row y) and begin streaming
 * (it returns with CS low).
 */
static void position_cursor(uint32_t x1, uint32_t x2, uint32_t y) {
    if (s_cursor_valid && s_cursor_x1 == x1 && s_cursor_x2 == x2 &&
        s_cursor_row == y) {
        stream_begin_inner(0x3C);                 /* RAMWRC — costs nothing */
        return;
    }
    if (y == 0) {
        caset_inner(x1, x2);
        stream_begin_inner(0x2C);
        return;
    }
    skip_with_strip(y);                           /* cursor -> row y */
    caset_inner(x1, x2);
    stream_begin_inner(0x3C);                     /* continue, preserving the row */
}

static void mark_cursor(uint32_t x1, uint32_t x2, uint32_t row) {
    s_cursor_valid = true;
    s_cursor_x1 = x1;
    s_cursor_x2 = x2;
    s_cursor_row = row;
}

/**
 * Align the window to 2 pixels. The returned range has an even x1 and an odd
 * x2. `left` and `right` report how much edge padding is needed (0 or 1).
 */
static void align_window(uint32_t x, uint32_t w,
                         uint32_t *x1, uint32_t *x2,
                         uint32_t *left, uint32_t *right) {
    *x1 = x & ~1u;
    *x2 = (x + w - 1) | 1u;
    if (*x2 >= PB_PANEL_W) *x2 = PB_PANEL_W - 1;   /* 171 is already odd */
    *left = x - *x1;
    *right = *x2 - (x + w - 1);
}

void pb_lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 const uint16_t *buf) {
    if (!buf || w == 0 || h == 0) return;
    if (x >= PB_PANEL_W || y >= PB_PANEL_H) return;
    if (x + w > PB_PANEL_W) w = PB_PANEL_W - x;
    if (y + h > PB_PANEL_H) h = PB_PANEL_H - y;

    uint32_t x1, x2, left, right;
    align_window(x, w, &x1, &x2, &left, &right);
    const uint32_t pw = x2 - x1 + 1;

    position_cursor(x1, x2, y);

    /* Not row by row but chunk by chunk: contiguous buffer, single DMA (see
     * the note above). */
    uint32_t rows_per_chunk = PB_STACK_PIXELS / pw;
    if (rows_per_chunk == 0) rows_per_chunk = 1;

    for (uint32_t row0 = 0; row0 < h; row0 += rows_per_chunk) {
        uint32_t n = h - row0;
        if (n > rows_per_chunk) n = rows_per_chunk;

        uint16_t *dst = s_stack;
        for (uint32_t r = 0; r < n; r++) {
            const uint16_t *src = buf + (size_t)(row0 + r) * w;
            for (uint32_t i = 0; i < w; i++) {
                dst[left + i] = (uint16_t)((src[i] >> 8) | (src[i] << 8));
            }
            /* Alignment padding: the edge pixel is duplicated. For aligned
             * callers (LVGL included) this never runs. */
            if (left) dst[0] = dst[1];
            if (right) dst[pw - 1] = dst[pw - 2];
            if (x1 == 0 && y + row0 + r < PB_PANEL_H) {
                s_strip[y + row0 + r][0] = dst[0];
                s_strip[y + row0 + r][1] = dst[1];
            }
            dst += pw;
        }
        send_stack(n * pw);
    }

    pb_lcd_stream_end();
    mark_cursor(x1, x2, y + h);
}

void pb_lcd_blit_strided(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         const uint16_t *buf, int32_t col_step, int32_t row_step) {
    if (!buf || w == 0 || h == 0) return;
    if (x >= PB_PANEL_W || y >= PB_PANEL_H) return;
    /* No clipping here: because the steps can be negative, a clipped
     * rectangle would also have to shift its source origin, and doing that
     * without the caller knowing invites silent bugs. Out-of-bounds requests
     * are rejected. */
    if (x + w > PB_PANEL_W || y + h > PB_PANEL_H) return;

    uint32_t x1, x2, left, right;
    align_window(x, w, &x1, &x2, &left, &right);
    const uint32_t pw = x2 - x1 + 1;

    position_cursor(x1, x2, y);

    uint32_t rows_per_chunk = PB_STACK_PIXELS / pw;
    if (rows_per_chunk == 0) rows_per_chunk = 1;

    for (uint32_t row0 = 0; row0 < h; row0 += rows_per_chunk) {
        uint32_t n = h - row0;
        if (n > rows_per_chunk) n = rows_per_chunk;

        uint16_t *dst = s_stack;
        for (uint32_t r = 0; r < n; r++) {
            const uint16_t *src = buf + (int32_t)(row0 + r) * row_step;
            for (uint32_t i = 0; i < w; i++) {
                uint16_t px = src[(int32_t)i * col_step];
                dst[left + i] = (uint16_t)((px >> 8) | (px << 8));
            }
            if (left) dst[0] = dst[1];
            if (right) dst[pw - 1] = dst[pw - 2];
            if (x1 == 0 && y + row0 + r < PB_PANEL_H) {
                s_strip[y + row0 + r][0] = dst[0];
                s_strip[y + row0 + r][1] = dst[1];
            }
            dst += pw;
        }
        send_stack(n * pw);
    }

    pb_lcd_stream_end();
    mark_cursor(x1, x2, y + h);
}

void pb_lcd_fill(uint16_t color) {
    /* A single pass: full-width column window, RAMWR, the whole screen.
     * The old version issued 640 separate window+RAMWR pairs and every row
     * landed on the same TOP row — the headline symptom of "the screen does
     * not clear". */
    caset_inner(0, PB_PANEL_W - 1);
    stream_begin_inner(0x2C);
    pb_lcd_stream_color(color, (uint32_t)PB_PANEL_W * PB_PANEL_H);
    pb_lcd_stream_end();

    const uint16_t be = (uint16_t)((color >> 8) | (color << 8));
    for (uint32_t r = 0; r < PB_PANEL_H; r++) { s_strip[r][0] = be; s_strip[r][1] = be; }
    s_strip_valid = true;
    mark_cursor(0, PB_PANEL_W - 1, PB_PANEL_H);
}

void pb_lcd_stream_flat(uint16_t color, uint32_t pixel) {
    pb_lcd_stream_begin(0x2c);
    pb_lcd_stream_color(color, pixel);
    pb_lcd_stream_end();
    pb_lcd_cursor_invalidate();
}
