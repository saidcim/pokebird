#include "ui/ekran_dinleme.h"

#include <stdio.h>
#include <string.h>

#include "ui/metin.h"
#include "ui/tema.h"

/* ── Yerleşim — KAYIT BUTONUNA göre yeniden kuruldu ───────────────────────
 *
 * Cihaz artık sürekli dinlemiyor; dinlemeyi kullanıcı başlatıyor. Ekran da
 * buna göre değişti: solda tam yükseklikte bir kayıt butonu, ortada tek bir
 * ÖNE ÇIKAN sonuç, altında 2. ve 3. tahmin tek satırlık kompakt hâlde.
 *
 * ⛔ BUTON NEDEN TAM YÜKSEKLİKTE BİR ŞERİT — ölçüme dayalı, keyfî değil.
 * Dokunmatik kalibrasyonu (§9r) kısa eksenin KULLANILAMAZ olduğunu gösterdi:
 * ekranın tamamı boyunca yalnızca 14 birim değişiyor (kasanın çıkıntısı üst/
 * alt kenara dokunmayı engelliyor olmalı). Yani bir dokunuşun DİKEY yerini
 * bilemiyoruz, yalnızca YATAY yerini. Tam yükseklikte bir şerit, yalnızca
 * yatay konumla güvenle vurulabilen tek buton biçimi.
 *
 *     x   0..95    BUTON
 *     x  96..383   içerik
 *     x 384..639   spektrogram (LVGL dokunmuyor)
 *
 *     y   8   durum satırı
 *     y  32   1. tahmin: ad (+ sağda güven)
 *     y  54   bilimsel ad
 *     y  74   güven çubuğu
 *     y  98   2. tahmin  (tek satır)
 *     y 116   3. tahmin  (tek satır)
 *     y 162   sayfa noktaları (tema.c)
 */
#define BUTON_W      96
#define IC_X         102                 /* içerik sol kenarı               */
#define IC_SAG       378                 /* içerik sağ kenarı               */

#define AD_W         200
#define AD_H         21
#define LATIN_H      13
#define YUZDE_X      306
#define YUZDE_W      (IC_SAG - YUZDE_X)

#define CUBUK_Y      74
#define CUBUK_H      6
#define CUBUK_W      (IC_SAG - IC_X)

#define ALT_Y0       98
#define ALT_ADIM     18
#define ALT_AD_W     206
#define ALT_YUZDE_X  312

static lv_obj_t *s_ekran;
static lv_obj_t *s_nokta, *s_durum, *s_bos;
static lv_obj_t *s_ad, *s_latin, *s_yuzde, *s_cubuk, *s_cubuk_yatak, *s_ayrac;
static lv_obj_t *s_alt[2], *s_alt_yuzde[2];

/* Buton parçaları */
static lv_obj_t *s_buton, *s_buton_isaret, *s_buton_yazi;

static char s_son_durum[32], s_son_ad[80], s_son_latin[64], s_son_yuzde[12];
static char s_son_alt[2][64], s_son_alt_yuzde[2][12];
static int32_t s_son_cubuk_w = -1;
static uint32_t s_son_renk = 0xFFFFFFFFu;
static const lv_font_t *s_son_font;
static int s_son_liste = -1;
static int s_son_kayit = -1;

/** Sonuç bölgesini tümüyle göster/gizle (aday yokken). */
static void sonuc_goster(bool goster)
{
    lv_obj_t *hepsi[] = { s_ad, s_latin, s_yuzde, s_cubuk, s_cubuk_yatak,
                          s_ayrac, s_alt[0], s_alt[1],
                          s_alt_yuzde[0], s_alt_yuzde[1] };
    for (uint32_t i = 0; i < sizeof(hepsi) / sizeof(hepsi[0]); i++) {
        if (goster) lv_obj_clear_flag(hepsi[i], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(hepsi[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void buton_kur(void)
{
    /* Dış çerçeve — tam yükseklikte şerit. */
    s_buton = lv_obj_create(s_ekran);
    lv_obj_remove_style_all(s_buton);
    lv_obj_set_pos(s_buton, 10, 14);
    lv_obj_set_size(s_buton, BUTON_W - 22, PB_EKRAN_H - 28);
    lv_obj_set_style_radius(s_buton, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_buton, 2, LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_buton, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_buton, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_buton, LV_OBJ_FLAG_SCROLLABLE);

    /* Gösterge: boştayken daire (kayıt), dinlerken kare (dur). Şekli
     * değiştirmek yerine YARIÇAPI değiştiriyoruz — tek nesne, tek çizim. */
    s_buton_isaret = lv_obj_create(s_buton);
    lv_obj_remove_style_all(s_buton_isaret);
    lv_obj_set_size(s_buton_isaret, 26, 26);
    lv_obj_align(s_buton_isaret, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_bg_opa(s_buton_isaret, LV_OPA_COVER, LV_PART_MAIN);

    s_buton_yazi = lv_label_create(s_buton);
    lv_obj_set_style_text_font(s_buton_yazi, &pb_font_kalin_13, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(s_buton_yazi, 2, LV_PART_MAIN);
    lv_obj_align(s_buton_yazi, LV_ALIGN_TOP_MID, 0, 76);
    lv_label_set_text(s_buton_yazi, "KAYIT");
}

lv_obj_t *pb_ekran_dinleme_olustur(void)
{
    s_ekran = pb_ekran_yeni();

    buton_kur();

    /* Butonla içerik arasında ince ayraç. */
    pb_kutu(s_ekran, BUTON_W - 2, 14, 1, PB_EKRAN_H - 28, PB_RENK_SATIR, 0);

    /* ── Durum satırı ──
     * Nokta LVGL animasyonuyla YANIP SÖNMÜYOR: animasyon her karede
     * geçersizleştirir ve dilimi 60 Hz panele bastırırdı. Rengi kiple
     * değişiyor, o yeterli. */
    s_nokta = pb_kutu(s_ekran, IC_X, 11, 7, 7, PB_RENK_SILIK, 4);
    s_durum = pb_etiket(s_ekran, &pb_font_kalin_13, PB_RENK_SOLUK, IC_X + 14, 6);
    lv_obj_set_style_text_letter_space(s_durum, 3, LV_PART_MAIN);

    /* ── 1. tahmin — öne çıkan sonuç ── */
    s_ad = pb_etiket(s_ekran, &pb_font_ad_18, PB_RENK_METIN, IC_X, 30);
    lv_obj_set_size(s_ad, AD_W, AD_H);
    /* ⚠ Yükseklik de veriliyor: `DOTS` yalnızca genişlikle kesmiyor, uzun ad
     * iki satıra sarıp altındakinin üstüne biniyor (§9r'de yakalandı). */
    lv_label_set_long_mode(s_ad, LV_LABEL_LONG_MODE_DOTS);

    s_latin = pb_etiket(s_ekran, &pb_font_mono_10, PB_RENK_LATIN, IC_X + 1, 54);
    lv_obj_set_size(s_latin, AD_W, LATIN_H);
    lv_label_set_long_mode(s_latin, LV_LABEL_LONG_MODE_DOTS);

    s_yuzde = pb_etiket(s_ekran, &pb_font_ad_18, PB_RENK_VURGU, YUZDE_X, 30);
    lv_obj_set_width(s_yuzde, YUZDE_W);
    lv_obj_set_style_text_align(s_yuzde, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    s_cubuk_yatak = pb_kutu(s_ekran, IC_X, CUBUK_Y, CUBUK_W, CUBUK_H,
                            PB_RENK_SATIR, 3);
    s_cubuk = pb_kutu(s_ekran, IC_X, CUBUK_Y, 0, CUBUK_H, PB_RENK_VURGU, 3);

    s_ayrac = pb_kutu(s_ekran, IC_X, 90, CUBUK_W, 1, PB_RENK_SATIR, 0);

    /* ── 2. ve 3. tahmin — tek satır, kompakt ──
     * ARCHITECTURE §4: arayüz tek cevap değil ilk 3 tahmin gösterecek.
     * Top-1 %70,4 ama top-3 %82,2 (§9k), yani bu iki satır gerçekten bilgi
     * veriyor; ama birinciyle aynı ağırlıkta olmamalılar. */
    for (int i = 0; i < 2; i++) {
        const int32_t y = ALT_Y0 + i * ALT_ADIM;
        s_alt[i] = pb_etiket(s_ekran, &pb_font_dar_11, PB_RENK_SOLUK, IC_X, y);
        lv_obj_set_size(s_alt[i], ALT_AD_W, 14);
        lv_label_set_long_mode(s_alt[i], LV_LABEL_LONG_MODE_DOTS);

        s_alt_yuzde[i] = pb_etiket(s_ekran, &pb_font_dar_11, PB_RENK_SILIK,
                                   ALT_YUZDE_X, y);
        lv_obj_set_width(s_alt_yuzde[i], IC_SAG - ALT_YUZDE_X);
        lv_obj_set_style_text_align(s_alt_yuzde[i], LV_TEXT_ALIGN_RIGHT,
                                    LV_PART_MAIN);
        s_son_alt[i][0] = s_son_alt_yuzde[i][0] = '\0';
    }

    /* Boş durum — sonuç yokken üç boş satır göstermek anlamsız. */
    s_bos = pb_etiket(s_ekran, &pb_font_dar_11, PB_RENK_SILIK, IC_X, 74);
    lv_obj_set_width(s_bos, CUBUK_W);
    lv_obj_set_style_text_align(s_bos, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    /* Spektrogramla arasındaki ince ayraç (tasarımdaki `border-left`). */
    pb_kutu(s_ekran, PB_SOL_W - 1, 10, 1, PB_EKRAN_H - 28, PB_RENK_CIZGI, 0);

    pb_sayfa_noktalari(s_ekran, PB_EKRAN_DINLEME);

    s_son_durum[0] = s_son_ad[0] = s_son_latin[0] = s_son_yuzde[0] = '\0';
    s_son_font = &pb_font_ad_18;
    return s_ekran;
}

void pb_ekran_dinleme_kayit_ayarla(bool kayitta)
{
    if (!s_buton || (int)kayitta == s_son_kayit) return;
    s_son_kayit = (int)kayitta;

    /* Dinlerken: kırmızı dolgu + KARE (dur). Boştayken: içi boş çerçeve +
     * DAİRE (kayıt). Renk tek başına yetmiyor — küçük ekranda ve gündüz
     * ışığında şekil farkı daha güvenilir okunuyor. */
    /* Gösterge HER İKİ durumda da kırmızı: boştayken "buraya bas" daveti,
     * dinlerken "kayıtta" uyarısı. Ayrımı ŞEKİL ve ÇERÇEVE taşıyor — soluk
     * gri bir daire butonu devre dışı gibi gösteriyordu. */
    const uint32_t cerceve = kayitta ? PB_RENK_KAYIT : PB_RENK_KENAR;
    const uint32_t yazi    = kayitta ? PB_RENK_KAYIT : PB_RENK_SOLUK;

    lv_obj_set_style_border_color(s_buton, lv_color_hex(cerceve), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_buton, lv_color_hex(PB_RENK_ZEMIN), LV_PART_MAIN);

    lv_obj_set_style_bg_color(s_buton_isaret, lv_color_hex(PB_RENK_KAYIT),
                              LV_PART_MAIN);
    lv_obj_set_style_radius(s_buton_isaret, kayitta ? 3 : 13, LV_PART_MAIN);

    lv_obj_set_style_text_color(s_buton_yazi, lv_color_hex(yazi), LV_PART_MAIN);
    lv_label_set_text(s_buton_yazi, kayitta ? "DUR" : "KAYIT");
}

void pb_ekran_dinleme_guncelle(const pb_sonuc_gorunum_t *g)
{
    if (!g || !s_ekran) return;

    const bool tur      = (g->kip == PB_KARAR_TUR);
    const bool belirsiz = (g->kip == PB_KARAR_BELIRSIZ);
    const bool ses      = (g->kip == PB_KARAR_SES);
    const bool kayitta  = (s_son_kayit == 1);

    /* ── Durum satırı ── */
    const char *durum = !kayitta ? "BOŞTA"
                      : tur ? "TANINDI"
                      : belirsiz ? "OLABİLİR..."
                      : ses ? "SES ALGILANDI"
                            : "DİNLİYOR";
    const uint32_t vurgu = !kayitta ? PB_RENK_KENAR
                         : tur ? PB_RENK_TEPE
                         : belirsiz ? PB_RENK_VURGU
                         : ses ? PB_RENK_TEPE
                               : PB_RENK_KAYIT;

    pb_yaz(s_durum, s_son_durum, sizeof(s_son_durum), durum);
    if (vurgu != s_son_renk) {
        s_son_renk = vurgu;
        lv_obj_set_style_bg_color(s_nokta, lv_color_hex(vurgu), LV_PART_MAIN);
    }

    /* ── Sonuç var mı ── */
    const int liste = (g->ilk3_ad[0] != NULL) ? 1 : 0;
    if (liste != s_son_liste) {
        s_son_liste = liste;
        sonuc_goster(liste == 1);
        if (liste) lv_obj_add_flag(s_bos, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_clear_flag(s_bos, LV_OBJ_FLAG_HIDDEN);
    }
    if (!liste) {
        /* Türkçe yazılabiliyor: üretilen yazı tipleri (tools/font_uret.py)
         * ç ğ ı İ ö ş ü içeriyor ve üretimde doğrulanıyor. */
        lv_label_set_text(s_bos,
            kayitta ? "dinleniyor..." : "kayıt için butona basın");
        return;
    }

    /* ── 1. tahmin ── */
    char buf[80];
    pb_turkce_buyut(g->ilk3_ad[0], buf, sizeof(buf));

    /* Sığmayan adı KESMEK yerine önce KÜÇÜLT: 178 türün birkaçı 18 px'e
     * sığmıyor ve bir kademe küçük yazıyla tamamı okunuyor. */
    const lv_font_t *font =
        (lv_text_get_width(buf, (uint32_t)strlen(buf), &pb_font_ad_18, 0) > AD_W)
            ? &pb_font_kalin_13 : &pb_font_ad_18;
    if (font != s_son_font) {
        s_son_font = font;
        lv_obj_set_style_text_font(s_ad, font, LV_PART_MAIN);
    }
    pb_yaz(s_ad, s_son_ad, sizeof(s_son_ad), buf);
    pb_yaz(s_latin, s_son_latin, sizeof(s_son_latin),
           g->ilk3_latin[0] ? g->ilk3_latin[0] : "");

    char y[12];
    snprintf(y, sizeof(y), "%d%%", (int)(g->ilk3_olasilik[0] * 100.0f + 0.5f));
    pb_yaz(s_yuzde, s_son_yuzde, sizeof(s_son_yuzde), y);

    /* Vurgu rengi: yalnızca karar bir şey söylüyorsa. Aksi hâlde nötr —
     * ekran "tanıdım" demiyor. */
    const uint32_t ad_renk = tur ? PB_RENK_TEPE
                           : belirsiz ? PB_RENK_VURGU : PB_RENK_METIN;
    lv_obj_set_style_text_color(s_ad, lv_color_hex(ad_renk), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_yuzde, lv_color_hex(ad_renk), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_cubuk, lv_color_hex(ad_renk), LV_PART_MAIN);

    int32_t w = (int32_t)(g->ilk3_olasilik[0] * (float)CUBUK_W + 0.5f);
    if (w > CUBUK_W) w = CUBUK_W;
    if (w != s_son_cubuk_w) { s_son_cubuk_w = w; lv_obj_set_width(s_cubuk, w); }

    /* ── 2. ve 3. tahmin ── */
    for (int i = 0; i < 2; i++) {
        const char *ham = g->ilk3_ad[i + 1];
        char satir[80];
        if (ham) {
            char b2[64];
            pb_turkce_buyut(ham, b2, sizeof(b2));
            snprintf(satir, sizeof(satir), "%d. %s", i + 2, b2);
        } else {
            satir[0] = '\0';
        }
        pb_yaz(s_alt[i], s_son_alt[i], sizeof(s_son_alt[i]), satir);

        char y2[12];
        if (ham) snprintf(y2, sizeof(y2), "%d%%",
                          (int)(g->ilk3_olasilik[i + 1] * 100.0f + 0.5f));
        else     y2[0] = '\0';
        pb_yaz(s_alt_yuzde[i], s_son_alt_yuzde[i], sizeof(s_son_alt_yuzde[i]), y2);
    }
}
