/**
 * screen_listen.h — SCREEN 0 · LISTEN
 *
 * The left 384 pixels (LVGL slices 0..2): a status heading plus the three
 * species closest to the sound, each with its name, scientific name and
 * confidence bar. The right 256 pixels (slices 3..4) belong to the
 * spectrogram and are written to the panel DIRECTLY — this file never touches
 * them (see lv_port.c for slice ownership).
 */
#ifndef POKEBIRD_SCREEN_LISTEN_H
#define POKEBIRD_SCREEN_LISTEN_H

#include "lvgl.h"
#include "ui/interface.h"

/** Build the screen and return it (loading it into LVGL is the caller's job). */
lv_obj_t *pb_screen_listen_create(void);

/** Refresh the contents — only labels that actually changed are rewritten. */
void pb_screen_listen_update(const pb_result_view_t *view);

/** Set the record button's appearance (filled square = listening, circle =
 *  idle). */
void pb_screen_listen_set_recording(bool recording);

#endif /* POKEBIRD_SCREEN_LISTEN_H */
