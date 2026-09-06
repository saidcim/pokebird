/**
 * screen_log.h — SCREEN 1 · LOG
 *
 * Full width (640, all five slices belong to LVGL): the species identified so
 * far, newest at the top, with a counter row at the bottom that can be read
 * without any other instrumentation.
 *
 * WHERE THE LOG LIVES: in RAM, in an 8-entry ring. An SD-card log was a later
 * planned step and does not exist yet, and the RTC was never brought up
 * either, so the rows show **time since boot** rather than a clock time
 * ("12 min ago"). When a real clock arrives, `write_age` is the only place
 * that needs to change.
 */
#ifndef POKEBIRD_SCREEN_LOG_H
#define POKEBIRD_SCREEN_LOG_H

#include "lvgl.h"

/** Build the screen and return it. */
lv_obj_t *pb_screen_log_create(void);

/** Add a detection to the log (at the top). */
void pb_screen_log_add(const char *name, const char *latin, float confidence);

/** Refresh the "x min ago" texts and the counter row. */
void pb_screen_log_refresh(uint32_t frame_rate, uint32_t inference,
                           uint32_t overrun);

#endif /* POKEBIRD_SCREEN_LOG_H */
