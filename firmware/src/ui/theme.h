/**
 * tema.h — Arayüzün renkleri, yazı tipleri ve ortak çizim yardımcıları
 *
 * Renkler `Kus Sesi Arayuz.dc.html` tasarımından BİREBİR alındı (retro/analog,
 * koyu tema). Tasarımın px değerleri doğrudan cihazın pikseli: ekran 640x172
 * ve tasarım da 640x172 çiziyor, arada ölçek yok.
 *
 * ⚠ YAZI TİPLERİ src/ui/fonts/ altında ÜRETİLMİŞ dosyalar — elle düzenlemeyin,
 * `python tools/generate_fonts.py` ile yeniden üretilirler. Tasarımın Oswald +
 * Space Mono ikilisi Google Fonts'ta; indirme yapmamak için Windows'un kendi
 * yazı tiplerinden aynı role oturan ikisi seçildi (gerekçe font_uret.py'de):
 *
 *     Oswald      -> Liberation Sans Narrow Bold
 *     Space Mono  -> DejaVu Sans Mono (eğik)
 *
 * ⚠ Bu yazı tiplerinde Türkçe harfler VAR (üretim sırasında doğrulanıyor).
 * §9p'deki ASCII indirgeme (`pb_ascii_fold`) artık EKRAN için gerekli değil;
 * tür adları ekrana tam Türkçe yazılıyor.
 */
#ifndef POKEBIRD_THEME_H
#define POKEBIRD_THEME_H

#include "lvgl.h"

/* ── Yazı tipleri (src/ui/fonts/, üretilmiş) ─────────────────────────────── */
extern const lv_font_t pb_font_name_18;      /* tür adı — sıkışık, kalın        */
extern const lv_font_t pb_font_bold_13;   /* başlık, yüzde, günlük satırı    */
extern const lv_font_t pb_font_narrow_11;     /* ikincil bilgi                   */
extern const lv_font_t pb_font_mono_10;    /* bilimsel ad, sayaçlar (eğik)    */

/* ── Renkler — tasarımdan ────────────────────────────────────────────────── */
#define PB_COLOR_BACKGROUND      0x0B0908   /* ekran zemini                         */
#define PB_COLOR_TEXT      0xE8E2D6   /* birincil metin                       */
#define PB_COLOR_MUTED      0x8A8072   /* ikincil metin                        */
#define PB_COLOR_FAINT      0x6F675C   /* üçüncül metin                        */
#define PB_COLOR_LATIN      0x7A7263   /* bilimsel ad                          */
#define PB_COLOR_LINE      0x2A2620   /* ayraç                                */
#define PB_COLOR_ROW      0x211D18   /* satır altı çizgisi / çubuk yatağı    */
#define PB_COLOR_BORDER      0x3A332A   /* rozet kenarı, pasif nokta            */
#define PB_COLOR_INACTIVE      0x5F5849   /* pasif çubuk dolgusu                  */

#define PB_COLOR_ACCENT      0xFFB020   /* kehribar — genel vurgu               */
#define PB_COLOR_PEAK       0x7BD88F   /* yeşil — en yüksek skorlu tür         */
#define PB_COLOR_RECORD      0xE86A5A   /* kırmızı — "dinliyor" noktası         */

/* ── Yerleşim ────────────────────────────────────────────────────────────── */
#define PB_SCREEN_W   640
#define PB_SCREEN_H   172

/* Spektrogram sağdaki İKİ dilimi (128 px x 2) kullanıyor; LVGL soldaki üçü.
 * Tasarım 236 px istiyordu — 256, dilim sınırına oturan en yakın değer ve
 * dilim sınırına oturmak şart: LVGL ile spektrogram aynı dilimi paylaşırsa
 * birbirlerini silerler (lv_port.c). */
#define PB_SPEC_SLICE_COUNT  2
#define PB_LVGL_SLICE_MASK_LISTEN  0x07u   /* dilim 0,1,2 -> ui x 0..383    */
#define PB_LVGL_SLICE_MASK_ALL      0x1Fu   /* beşi de LVGL'in              */

#define PB_LEFT_W     384                     /* dinleme ekranının sol sütunu  */
#define PB_MARGIN     18                      /* sol/sağ iç boşluk             */

/* ── Ortak çizim yardımcıları (tema.c) ───────────────────────────────────── */

/** Zemini, dolgusu ve kenarlığı sıfırlanmış boş bir ekran nesnesi. */
lv_obj_t *pb_screen_new(void);

/** Sol üstten konumlanan etiket. */
lv_obj_t *pb_label(lv_obj_t *par, const lv_font_t *f, uint32_t renk,
                    int32_t x, int32_t y);

/** Düz renk dikdörtgen — çubuk, ayraç çizgisi, nokta. `yaricap` yuvarlaklık. */
lv_obj_t *pb_box(lv_obj_t *par, int32_t x, int32_t y, int32_t w, int32_t h,
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
bool pb_write(lv_obj_t *o, char *son, uint32_t n, const char *metin);

/** Alt ortadaki sayfa noktaları — iki ekran olduğunu gösteren tek işaret. */
void pb_page_dots(lv_obj_t *par, int aktif);

#endif /* POKEBIRD_THEME_H */
