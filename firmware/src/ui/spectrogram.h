/**
 * spectrogram.h — the live scrolling spectrogram
 *
 * We do not use an LVGL canvas. A full 640x172 framebuffer in RGB565 would be
 * 220 KB, 42% of the 520 KB of SRAM. Instead each frame produces exactly ONE
 * pixel column and sends it over QSPI — the memory cost is one column
 * (172 pixels = 344 bytes) and the CPU cost is close to zero.
 *
 * The display has no hardware vertical scrolling, so the columns are written
 * circularly: a new column overwrites the oldest one and the write position
 * advances to the right. That gives the classic scrolling-strip look with
 * none of the cost of actually scrolling.
 */
#ifndef POKEBIRD_SPECTROGRAM_H
#define POKEBIRD_SPECTROGRAM_H

#include <stdint.h>

/** The region of the screen given to the spectrogram (the right-hand side;
 *  LVGL draws the left).
 *
 * WARNING: THE BOUNDARY MUST LAND ON A SLICE. LVGL now runs at full width and
 * pushes the panel as vertical SLICES 128 pixels wide (lv_port.c). This strip
 * is the TWO right-hand slices: 384..639. If LVGL and the spectrogram shared
 * a slice they would erase each other, because each writes the whole slice.
 *
 * The design asked for 236 px; 256 is the nearest value that lands on a slice
 * boundary, and the 20-pixel difference is invisible in the layout. */
#define PB_SPEC_X0      384
#define PB_SPEC_X1      639
#define PB_SPEC_WIDTH   (PB_SPEC_X1 - PB_SPEC_X0 + 1)
#define PB_SPEC_HEIGHT  172

/** Clear the region and rewind the write position. */
void pb_spec_init(void);

/**
 * Draw one time slice as a single column.
 * @param bins    magnitudes 0..255 each, from low frequency to high
 * @param n_bins  number of bins; scaled to the screen height
 */
void pb_spec_push_column(const uint8_t *bins, uint32_t n_bins);

#endif /* POKEBIRD_SPECTROGRAM_H */
