/**
 * tema.h — Arayüzün renkleri, yazı tipleri ve ortak çizim yardımcıları
 *
 * Renkler `Kus Sesi Arayuz.dc.html` tasarımından BİREBİR alındı (retro/analog,
 * koyu tema). Tasarımın px değerleri doğrudan cihazın pikseli: ekran 640x172
 * ve tasarım da 640x172 çiziyor, arada ölçek yok.
 *
 * ⚠ YAZI TİPLERİ src/ui/fonts/ altında ÜRETİLMİŞ dosyalar — elle düzenlemeyin,
 * `python tools/font_uret.py` ile yeniden üretilirler. Tasarımın Oswald +
 * Space Mono ikilisi Google Fonts'ta; indirme yapmamak için Windows'un kendi
 * yazı tiplerinden aynı role oturan ikisi seçildi (gerekçe font_uret.py'de):
 *
 *     Oswald      -> Liberation Sans Narrow Bold
 *     Space Mono  -> DejaVu Sans Mono (eğik)
 *
 * ⚠ Bu yazı tiplerinde Türkçe harfler VAR (üretim sırasında doğrulanıyor).
 * §9p'deki ASCII indirgeme (`pb_ascii_tr`) artık EKRAN için gerekli değil;
 * tür adları ekrana tam Türkçe yazılıyor.
 */
#ifndef POKEBIRD_TEMA_H
#define POKEBIRD_TEMA_H

#include "lvgl.h"

/* ── Yazı tipleri (src/ui/fonts/, üretilmiş) ─────────────────────────────── */
extern const lv_font_t pb_font_ad_18;      /* tür adı — sıkışık, kalın        */
extern const lv_font_t pb_font_kalin_13;   /* başlık, yüzde, günlük satırı    */
extern const lv_font_t pb_font_dar_11;     /* ikincil bilgi                   */
extern const lv_font_t pb_font_mono_10;    /* bilimsel ad, sayaçlar (eğik)    */

/* ── Renkler — tasarımdan ────────────────────────────────────────────────── */
#define PB_RENK_ZEMIN      0x0B0908   /* ekran zemini                         */
#define PB_RENK_METIN      0xE8E2D6   /* birincil metin                       */
#define PB_RENK_SOLUK      0x8A8072   /* ikincil metin                        */
#define PB_RENK_SILIK      0x6F675C   /* üçüncül metin                        */
#define PB_RENK_LATIN      0x7A7263   /* bilimsel ad                          */
#define PB_RENK_CIZGI      0x2A2620   /* ayraç                                */
#define PB_RENK_SATIR      0x211D18   /* satır altı çizgisi / çubuk yatağı    */
#define PB_RENK_KENAR      0x3A332A   /* rozet kenarı, pasif nokta            */
#define PB_RENK_PASIF      0x5F5849   /* pasif çubuk dolgusu                  */

#define PB_RENK_VURGU      0xFFB020   /* kehribar — genel vurgu               */
#define PB_RENK_TEPE       0x7BD88F   /* yeşil — en yüksek skorlu tür         */
#define PB_RENK_KAYIT      0xE86A5A   /* kırmızı — "dinliyor" noktası         */

/* ── Yerleşim ────────────────────────────────────────────────────────────── */
#define PB_EKRAN_W   640
#define PB_EKRAN_H   172

/* Spektrogram sağdaki İKİ dilimi (128 px x 2) kullanıyor; LVGL soldaki üçü.
 * Tasarım 236 px istiyordu — 256, dilim sınırına oturan en yakın değer ve
 * dilim sınırına oturmak şart: LVGL ile spektrogram aynı dilimi paylaşırsa
 * birbirlerini silerler (lv_port.c). */
#define PB_SPEC_DILIM_SAYISI  2
#define PB_LVGL_DILIM_MASKE_DINLEME  0x07u   /* dilim 0,1,2 -> ui x 0..383    */
#define PB_LVGL_DILIM_MASKE_TAM      0x1Fu   /* beşi de LVGL'in              */

#define PB_SOL_W     384                     /* dinleme ekranının sol sütunu  */
#define PB_KENAR     18                      /* sol/sağ iç boşluk             */

/* ── Ortak çizim yardımcıları (tema.c) ───────────────────────────────────── */

/** Zemini, dolgusu ve kenarlığı sıfırlanmış boş bir ekran nesnesi. */
lv_obj_t *pb_ekran_yeni(void);

/** Sol üstten konumlanan etiket. */
lv_obj_t *pb_etiket(lv_obj_t *par, const lv_font_t *f, uint32_t renk,
                    int32_t x, int32_t y);

/** Düz renk dikdörtgen — çubuk, ayraç çizgisi, nokta. `yaricap` yuvarlaklık. */
lv_obj_t *pb_kutu(lv_obj_t *par, int32_t x, int32_t y, int32_t w, int32_t h,
                  uint32_t renk, int32_t yaricap);

/**
 * Etikete metni YALNIZCA DEĞİŞTİYSE yaz.
 *
 * `lv_label_set_text` metin aynı olsa da nesneyi kirletiyor; kirli alan da o
 * dilimin QSPI'ye yeniden basılması (44 KB) demek. Ekran 4 Hz güncelleniyor,
 * boşuna basmanın bedeli gerçek.
 *
 * @return metin değiştiyse true (çağıran rengi de güncellemek isteyebilir).
 */
bool pb_yaz(lv_obj_t *o, char *son, uint32_t n, const char *metin);

/** Alt ortadaki sayfa noktaları — iki ekran olduğunu gösteren tek işaret. */
void pb_sayfa_noktalari(lv_obj_t *par, int aktif);

#endif /* POKEBIRD_TEMA_H */
