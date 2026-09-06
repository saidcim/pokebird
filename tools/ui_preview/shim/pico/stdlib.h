/**
 * pico/stdlib.h — a FAKE Pico SDK header for the host preview
 *
 * The UI files (screen_log.c) reach into the Pico SDK only for the time. On
 * the host there is no real SDK, so those two calls are provided here and the
 * clock is driven BY HAND — which lets the "just now" / "3 min ago" rows all
 * be shown in a single render.
 *
 * This file is visible ONLY in the tools/ui_preview build (its include path
 * is specific to that target); the device build keeps using the real SDK.
 */
#ifndef POKEBIRD_PREVIEW_PICO_STDLIB_H
#define POKEBIRD_PREVIEW_PICO_STDLIB_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t absolute_time_t;

/** The preview's fake clock (ms). The test code writes it directly. */
extern uint32_t pb_preview_ms;

static inline absolute_time_t get_absolute_time(void) {
    return (absolute_time_t)pb_preview_ms * 1000u;
}

static inline uint32_t to_ms_since_boot(absolute_time_t t) {
    return (uint32_t)(t / 1000u);
}

#endif /* POKEBIRD_ONIZLE_PICO_STDLIB_H */
