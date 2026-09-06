/**
 * lv_port.h — LVGL'i PokeBird donanımına bağlayan katman
 *
 * Ekran (QSPI panel) ve giriş (kapasitif dokunmatik) sürücülerini LVGL'e
 * tanıtır. Arayüz YATAY (640x172), panel DİKEY (172x640); aradaki 90°
 * çevrim flush sırasında yapılıyor — ayrıntı lv_port.c.
 *
 * Arayüz TAM GENİŞLİK (640) ve ekran beş DİKEY DİLİM hâlinde basılıyor;
 * gerekçesi ve ölçümleri lv_port.c'nin başında. Tam ekran framebuffer'ı
 * (220 KB) hâlâ SIĞMIYOR — dilim yolu onun yerine geçiyor ve daha ucuz.
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
extern uint32_t pb_lv_dilim_basim;      /* panele basılan dilim sayısı */
void pb_lv_flush_sayaclari_sifirla(void);

/**
 * LVGL'in hangi dikey dilimleri çizdiğini bildir (bit d = dilim d, 128 px).
 *
 * Spektrogram panele DOĞRUDAN yazıyor (62 Hz, kendi hızlı sütun yolu). Onun
 * bölgesini LVGL de basarsa ikisi birbirini siler. Ekranlar bu yüzden kendi
 * yerleşimlerine göre sahipliği bildiriyor: dinleme ekranı sağdaki iki dilimi
 * spektrograma bırakıyor, günlük ekranı beşini de kendi alıyor.
 */
void pb_lv_dilim_sahibi_ayarla(uint32_t maske);

/** Bütün dilimleri kirlet — ekran değişiminde tam yeniden çizim için. */
void pb_lv_tumunu_kirlet(void);

/**
 * Ham dokunma noktasını ARAYÜZ koordinatlarında oku (0..639, 0..171).
 *
 * Eşleme çalışan sürücüden alındı (rsvpnano axs15231b_touch.cpp); ayrıntı ve
 * eski koddaki kırpma hatası lv_port.c'de yazılı. Kaydırma algılayıcısı
 * (`kaydirma.c`) LVGL'in giriş katmanına değil doğrudan buna bakıyor:
 * dokunmatik hiç parmakla doğrulanmadığı için (§9b) ham veriyi seri porta
 * dökebilmek gerekiyor.
 *
 * @return dokunma varsa true ve `ux`/`uy` yazılır; yoksa false.
 */
bool pb_lv_dokunma_al(int32_t *ux, int32_t *uy);

/** Panel dışına düşüp reddedilen dokunma karesi sayısı — kullanıcının
 *  gözlediği "~4000'e sıçrama" bunun içinde. Göz gerektirmeyen ölçüm. */
extern uint32_t pb_lv_dokunma_gecersiz;

/**
 * Sonraki `adet` flush alanını seri porta ASCII olarak dök — GÖZ GEREKMEZ.
 *
 * Dökümü sürücünün OKUDUĞU indislemeyle üretiyor, yani hem LVGL'in çizimini
 * hem 90° devrik okumayı aynı anda sınıyor. Terminalde yazı düzgün
 * okunuyorsa bozulma daha aşağıda (panel/hat); okunmuyorsa LVGL tarafında.
 */
void pb_lv_dokum_iste(int adet);

/**
 * Kart framebuffer'ının TAMAMINI seri porta ASCII dök — GÖZ GEREKMEZ.
 *
 * `pb_lv_dokum_iste` yalnızca tek bir flush ALANINI gösteriyor; bu, kartın o
 * anki tam hâlini gösteriyor. İkisinin farkı teşhiste belirleyici:
 *
 *   döküm okunuyor  -> LVGL, yerleşim ve devrik yazım DOĞRU; bozulma panele
 *                      giden yolda (imleç, pencere, DMA) demektir
 *   döküm okunmuyor -> bozulma LVGL/yerleşim tarafında; panele hiç bakmadan
 *                      düzeltilebilir
 *
 * Çıktı ARAYÜZ yöneliminde ve DİLİM DİLİM: LVGL'e ait her dilim için 172
 * satır x 128 sütun, soldan sağa ve yukarıdan aşağıya — yani ekrana bakınca
 * görülmesi gerekenle aynı düzen, beş parça hâlinde.
 */
void pb_lv_kart_fb_dok(void);

#endif /* POKEBIRD_LV_PORT_H */
