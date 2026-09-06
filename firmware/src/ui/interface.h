/**
 * arayuz.h — Cihazın iki ekranlı arayüzü ve aralarındaki kaydırma geçişi
 *
 * Tasarım: `Kus Sesi Arayuz.dc.html` (retro/analog, koyu tema, 640x172).
 *
 *   EKRAN 0 · DİNLEME   sol: en yakın 3 tür (ad + bilimsel ad + güven çubuğu)
 *                       sağ: canlı spektrogram (panele DOĞRUDAN yazılıyor)
 *   EKRAN 1 · GÜNLÜK    o ana kadar tanınan türlerin listesi
 *
 * ⛔ EKRAN SÖZLEŞMESİ: bu modülde panele doğrudan yazan tek satır yok. Çizimin
 * tamamı LVGL'den geçiyor; `lv_port.c` panele her zaman TAM GENİŞLİKTE
 * (0..171 sütun) dikey dilimler hâlinde basıyor. Dar sütun bandına çok satırlı
 * yazma bu panelde KAYIYOR (§9n). Buraya panel çağrısı eklemeyin.
 *
 * ⚠ GEÇİŞ YOLU İKİ TANE — ve bu bilinçli. Kaydırma dokunmatiğe bağlı,
 * dokunmatik ise bugüne kadar hiç parmakla denenmedi (§9b: boşta sabit 0xDB
 * geliyor, "dokunma yok" mu hata mı belirlenemedi). Bu yüzden ekran değiştirme
 * `pb_ui_set_screen`nın arkasında duruyor ve seri porttan da
 * sürülebiliyor: dokunmatik ölü çıksa bile arayüz kullanılabilir kalıyor.
 */
#ifndef POKEBIRD_INTERFACE_H
#define POKEBIRD_INTERFACE_H

#include <stdbool.h>
#include <stdint.h>

#include "ai/decision.h"

#define PB_SCREEN_LISTEN  0
#define PB_SCREEN_LOG   1
#define PB_SCREEN_COUNT   2

/** Arayüzün bir güncellemede göstereceği her şey. Sınıf adlarını çağıran
 *  veriyor: bu modül `siniflar.h`'yi dahil etmiyor, böylece 179 elemanlı
 *  tablonun ikinci bir kopyası flash'a girmiyor. */
typedef struct {
    pb_decision_mode_t kip;
    const char    *tur_ad;          /* gösterilecek tür; yoksa NULL           */
    float          guven;           /* 0..1                                   */

    const char    *ilk3_ad[3];      /* birleştirmenin ilk 3'ü; NULL olabilir  */
    const char    *ilk3_latin[3];   /* bilimsel adlar; NULL olabilir          */
    float          ilk3_olasilik[3];

    /* Alt satırdaki teşhis sayaçları — göz gerektirmeyen doğrulama için
     * ekranda da duruyorlar (seri portta da var). */
    uint32_t kare_hiz;              /* mel karesi / s                         */
    uint32_t cikarim;
    uint32_t birlesen;
    uint32_t overrun;
    float    bant_db;
} pb_result_view_t;

/** İki ekranı da kur. `pb_lv_init()` önce çağrılmış olmalı. */
void pb_ui_create(void);

/**
 * LVGL'i çevir, kaydırmayı yokla, kirli dilimleri panele bas.
 * Ana döngüden düzenli çağrılmalı (eski `pb_lv_tick`in yerine).
 */
void pb_ui_tick(void);

/** Dinleme ekranının içeriğini tazele. */
void pb_ui_update(const pb_result_view_t *g);

/**
 * Günlüğe bir tespit yaz. Aynı tür arka arkaya gelirse yeni satır AÇMIYOR,
 * en üstteki satırı tazeliyor: karar kuralı bir türü 5 saniye ekranda tutuyor
 * (`PB_DECISION_HOLD_MS`) ve o süre boyunca aynı tespit tekrar tekrar düşerse
 * günlük tek bir olayla dolardı.
 */
void pb_ui_log_add(const char *ad, const char *latin, float guven);

/** Etkin ekran (PB_EKRAN_*). */
int  pb_ui_screen(void);

/** Ekranı değiştir. Aralık dışı değer yok sayılır. */
void pb_ui_set_screen(int ekran);

/** Bir sonraki ekrana geç (döngüsel) — seri porttaki yedek yol. */
void pb_ui_next(void);

/* ── Kayıt (dinleme) durumu ───────────────────────────────────────────────
 *
 * Cihaz artık SÜREKLİ DİNLEMİYOR (kullanıcının kararı): dinlemeyi kayıt
 * butonu başlatıp durduruyor ve açılışta kapalı. Butonun kendisi dinleme
 * ekranının sol 96 pikselinde, TAM YÜKSEKLİKTE bir şerit — gerekçesi
 * ekran_dinleme.c'de: dokunmatik kalibrasyonu kısa ekseni kullanılamaz
 * gösterdi, bir dokunuşun yalnızca YATAY yerini biliyoruz.
 *
 * Hattı gerçekten durdurup başlatmak ÇAĞIRANIN işi: bu modül donanıma
 * dokunmuyor, yalnızca durumu ve görünümü tutuyor. `main.c` her turda
 * `pb_ui_recording()`ya bakıp `pb_recognizer_start`/`pb_recognizer_stop`
 * çağırıyor.
 */
bool pb_ui_recording(void);
void pb_ui_set_recording(bool kayitta);

/** Kayıt butonuna kaç kez basıldı — göz gerektirmeyen ölçüm. */
extern uint32_t pb_button_press;

/* ── Kaydırma teşhisi — GÖZ GEREKMEZ ──────────────────────────────────────
 * Dokunmatik doğrulanmadığı için kaydırmanın neden çalışmadığını ekrana
 * bakmadan anlayabilmek şart. Sayaçlar seri porta dökülüyor. */
extern uint32_t pb_swipe_touch;    /* geçerli okunan dokunma karesi     */
extern uint32_t pb_swipe_begin;      /* parmak indi                        */
extern uint32_t pb_swipe_accept;      /* kaydırma sayıldı                   */
extern uint32_t pb_swipe_short;       /* hareket eşiğin altında kaldı       */
extern int32_t  pb_swipe_last_dx;     /* son parmak hareketi (ui px)        */
extern int32_t  pb_swipe_last_dy;

#endif /* POKEBIRD_INTERFACE_H */
