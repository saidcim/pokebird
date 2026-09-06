/**
 * pico/stdlib.h — host önizlemesi için SAHTE Pico SDK başlığı
 *
 * Arayüz dosyaları (ekran_gunluk.c) yalnızca zaman için Pico SDK'ya bakıyor.
 * Host'ta gerçek SDK yok; burada o iki çağrının karşılığı veriliyor ve saat
 * ELLE sürülüyor — böylece "az once" / "3 dk once" satırlarının hepsi tek
 * render'da gösterilebiliyor.
 *
 * Bu dosya YALNIZCA tools/ui_preview derlemesinde görünür (include yolu
 * oraya özel); cihaz derlemesi gerçek SDK'yı kullanmaya devam ediyor.
 */
#ifndef POKEBIRD_ONIZLE_PICO_STDLIB_H
#define POKEBIRD_ONIZLE_PICO_STDLIB_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t absolute_time_t;

/** Önizlemenin sahte saati (ms). Test kodu doğrudan yazıyor. */
extern uint32_t pb_preview_ms;

static inline absolute_time_t get_absolute_time(void) {
    return (absolute_time_t)pb_preview_ms * 1000u;
}

static inline uint32_t to_ms_since_boot(absolute_time_t t) {
    return (uint32_t)(t / 1000u);
}

#endif /* POKEBIRD_ONIZLE_PICO_STDLIB_H */
