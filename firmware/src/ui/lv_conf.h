/**
 * lv_conf.h — LVGL v9.3 configuration (PokeBird)
 *
 * Rather than copying all of `third_party/lvgl/lv_conf_template.h`, only the
 * settings we DEVIATE from the default on are here; the rest comes from
 * LVGL's own defaults (`lv_conf_internal.h` fills in every undefined macro).
 * That keeps this file small across LVGL upgrades and makes it obvious which
 * decisions were deliberate.
 *
 * THE HARDEST CONSTRAINT: 520 KB of SRAM, NO PSRAM. The budget gives LVGL
 * about 26 KB of draw buffer; the object pool comes out of the general heap.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* ── Colour and memory ─────────────────────────────────────────────────── */

/* The panel is RGB565. The byte-order swap happens in our own blit (see
 * hal/display/lcd_blit.c); we do not ask LVGL to swap. */
#define LV_COLOR_DEPTH 16

/* LVGL's own heap. Objects, styles and animations come from here.
 *
 * 24 KB WAS NOT ENOUGH — MEASURED, not guessed. An earlier comment claimed it
 * was "plenty for a simple single-screen UI", which became wrong the moment
 * the UI grew to TWO screens. `tools/ui_preview` (host, same lv_conf as the
 * device) measured it with `lv_mem_monitor`:
 *
 *     after screen 0 (listen) was built:  16,384 / 20,624 bytes = 80% full
 *     while building screen 1 (log):      the pool ran out
 *
 * (About 20.6 KB of the 24 KB is usable; the rest is LVGL's own bookkeeping.)
 * When the pool runs out `lv_obj_create` returns NULL and the caller does not
 * check it — a segfault on the host, and on the board a screen that is
 * silently incomplete or corrupt.
 *
 * 64 KB WAS THEN CHOSEN, but when M7 (adding the stage-1 binary net) needed
 * SRAM, the pool's real usage was MEASURED AGAIN with `tools/ui_preview`:
 * with both screens built it is **22,112 / 60,512 bytes (37%)**. The old
 * "two screens, about 33 KB" was an estimate and the measurement came in
 * lower — most of the 64 KB was never touched.
 *
 * Reduced to 32 KB: about 10.6 KB (48%) of headroom above the measurement,
 * enough for fragmentation and small growth without being wastefully large.
 * The 32 KB that freed up went spare (an attempt to grow the audio ring hit a
 * hardware limit and was reverted, see audio_i2s.h) and is headroom for
 * future stages. */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN
#define LV_MEM_SIZE             (32 * 1024)

/* Time base: v9 has no macro for it, it is supplied at run time with
 * `lv_tick_set_cb()` (see ui/lv_port.c). v8's LV_TICK_CUSTOM does nothing
 * here. */

/* ── Drawing ───────────────────────────────────────────────────────────── */
/* Single core, no auxiliary draw unit. */
#define LV_USE_DRAW_SW          1
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_DRAW_THREAD_STACK_SIZE (2 * 1024)

/* THIS MUST BE 1 — it used to be 0, with an INCOMPLETE justification.
 *
 * The old comment said "complex effects (shadows, gradient masks) are not
 * needed". True but insufficient: this flag ALSO disables ROUNDED CORNERS.
 * Every rectangle with a radius above zero is silently NOT DRAWN AT ALL — no
 * error, it simply does not appear.
 *
 * It went unnoticed because the old text-heavy card had no rounded objects.
 * The current UI is built on confidence bars (radius 3), rank badges
 * (circles) and page dots; all of them had vanished.
 *
 * CAUGHT with `tools/ui_preview`: separator lines (radius 0) were drawn while
 * bars and badges were not. The board behaved identically. */
#define LV_DRAW_SW_COMPLEX      1

/* ── Disabled features — saving flash and RAM ──────────────────────────── */
#define LV_USE_LOG              0
#define LV_USE_ASSERT_NULL      1
#define LV_USE_ASSERT_MALLOC    1
#define LV_USE_ASSERT_STYLE     0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ       0

#define LV_USE_SYSMON           0

/* Filesystem and image decoders can be switched on if the SD card ever needs
 * them. (The v9 names are LODEPNG / TJPGD, not v8's LV_USE_PNG /
 * LV_USE_JPEGDEC.) */
#define LV_USE_FS_STDIO         0
#define LV_USE_LODEPNG          0
#define LV_USE_BMP              0
#define LV_USE_TJPGD            0
#define LV_USE_GIF              0
#define LV_USE_QRCODE           0

/* ── Fonts ─────────────────────────────────────────────────────────────── */
/* The screen is a strip 172 px tall: one small and one medium face is enough.
 * Every font costs flash, so the unneeded ones are off. */
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_DEFAULT         &lv_font_montserrat_14

/* ── Widgets ───────────────────────────────────────────────────────────── */
/* What the UI needs: labels, buttons, bars and list-like layouts. The rest
 * are off and can each be switched back on with one line. */
#define LV_USE_LABEL            1
#define LV_USE_BUTTON           1
#define LV_USE_BAR              1
#define LV_USE_IMAGE            1
#define LV_USE_LINE             1
#define LV_USE_CANVAS           0   /* the spectrogram writes straight to QSPI */
#define LV_USE_CHART            0
#define LV_USE_KEYBOARD         0
#define LV_USE_TEXTAREA         0
#define LV_USE_CALENDAR         0
#define LV_USE_ANIMIMG          0

/* These DEPEND on things we disabled but default to on. Left enabled, LVGL
 * raises an #error at compile time (spinbox -> textarea, lottie -> canvas).
 * Disabling a widget means disabling its dependants too. */
#define LV_USE_SPINBOX          0   /* requires textarea */
#define LV_USE_LOTTIE           0   /* requires canvas + ThorVG */

#define LV_USE_THEME_DEFAULT    1
#define LV_USE_THEME_SIMPLE     0
#define LV_USE_FLEX             1
#define LV_USE_GRID             0

/* Do not build the examples and demos. */
#define LV_BUILD_EXAMPLES       0

#endif /* LV_CONF_H */
