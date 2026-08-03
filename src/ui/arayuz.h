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
 * `pb_arayuz_ekran_ayarla`nın arkasında duruyor ve seri porttan da
 * sürülebiliyor: dokunmatik ölü çıksa bile arayüz kullanılabilir kalıyor.
 */
#ifndef POKEBIRD_ARAYUZ_H
#define POKEBIRD_ARAYUZ_H

#include <stdbool.h>
#include <stdint.h>

#include "ai/karar.h"

#define PB_EKRAN_DINLEME  0
#define PB_EKRAN_GUNLUK   1
#define PB_EKRAN_SAYISI   2

/** Arayüzün bir güncellemede göstereceği her şey. Sınıf adlarını çağıran
 *  veriyor: bu modül `siniflar.h`'yi dahil etmiyor, böylece 179 elemanlı
 *  tablonun ikinci bir kopyası flash'a girmiyor. */
typedef struct {
    pb_karar_kip_t kip;
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
} pb_sonuc_gorunum_t;

/** İki ekranı da kur. `pb_lv_init()` önce çağrılmış olmalı. */
void pb_arayuz_olustur(void);

/**
 * LVGL'i çevir, kaydırmayı yokla, kirli dilimleri panele bas.
 * Ana döngüden düzenli çağrılmalı (eski `pb_lv_tick`in yerine).
 */
void pb_arayuz_tick(void);

/** Dinleme ekranının içeriğini tazele. */
void pb_arayuz_guncelle(const pb_sonuc_gorunum_t *g);

/**
 * Günlüğe bir tespit yaz. Aynı tür arka arkaya gelirse yeni satır AÇMIYOR,
 * en üstteki satırı tazeliyor: karar kuralı bir türü 5 saniye ekranda tutuyor
 * (`PB_KARAR_TUT_MS`) ve o süre boyunca aynı tespit tekrar tekrar düşerse
 * günlük tek bir olayla dolardı.
 */
void pb_arayuz_gunluge_ekle(const char *ad, const char *latin, float guven);

/** Etkin ekran (PB_EKRAN_*). */
int  pb_arayuz_ekran(void);

/** Ekranı değiştir. Aralık dışı değer yok sayılır. */
void pb_arayuz_ekran_ayarla(int ekran);

/** Bir sonraki ekrana geç (döngüsel) — seri porttaki yedek yol. */
void pb_arayuz_sonraki(void);

/* ── Kaydırma teşhisi — GÖZ GEREKMEZ ──────────────────────────────────────
 * Dokunmatik doğrulanmadığı için kaydırmanın neden çalışmadığını ekrana
 * bakmadan anlayabilmek şart. Sayaçlar seri porta dökülüyor. */
extern uint32_t pb_kaydirma_dokunma;    /* geçerli okunan dokunma karesi     */
extern uint32_t pb_kaydirma_basla;      /* parmak indi                        */
extern uint32_t pb_kaydirma_kabul;      /* kaydırma sayıldı                   */
extern uint32_t pb_kaydirma_kisa;       /* hareket eşiğin altında kaldı       */
extern int32_t  pb_kaydirma_son_dx;     /* son parmak hareketi (ui px)        */
extern int32_t  pb_kaydirma_son_dy;

#endif /* POKEBIRD_ARAYUZ_H */
