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
#include <stdint.h>

/**
 * LVGL'i, ekranı ve dokunmatiği başlat.
 *
 * Ekran donanımı (QSPI + panel) BU ÇAĞRIDAN ÖNCE başlatılmış olmalı.
 * @return dokunmatik yanıt veriyorsa true; false dönse de ekran çalışır.
 */
bool pb_lv_init(void);

/** LVGL'in zamanlayıcısını çevir. Ana döngüden düzenli çağrılmalı. */
void pb_lv_tick(void);

/* Flush sayaçları — panel sütun hizalaması gerçekten tutuyor mu (§9n).
 * Panel sütun aralığını 2 piksele yuvarlıyor; sütun sayısı tek olursa veri
 * her satırda bir piksel kayar ve yazı yatay sürüklenmiş görünür.
 * Göz gerektirmeyen ölçüm: `a` ve `u` çıkışında basılıyor. */
extern uint32_t pb_lv_flush_say;
extern uint32_t pb_lv_flush_hizasiz;
extern uint32_t pb_lv_flush_stride_farkli;
extern uint32_t pb_lv_flush_w_min, pb_lv_flush_w_max;
extern int32_t  pb_lv_son_x1, pb_lv_son_x2, pb_lv_son_y1, pb_lv_son_y2;
extern int32_t  pb_lv_son_stride_px, pb_lv_son_alan_w;
void pb_lv_flush_sayaclari_sifirla(void);

/**
 * Sonraki `adet` flush alanını seri porta ASCII olarak dök — GÖZ GEREKMEZ.
 *
 * Dökümü sürücünün OKUDUĞU indislemeyle üretiyor, yani hem LVGL'in çizimini
 * hem 90° devrik okumayı aynı anda sınıyor. Terminalde yazı düzgün
 * okunuyorsa bozulma daha aşağıda (panel/hat); okunmuyorsa LVGL tarafında.
 */
void pb_lv_dokum_iste(int adet);

#endif /* POKEBIRD_LV_PORT_H */
