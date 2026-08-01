/**
 * lv_port.h — LVGL'i PokeBird donanımına bağlayan katman
 *
 * Ekran (QSPI panel) ve giriş (kapasitif dokunmatik) sürücülerini LVGL'e
 * tanıtır. Arayüz YATAY (640x172), panel DİKEY (172x640); aradaki 90°
 * çevrim ek tampon olmadan flush sırasında yapılıyor — ayrıntı lv_port.c.
 */
#ifndef POKEBIRD_LV_PORT_H
#define POKEBIRD_LV_PORT_H

#include <stdbool.h>

/**
 * LVGL'i, ekranı ve dokunmatiği başlat.
 *
 * Ekran donanımı (QSPI + panel) BU ÇAĞRIDAN ÖNCE başlatılmış olmalı.
 * @return dokunmatik yanıt veriyorsa true; false dönse de ekran çalışır.
 */
bool pb_lv_init(void);

/** LVGL'in zamanlayıcısını çevir. Ana döngüden düzenli çağrılmalı. */
void pb_lv_tick(void);

#endif /* POKEBIRD_LV_PORT_H */
