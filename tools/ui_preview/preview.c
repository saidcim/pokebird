/**
 * preview.c — renders the UI on the HOST with real LVGL and writes PNGs
 *
 * WHY THIS EXISTS: the only way to see what the UI looked like was to flash
 * the board and ask someone to look at it. Every round of that is slow and
 * expensive, and design iteration is impossible that way.
 *
 * What is compiled here is THE DEVICE'S screen code ITSELF (screen_listen.c,
 * screen_log.c, theme.c, text.c and the generated fonts) — nothing is
 * reimplemented. The only difference is underneath: an in-memory framebuffer
 * instead of the panel, and a small fake clock (shim/pico/stdlib.h) instead
 * of the Pico SDK.
 *
 * So this preview shows the LAYOUT, the FONTS, the COLOURS and the TEXT
 * WRAPPING exactly. What it does NOT show is the path to the panel: slice
 * boundaries, slip, QSPI timing. Those still have to be verified on the
 * board.
 *
 *   cmake -S tools/ui_preview -B tools/ui_preview/build -G Ninja
 *   cmake --build tools/ui_preview/build
 *   ./tools/ui_preview/build/ui_preview
 *   python tools/ui_preview/ppm_png.py
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "hal/display/lcd_blit.h"
#include "ui/screen_listen.h"
#include "ui/screen_log.h"
#include "ui/spectrogram.h"
#include "ui/theme.h"

uint32_t pb_preview_ms = 0;

#define W PB_SCREEN_W
#define H PB_SCREEN_H

static uint16_t s_fb[W * H];
static lv_display_t *s_disp;

/* PARTIAL mode, as on the device: LVGL draws in strips and flush_cb commits
 * them to the framebuffer. This was chosen over DIRECT mode because it is the
 * device's drawing path, and going through the same code path is what makes
 * the preview worth having. */
#define STRIP_H 43
static uint16_t s_draw[W * STRIP_H];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
    const uint16_t *src = (const uint16_t *)(void *)px;

    int32_t step = a->x2 - a->x1 + 1;
    lv_draw_buf_t *db = lv_display_get_buf_active(d);
    if (db && db->header.stride) step = (int32_t)(db->header.stride / 2);

    for (int32_t y = a->y1; y <= a->y2; y++) {
        for (int32_t x = a->x1; x <= a->x2; x++) {
            if (x < 0 || x >= W || y < 0 || y >= H) continue;
            s_fb[y * W + x] = src[(size_t)(y - a->y1) * step + (x - a->x1)];
        }
    }
    lv_display_flush_ready(d);
}

static uint32_t tick_cb(void) { return pb_preview_ms; }

/* ── The fake panel — the spectrogram is drawn by its REAL code ───────────
 *
 * The spectrogram does not go through LVGL; it writes to the panel DIRECTLY
 * (its own fast column path). That is why the right-hand side of the preview
 * used to come out empty. With an equivalent of `pb_lcd_blit` here, the
 * device's `spectrogram.c` compiles unchanged and draws into the same
 * framebuffer — colour mapping and cursor column included.
 *
 * The SAME orientation mapping as the device (see the header of
 * ui/spectrogram.c):
 *     panel row r     ->  ui_x = r
 *     panel column c  ->  ui_y = 171 - c
 */
void pb_lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 const uint16_t *buf) {
    for (uint32_t r = 0; r < h; r++) {
        const uint32_t ux = y + r;
        if (ux >= (uint32_t)W) continue;
        for (uint32_t c = 0; c < w; c++) {
            const uint32_t nc = x + c;
            if (nc >= (uint32_t)H) continue;
            s_fb[(H - 1 - nc) * W + ux] = buf[r * w + c];
        }
    }
}

/** A realistic song pattern — syllables, a sweeping fundamental, two
 *  harmonics and a noise floor. The same pattern as the `C` demo: it MUST NOT
 *  be a flat colour, or it would both hide slip and say nothing about what
 *  the real output looks like. */
static void fill_spectrogram(void) {
    pb_spec_init();
    for (uint32_t column = 0; column < PB_SPEC_WIDTH; column++) {
        uint8_t bins[64];
        const uint32_t phase = column % 48;
        const bool silent = (phase >= 34);
        const int base = 13 + (int)(phase < 17 ? phase : 34 - phase);
        for (int b = 0; b < 64; b++) {
            int v = 10 + (int)((column * 7u + (uint32_t)b * 13u) % 9u);
            if (!silent) {
                for (int hrm = 1; hrm <= 3; hrm++) {
                    const int center = base * hrm;
                    const int d = b - center;
                    if (center < 64 && d > -4 && d < 4) {
                        const int g = (d < 0 ? -d : d);
                        v += (240 / hrm) - g * 40;
                    }
                }
            }
            bins[b] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        pb_spec_push_column(bins, 64);
    }
}

/** Write the framebuffer as PPM (P6, 8-bit RGB); ppm_png.py converts it to
 *  PNG. */
static void dump(const char *name) {
    FILE *f = fopen(name, "wb");
    if (!f) { printf("could not open: %s\n", name); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        const uint16_t p = s_fb[i];
        /* RGB565 -> RGB888, replicating the high bits into the low ones so
         * that pure white stays white rather than becoming an approximation
         * like 0xF8. */
        const uint8_t r = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
        const uint8_t g = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
        const uint8_t b = (uint8_t)((p & 0x1F) * 255 / 31);
        fputc(r, f); fputc(g, f); fputc(b, f);
    }
    fclose(f);
    printf("  wrote: %s\n", name);
}

/** LVGL heap usage — the SAME lv_conf (LV_MEM_SIZE) as the device applies,
 *  so a pool that fills up here fills up on the board too. */
static void memory(const char *where) {
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    printf("  [LVGL heap] %-22s used %6u / %6u bytes  (%%%u full, "
           "largest free block %u)\n",
           where, (unsigned)(m.total_size - m.free_size), (unsigned)m.total_size,
           (unsigned)m.used_pct, (unsigned)m.free_biggest_size);
}

/* LVGL FIRST, spectrogram SECOND: slice ownership is not simulated in the
 * preview, so LVGL paints the whole 640 and would erase the strip if the
 * order were reversed. The device does not have this problem (lv_port.c's
 * slice mask). */
static void draw(lv_obj_t *scr, const char *name, bool spectro) {
    lv_screen_load(scr);
    lv_obj_invalidate(scr);
    lv_refr_now(s_disp);
    if (spectro) fill_spectrogram();
    dump(name);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* do not lose output on a crash */
    printf("UI preview (%dx%d)\n", W, H);

    lv_init();
    lv_tick_set_cb(tick_cb);

    s_disp = lv_display_create(W, H);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, s_draw, NULL, sizeof(s_draw),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    printf("  display created\n");

    /* ── SCREEN 0 · LISTEN ────────────────────────────────────────────────
     * A realistic and DIFFICULT example: a long species name, three
     * candidates, and an IDENTIFIED decision. */
    lv_obj_t *listen = pb_screen_listen_create();
    memory("screen 0 built");

    pb_result_view_t g;
    memset(&g, 0, sizeof(g));
    g.mode = PB_DECISION_SPECIES;
    g.species_name = "Common Nightingale";
    g.confidence = 0.94f;
    g.top3_name[0] = "Common Nightingale";
    g.top3_name[1] = "Eurasian Blackbird";
    g.top3_name[2] = "European Robin";
    g.top3_latin[0] = "Luscinia megarhynchos";
    g.top3_latin[1] = "Turdus merula";
    g.top3_latin[2] = "Erithacus rubecula";
    g.top3_probability[0] = 0.94f;
    g.top3_probability[1] = 0.61f;
    g.top3_probability[2] = 0.38f;
    g.frame_rate = 63;
    g.inference = 12;
    g.overrun = 0;
    pb_screen_listen_set_recording(true);
    pb_screen_listen_update(&g);
    draw(listen, "ui-identified.ppm", true);

    /* Listening mode — what the screen shows with no species at all. */
    pb_result_view_t b;
    memset(&b, 0, sizeof(b));
    b.mode = PB_DECISION_LISTENING;
    b.frame_rate = 63;
    pb_screen_listen_set_recording(true);
    pb_screen_listen_update(&b);
    draw(listen, "ui-listening.ppm", true);

    /* BOSTA — cihaz acilista dinlemiyor, kullanici butona basacak. */
    pb_screen_listen_set_recording(false);
    pb_screen_listen_update(&b);
    draw(listen, "ui-idle.ppm", true);

    /* Longest species name — the worst case for wrapping and truncation. */
    pb_result_view_t u;
    memset(&u, 0, sizeof(u));
    u.mode = PB_DECISION_UNSURE;
    u.species_name = "Greater White-fronted Goose";
    u.confidence = 0.42f;
    u.top3_name[0] = "Greater White-fronted Goose";
    u.top3_name[1] = "Eastern Olivaceous Warbler";
    u.top3_name[2] = "Lesser Spotted Woodpecker";
    u.top3_latin[0] = "Anser albifrons";
    u.top3_latin[1] = "Iduna pallida";
    u.top3_latin[2] = "Dryobates minor";
    u.top3_probability[0] = 0.42f;
    u.top3_probability[1] = 0.29f;
    u.top3_probability[2] = 0.11f;
    pb_screen_listen_set_recording(true);
    pb_screen_listen_update(&u);
    draw(listen, "ui-longest-name.ppm", true);

    /* ── SCREEN 1 · LOG ──────────────────────────────────────────────── */
    lv_obj_t *log = pb_screen_log_create();
    memory("screen 1 built");

    pb_preview_ms = 0;
    draw(log, "ui-log-empty.ppm", false);

    /* Advance the clock by hand to produce three different ages. */
    pb_preview_ms = 10u * 1000u;
    pb_screen_log_add("Alexandrine Parakeet", "Psittacula eupatria", 0.78f);
    pb_preview_ms = 40u * 60u * 1000u;
    pb_screen_log_add("Yellow-legged Gull", "Larus michahellis", 0.87f);
    pb_preview_ms = 95u * 60u * 1000u;
    pb_screen_log_add("Common Nightingale", "Luscinia megarhynchos", 0.94f);

    pb_preview_ms = 96u * 60u * 1000u;
    pb_screen_log_refresh(63, 12, 0);
    draw(log, "ui-log.ppm", false);

    printf("done\n");
    return 0;
}
