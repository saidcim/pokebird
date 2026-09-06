#include "ui/screen_listen.h"

#include <stdio.h>
#include <string.h>

#include "ui/text.h"
#include "ui/theme.h"

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
    lv_obj_set_size(s_buton, BUTON_W - 22, PB_SCREEN_H - 28);
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
    lv_obj_set_style_text_font(s_buton_yazi, &pb_font_bold_13, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(s_buton_yazi, 2, LV_PART_MAIN);
    lv_obj_align(s_buton_yazi, LV_ALIGN_TOP_MID, 0, 76);
    lv_label_set_text(s_buton_yazi, "RECORD");
}

lv_obj_t *pb_screen_listen_create(void)
{
    s_ekran = pb_screen_new();

    buton_kur();

    /* Butonla içerik arasında ince ayraç. */
    pb_box(s_ekran, BUTON_W - 2, 14, 1, PB_SCREEN_H - 28, PB_COLOR_ROW, 0);

    /* ── Durum satırı ──
     * Nokta LVGL animasyonuyla YANIP SÖNMÜYOR: animasyon her karede
     * geçersizleştirir ve dilimi 60 Hz panele bastırırdı. Rengi kiple
     * değişiyor, o yeterli. */
    s_nokta = pb_box(s_ekran, IC_X, 11, 7, 7, PB_COLOR_FAINT, 4);
    s_durum = pb_label(s_ekran, &pb_font_bold_13, PB_COLOR_MUTED, IC_X + 14, 6);
    lv_obj_set_style_text_letter_space(s_durum, 3, LV_PART_MAIN);

    /* ── 1. tahmin — öne çıkan sonuç ── */
    s_ad = pb_label(s_ekran, &pb_font_name_18, PB_COLOR_TEXT, IC_X, 30);
    lv_obj_set_size(s_ad, AD_W, AD_H);
    /* ⚠ Yükseklik de veriliyor: `DOTS` yalnızca genişlikle kesmiyor, uzun ad
     * iki satıra sarıp altındakinin üstüne biniyor (§9r'de yakalandı). */
    lv_label_set_long_mode(s_ad, LV_LABEL_LONG_MODE_DOTS);

    s_latin = pb_label(s_ekran, &pb_font_mono_10, PB_COLOR_LATIN, IC_X + 1, 54);
    lv_obj_set_size(s_latin, AD_W, LATIN_H);
    lv_label_set_long_mode(s_latin, LV_LABEL_LONG_MODE_DOTS);

    s_yuzde = pb_label(s_ekran, &pb_font_name_18, PB_COLOR_ACCENT, YUZDE_X, 30);
    lv_obj_set_width(s_yuzde, YUZDE_W);
    lv_obj_set_style_text_align(s_yuzde, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    s_cubuk_yatak = pb_box(s_ekran, IC_X, CUBUK_Y, CUBUK_W, CUBUK_H,
                            PB_COLOR_ROW, 3);
    s_cubuk = pb_box(s_ekran, IC_X, CUBUK_Y, 0, CUBUK_H, PB_COLOR_ACCENT, 3);

    s_ayrac = pb_box(s_ekran, IC_X, 90, CUBUK_W, 1, PB_COLOR_ROW, 0);

    /* ── 2. ve 3. tahmin — tek satır, kompakt ──
     * ARCHITECTURE §4: arayüz tek cevap değil ilk 3 tahmin gösterecek.
     * Top-1 %70,4 ama top-3 %82,2 (§9k), yani bu iki satır gerçekten bilgi
     * veriyor; ama birinciyle aynı ağırlıkta olmamalılar. */
    for (int i = 0; i < 2; i++) {
        const int32_t y = ALT_Y0 + i * ALT_ADIM;
        s_alt[i] = pb_label(s_ekran, &pb_font_narrow_11, PB_COLOR_MUTED, IC_X, y);
        lv_obj_set_size(s_alt[i], ALT_AD_W, 14);
        lv_label_set_long_mode(s_alt[i], LV_LABEL_LONG_MODE_DOTS);

        s_alt_yuzde[i] = pb_label(s_ekran, &pb_font_narrow_11, PB_COLOR_FAINT,
                                   ALT_YUZDE_X, y);
        lv_obj_set_width(s_alt_yuzde[i], IC_SAG - ALT_YUZDE_X);
        lv_obj_set_style_text_align(s_alt_yuzde[i], LV_TEXT_ALIGN_RIGHT,
                                    LV_PART_MAIN);
        s_son_alt[i][0] = s_son_alt_yuzde[i][0] = '\0';
    }

    /* Boş durum — sonuç yokken üç boş satır göstermek anlamsız. */
    s_bos = pb_label(s_ekran, &pb_font_narrow_11, PB_COLOR_FAINT, IC_X, 74);
    lv_obj_set_width(s_bos, CUBUK_W);
    lv_obj_set_style_text_align(s_bos, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    /* Spektrogramla arasındaki ince ayraç (tasarımdaki `border-left`). */
    pb_box(s_ekran, PB_LEFT_W - 1, 10, 1, PB_SCREEN_H - 28, PB_COLOR_LINE, 0);

    pb_page_dots(s_ekran, PB_SCREEN_LISTEN);

    s_son_durum[0] = s_son_ad[0] = s_son_latin[0] = s_son_yuzde[0] = '\0';
    s_son_font = &pb_font_name_18;
    return s_ekran;
}

void pb_screen_listen_set_recording(bool kayitta)
{
    if (!s_buton || (int)kayitta == s_son_kayit) return;
    s_son_kayit = (int)kayitta;

    /* Dinlerken: kırmızı dolgu + KARE (dur). Boştayken: içi boş çerçeve +
     * DAİRE (kayıt). Renk tek başına yetmiyor — küçük ekranda ve gündüz
     * ışığında şekil farkı daha güvenilir okunuyor. */
    /* Gösterge HER İKİ durumda da kırmızı: boştayken "buraya bas" daveti,
     * dinlerken "kayıtta" uyarısı. Ayrımı ŞEKİL ve ÇERÇEVE taşıyor — soluk
     * gri bir daire butonu devre dışı gibi gösteriyordu. */
    const uint32_t cerceve = kayitta ? PB_COLOR_RECORD : PB_COLOR_BORDER;
    const uint32_t yazi    = kayitta ? PB_COLOR_RECORD : PB_COLOR_MUTED;

    lv_obj_set_style_border_color(s_buton, lv_color_hex(cerceve), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_buton, lv_color_hex(PB_COLOR_BACKGROUND), LV_PART_MAIN);

    lv_obj_set_style_bg_color(s_buton_isaret, lv_color_hex(PB_COLOR_RECORD),
                              LV_PART_MAIN);
    lv_obj_set_style_radius(s_buton_isaret, kayitta ? 3 : 13, LV_PART_MAIN);

    lv_obj_set_style_text_color(s_buton_yazi, lv_color_hex(yazi), LV_PART_MAIN);
    lv_label_set_text(s_buton_yazi, kayitta ? "STOP" : "RECORD");
}

void pb_screen_listen_update(const pb_result_view_t *g)
{
    if (!g || !s_ekran) return;

    const bool tur      = (g->kip == PB_DECISION_SPECIES);
    const bool belirsiz = (g->kip == PB_DECISION_UNSURE);
    const bool ses      = (g->kip == PB_DECISION_SOUND);
    const bool kayitta  = (s_son_kayit == 1);

    /* ── Durum satırı ── */
    const char *durum = !kayitta ? "IDLE"
                      : tur ? "IDENTIFIED"
                      : belirsiz ? "MAYBE..."
                      : ses ? "SOUND DETECTED"
                            : "LISTENING";
    const uint32_t vurgu = !kayitta ? PB_COLOR_BORDER
                         : tur ? PB_COLOR_PEAK
                         : belirsiz ? PB_COLOR_ACCENT
                         : ses ? PB_COLOR_PEAK
                               : PB_COLOR_RECORD;

    pb_write(s_durum, s_son_durum, sizeof(s_son_durum), durum);
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
        /* Türkçe yazılabiliyor: üretilen yazı tipleri (tools/generate_fonts.py)
         * ç ğ ı İ ö ş ü içeriyor ve üretimde doğrulanıyor. */
        lv_label_set_text(s_bos,
            kayitta ? "listening..." : "press the button to record");
        return;
    }

    /* ── 1. tahmin ── */
    char buf[80];
    pb_text_upper(g->ilk3_ad[0], buf, sizeof(buf));

    /* Sığmayan adı KESMEK yerine önce KÜÇÜLT: 178 türün birkaçı 18 px'e
     * sığmıyor ve bir kademe küçük yazıyla tamamı okunuyor. */
    const lv_font_t *font =
        (lv_text_get_width(buf, (uint32_t)strlen(buf), &pb_font_name_18, 0) > AD_W)
            ? &pb_font_bold_13 : &pb_font_name_18;
    if (font != s_son_font) {
        s_son_font = font;
        lv_obj_set_style_text_font(s_ad, font, LV_PART_MAIN);
    }
    pb_write(s_ad, s_son_ad, sizeof(s_son_ad), buf);
    pb_write(s_latin, s_son_latin, sizeof(s_son_latin),
           g->ilk3_latin[0] ? g->ilk3_latin[0] : "");

    char y[12];
    snprintf(y, sizeof(y), "%d%%", (int)(g->ilk3_olasilik[0] * 100.0f + 0.5f));
    pb_write(s_yuzde, s_son_yuzde, sizeof(s_son_yuzde), y);

    /* Vurgu rengi: yalnızca karar bir şey söylüyorsa. Aksi hâlde nötr —
     * ekran "tanıdım" demiyor. */
    const uint32_t ad_renk = tur ? PB_COLOR_PEAK
                           : belirsiz ? PB_COLOR_ACCENT : PB_COLOR_TEXT;
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
            pb_text_upper(ham, b2, sizeof(b2));
            snprintf(satir, sizeof(satir), "%d. %s", i + 2, b2);
        } else {
            satir[0] = '\0';
        }
        pb_write(s_alt[i], s_son_alt[i], sizeof(s_son_alt[i]), satir);

        char y2[12];
        if (ham) snprintf(y2, sizeof(y2), "%d%%",
                          (int)(g->ilk3_olasilik[i + 1] * 100.0f + 0.5f));
        else     y2[0] = '\0';
        pb_write(s_alt_yuzde[i], s_son_alt_yuzde[i], sizeof(s_son_alt_yuzde[i]), y2);
    }
}
