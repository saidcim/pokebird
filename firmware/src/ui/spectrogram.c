#include "ui/spectrogram.h"

#include <string.h>

#include "hal/display/lcd_blit.h"

/* ── Orientation mapping ──────────────────────────────────────────────────
 * The panel is natively 172 wide x 640 tall (portrait). The UI holds the
 * device in landscape: 640 wide x 172 tall.
 *
 * MEASURED ON THE BOARD (the `o` command — four colours in four corners, with
 * the device held so the USB socket points DOWN):
 *
 *     panel X 0->171  =  physical LEFT -> RIGHT
 *     panel Y 0->639  =  physical TOP  -> BOTTOM
 *
 * For landscape use the device is turned 90 degrees to the left; in that
 * position the **USB socket ends up on the RIGHT** and the axes fall out as:
 *
 *     panel Y+  =  physical LEFT -> RIGHT   -> the UI's x (time)
 *     panel X+  =  physical BOTTOM -> TOP   -> the UI's y, INVERTED
 *
 *   ui (ux, uy)  ->  panel (nx, ny)
 *   ux 0..639 (left->right, time)      ->  ny = ux
 *   uy 0..171 (top->bottom, frequency) ->  nx = 171 - uy
 *
 * So a VERTICAL COLUMN in the UI is a HORIZONTAL ROW on the panel — one blit
 * call, 172 pixels. The first attempt had this backwards: the driver was
 * handed X values up to 639 when the panel's X axis only goes 0..171. The
 * window address was invalid, so the screen stayed completely black.
 *
 * To hold the device the other way round (USB on the left), two things must
 * flip together: `ny` in `write_ui_column`, and the bin mapping below. */

static uint16_t s_column[PB_SPEC_HEIGHT];
static uint32_t s_write_x = 0;

/**
 * Turn a magnitude into a colour — a WARM ramp: screen background -> ember ->
 * amber -> white.
 *
 * A linear greyscale is poor for birdsong: the detail we care about clusters
 * in the upper magnitudes and greyscale cannot separate it. A ramp whose
 * brightness rises all the way through keeps both the faint harmonics and the
 * strong fundamental readable at once.
 *
 * The COLOURS changed at one point (they used to be black -> dark blue ->
 * cyan -> yellow -> white). The reason showed up in the preview: the rest of
 * the UI is warm and dark (amber/green on a #0B0908 background) while the
 * spectrogram was electric blue. Side by side they looked like two different
 * products, with a hard vertical seam at x=384.
 *
 * The current ramp fixes two things at once:
 *   · it resolves onto the design's amber (#FFB020)
 *   · its BASE IS the screen background itself (#0B0908), so silence is the
 *     same colour as the UI's background and the seam disappears
 */
typedef struct { uint8_t v, r, g, b; } stop_t;

/* Brightness rises all the way through; the intermediate colours pass through
 * the design's amber. */
static const stop_t RAMP[] = {
    {   0, 0x0B, 0x09, 0x08 },   /* screen background — silence            */
    {  56, 0x2E, 0x18, 0x0A },   /* ember                                  */
    { 128, 0x8A, 0x3F, 0x0C },   /* red amber                              */
    { 190, 0xFF, 0xB0, 0x20 },   /* PB_COLOR_ACCENT — the design's accent  */
    { 255, 0xFF, 0xF2, 0xCC },   /* warm white — the peak                  */
};

static uint16_t amplitude_to_rgb565(uint8_t v) {
    const uint32_t n = sizeof(RAMP) / sizeof(RAMP[0]);

    uint32_t i = 0;
    while (i + 2 < n && v > RAMP[i + 1].v) i++;

    const stop_t *a = &RAMP[i], *b = &RAMP[i + 1];
    const int span = (int)b->v - (int)a->v;
    const int t = span > 0 ? ((int)v - (int)a->v) * 255 / span : 0;

    const uint8_t r = (uint8_t)(a->r + ((int)b->r - (int)a->r) * t / 255);
    const uint8_t g = (uint8_t)(a->g + ((int)b->g - (int)a->g) * t / 255);
    const uint8_t bl = (uint8_t)(a->b + ((int)b->b - (int)a->b) * t / 255);

    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3));
}

/** Write a UI column (ux) to the panel: panel row ny=ux, nx=0..171.
 *  The buffer index is the panel's X directly; because the UI's y is
 *  inverted, the caller that fills the buffer (pb_spec_push_column) does the
 *  conversion. */
static void write_ui_column(uint32_t ux, const uint16_t *col) {
    pb_lcd_blit(0, ux, PB_SPEC_HEIGHT, 1, col);
}

void pb_spec_init(void) {
    memset(s_column, 0, sizeof(s_column));
    for (uint32_t ux = PB_SPEC_X0; ux <= PB_SPEC_X1; ux++) {
        write_ui_column(ux, s_column);
    }
    s_write_x = 0;
}

void pb_spec_push_column(const uint8_t *bins, uint32_t n_bins) {
    if (!bins || n_bins == 0) return;

    /* Low frequencies go at the BOTTOM of the screen — that is the
     * spectrogram convention. The buffer index is the panel's X, and in
     * landscape the panel's X+ runs physically BOTTOM TO TOP (see the
     * measurement above). So the low frequencies belong at nx=0: the bin
     * index rises directly with nx, with NO inversion. */
    for (uint32_t nx = 0; nx < PB_SPEC_HEIGHT; nx++) {
        uint32_t bin = (nx * n_bins) / PB_SPEC_HEIGHT;
        s_column[nx] = amplitude_to_rgb565(bins[bin]);
    }
    write_ui_column(PB_SPEC_X0 + s_write_x, s_column);

    /* Mark the next column with a cursor: without a visible "now", the
     * scrolling strip is unreadable. The colour is PB_COLOR_BORDER
     * (0x3A332A), the same as the UI's separator; the old dark grey looked
     * foreign inside the warm palette. */
    static uint16_t cursor[PB_SPEC_HEIGHT];
    for (uint32_t nx = 0; nx < PB_SPEC_HEIGHT; nx++) cursor[nx] = 0x3985;
    write_ui_column(PB_SPEC_X0 + ((s_write_x + 1) % PB_SPEC_WIDTH), cursor);

    s_write_x = (s_write_x + 1) % PB_SPEC_WIDTH;
}
