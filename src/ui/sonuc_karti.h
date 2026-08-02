/**
 * sonuc_karti.h — Tanıma sonucunu gösteren LVGL kartı (M7 adım 1)
 *
 * Ekranın sol 200 sütunu bu kart, sağı (200..639) spektrogram — yerleşim
 * `a` demosundan devralındı (bkz. ui/spectrogram.h, ui/lv_port.c).
 *
 * ⛔ EKRAN SÖZLEŞMESİ: burada panele DOĞRUDAN yazan tek satır yok. Çizimin
 * tamamı LVGL üzerinden gidiyor ve lv_port.c'nin flush yolu panele her zaman
 * TAM GENİŞLİKTE (0..171 sütun) basıyor. Dar sütun bandına çok satırlı yazma
 * bu panelde satır başına kayıyor (§9n). Bu dosyaya panel çağrısı eklemeyin.
 *
 * ⚠ YAZI TİPİ: LVGL'in gömülü Montserrat'ında Türkçe harfler (ç ğ ı İ ö ş ü)
 * YOK — yazılırsa kutu çıkar. Kart bu yüzden tür adlarını ASCII'ye
 * indiriyor ("Ak Karınlı Ebabil" -> "Ak Karinli Ebabil"). Gerçek Türkçe
 * gösterimi için lv_font_conv ile özel bir yazı tipi üretilmesi gerekir;
 * ayrı bir iş, sonuç ekranını bekletmiyor.
 */
#ifndef POKEBIRD_SONUC_KARTI_H
#define POKEBIRD_SONUC_KARTI_H

#include <stdbool.h>
#include <stdint.h>

#include "ai/karar.h"

/** Kartın bir güncellemede göstereceği her şey. Sınıf adlarını çağıran
 *  veriyor: bu modül `siniflar.h`'yi dahil etmiyor, böylece 179 elemanlı
 *  tablonun ikinci bir kopyası flash'a girmiyor. */
typedef struct {
    pb_karar_kip_t kip;
    const char    *tur_ad;          /* gösterilecek tür; yoksa NULL          */
    float          guven;           /* 0..1                                  */
    const char    *ilk3_ad[3];      /* birleştirmenin ilk 3'ü; NULL olabilir */
    float          ilk3_olasilik[3];

    /* Alt satırdaki teşhis sayaçları — göz gerektirmeyen doğrulama için
     * ekranda da duruyorlar (seri portta da var). */
    uint32_t kare_hiz;              /* mel karesi / s                        */
    uint32_t cikarim;
    uint32_t birlesen;
    uint32_t overrun;
    float    bant_db;
} pb_sonuc_gorunum_t;

/** Kartı etkin ekrana kur. pb_lv_init() önce çağrılmış olmalı. */
void pb_sonuc_karti_olustur(void);

/** Kartı güncelle. Yalnızca DEĞİŞEN etiketler yeniden çiziliyor: her
 *  güncelleme kartın QSPI'ye yeniden basılması demek (68,8 KB), boşuna
 *  yapılmasın. */
void pb_sonuc_karti_guncelle(const pb_sonuc_gorunum_t *g);

/**
 * UTF-8 Türkçe metni ASCII'ye indir (ç->c, ğ->g, ı->i, İ->I, ö->o, ş->s,
 * ü->u ve şapkalı ünlüler). Çözülemeyen çok baytlı diziler '?' oluyor.
 * Çıktı her zaman sonlandırılıyor.
 */
void pb_ascii_tr(const char *utf8, char *out, uint32_t n);

#endif /* POKEBIRD_SONUC_KARTI_H */
