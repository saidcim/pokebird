/**
 * ekran_gunluk.h — EKRAN 1 · GÜNLÜK
 *
 * Tam genişlik (640, beş dilim de LVGL'in): o ana kadar tanınan türler, en
 * yenisi üstte. Altta göz gerektirmeyen sayaç satırı.
 *
 * ⚠ KAYIT NEREDE DURUYOR: RAM'de, 8 elemanlı bir halkada. SD kart günlüğü
 * M7'nin 5. adımı (§9o) ve henüz yok; RTC de kurulmadı (4. adım), bu yüzden
 * satırlar saat değil **açılıştan bu yana geçen süre** gösteriyor ("12 dk
 * önce"). Gerçek saat geldiğinde değiştirilecek tek yer `sure_yaz`.
 */
#ifndef POKEBIRD_SCREEN_LOG_H
#define POKEBIRD_SCREEN_LOG_H

#include "lvgl.h"

/** Ekranı kur ve döndür. */
lv_obj_t *pb_screen_log_create(void);

/** Günlüğe bir tespit ekle (en üste). */
void pb_screen_log_add(const char *name, const char *latin, float confidence);

/** "x dk önce" metinlerini ve sayaç satırını tazele. */
void pb_screen_log_refresh(uint32_t frame_rate, uint32_t inference,
                            uint32_t overrun);

#endif /* POKEBIRD_SCREEN_LOG_H */
