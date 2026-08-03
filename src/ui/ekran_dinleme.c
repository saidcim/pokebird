#include "ui/ekran_dinleme.h"

#include <stdio.h>
#include <string.h>

#include "ui/metin.h"
#include "ui/tema.h"

/* ── Yerleşim ─────────────────────────────────────────────────────────────
 * Tasarımın (Kus Sesi Arayuz.dc.html) sol sütunu, LVGL'in sahip olduğu
 * 384 piksele oturtuldu. Tasarımda sol sütun ~340 px genişti; burada 384
 * var, yani sıkıştırma değil biraz nefes payı.
 *
 *     y   6   durum satırı: kayıt noktası + kip
 *     y  34   1. aday   \
 *     y  78   2. aday    >  satır yüksekliği 44
 *     y 122   3. aday   /
 *     y 162   sayfa noktaları (tema.c)
 *
 * Son satır 153'te bitiyor, noktalar 162'de; 172'yi aşan yok.             */
#define SATIR_Y0     34
#define SATIR_ADIM   44

#define ROZET_X      16
#define ROZET_BOY    20
#define AD_X         44
#define AD_W         206         /* AD_X .. 250 — çubuğa çarpmadan          */
#define CUBUK_X      258
#define CUBUK_W      62
#define YUZDE_X      326
#define YUZDE_W      40

typedef struct {
    lv_obj_t *rozet, *rozet_yazi;
    lv_obj_t *ad, *latin;
    lv_obj_t *cubuk_yatak, *cubuk;
    lv_obj_t *yuzde;

    char son_ad[80];
    char son_latin[64];
    char son_yuzde[12];
    int32_t son_cubuk_w;
    uint32_t son_renk;
} aday_t;

static lv_obj_t *s_ekran;
static lv_obj_t *s_nokta, *s_durum;
static aday_t    s_aday[3];

static char s_son_durum[32];
static uint32_t s_son_nokta_renk;

/** Rozet: 1. sıra dolu daire, diğerleri yalnızca kenarlık. */
static void rozet_boya(aday_t *a, uint32_t renk, bool dolu)
{
    if (dolu) {
        lv_obj_set_style_bg_color(a->rozet, lv_color_hex(renk), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(a->rozet, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(a->rozet, lv_color_hex(renk), LV_PART_MAIN);
        lv_obj_set_style_text_color(a->rozet_yazi, lv_color_hex(PB_RENK_ZEMIN),
                                    LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_opa(a->rozet, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(a->rozet, lv_color_hex(PB_RENK_KENAR),
                                      LV_PART_MAIN);
        lv_obj_set_style_text_color(a->rozet_yazi, lv_color_hex(PB_RENK_SOLUK),
                                    LV_PART_MAIN);
    }
}

static void aday_kur(aday_t *a, int sira, int32_t y)
{
    a->rozet = lv_obj_create(s_ekran);
    lv_obj_remove_style_all(a->rozet);
    lv_obj_set_pos(a->rozet, ROZET_X, y + 2);
    lv_obj_set_size(a->rozet, ROZET_BOY, ROZET_BOY);
    lv_obj_set_style_radius(a->rozet, ROZET_BOY / 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(a->rozet, 1, LV_PART_MAIN);
    lv_obj_set_style_border_opa(a->rozet, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(a->rozet, LV_OBJ_FLAG_SCROLLABLE);

    a->rozet_yazi = lv_label_create(a->rozet);
    lv_obj_set_style_text_font(a->rozet_yazi, &pb_font_kalin_13, LV_PART_MAIN);
    lv_obj_center(a->rozet_yazi);
    char n[4];
    snprintf(n, sizeof(n), "%d", sira + 1);
    lv_label_set_text(a->rozet_yazi, n);

    a->ad = pb_etiket(s_ekran, &pb_font_ad_18, PB_RENK_METIN, AD_X, y - 2);
    lv_obj_set_width(a->ad, AD_W);
    /* Kesme (DOT) seçildi, kaydırma (SCROLL) DEĞİL: kayan yazı her karede
     * kendini geçersizleştirir, bizde her geçersizleştirme bir dilimin
     * panele yeniden basılması (44 KB) demek. Uzun ad kesilir, tam hâli
     * seri portta yazılı. */
    lv_label_set_long_mode(a->ad, LV_LABEL_LONG_MODE_DOTS);

    a->latin = pb_etiket(s_ekran, &pb_font_mono_10, PB_RENK_LATIN, AD_X + 1, y + 19);
    lv_obj_set_width(a->latin, AD_W);
    lv_label_set_long_mode(a->latin, LV_LABEL_LONG_MODE_DOTS);

    a->cubuk_yatak = pb_kutu(s_ekran, CUBUK_X, y + 12, CUBUK_W, 5,
                             PB_RENK_SATIR, 3);
    a->cubuk = pb_kutu(s_ekran, CUBUK_X, y + 12, 0, 5, PB_RENK_PASIF, 3);

    a->yuzde = pb_etiket(s_ekran, &pb_font_kalin_13, PB_RENK_SOLUK,
                         YUZDE_X, y + 5);
    lv_obj_set_width(a->yuzde, YUZDE_W);
    lv_obj_set_style_text_align(a->yuzde, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    a->son_ad[0] = a->son_latin[0] = a->son_yuzde[0] = '\0';
    a->son_cubuk_w = -1;
    a->son_renk = 0xFFFFFFFFu;

    rozet_boya(a, PB_RENK_SOLUK, false);
}

lv_obj_t *pb_ekran_dinleme_olustur(void)
{
    s_ekran = pb_ekran_yeni();

    /* Durum satırı: yanıp sönen kayıt noktası + kip adı. Nokta LVGL
     * animasyonuyla YANIP SÖNMÜYOR — animasyon her karede geçersizleştirir ve
     * dilimi 60 Hz panele bastırırdı. Rengi kiple değişiyor, o yeterli. */
    s_nokta = pb_kutu(s_ekran, PB_KENAR, 11, 7, 7, PB_RENK_KAYIT, 4);

    s_durum = pb_etiket(s_ekran, &pb_font_kalin_13, PB_RENK_SOLUK, PB_KENAR + 14, 6);
    lv_obj_set_style_text_letter_space(s_durum, 3, LV_PART_MAIN);

    for (int i = 0; i < 3; i++) {
        aday_kur(&s_aday[i], i, SATIR_Y0 + i * SATIR_ADIM);
        /* Satır altı ince ayraç — tasarımdaki border-bottom. */
        if (i < 2) {
            pb_kutu(s_ekran, ROZET_X, SATIR_Y0 + i * SATIR_ADIM + 36,
                    YUZDE_X + YUZDE_W - ROZET_X, 1, PB_RENK_SATIR, 0);
        }
    }

    pb_sayfa_noktalari(s_ekran, PB_EKRAN_DINLEME);

    s_son_durum[0] = '\0';
    s_son_nokta_renk = 0xFFFFFFFFu;
    return s_ekran;
}

void pb_ekran_dinleme_guncelle(const pb_sonuc_gorunum_t *g)
{
    if (!g || !s_ekran) return;

    const bool tur      = (g->kip == PB_KARAR_TUR);
    const bool belirsiz = (g->kip == PB_KARAR_BELIRSIZ);
    const bool ses      = (g->kip == PB_KARAR_SES);

    /* ── Durum satırı ── */
    const char *durum = tur ? "TANINDI"
                      : belirsiz ? "OLABİLİR..."
                      : ses ? "SES ALGILANDI"
                            : "DİNLİYOR";
    const uint32_t vurgu = tur ? PB_RENK_TEPE
                         : belirsiz ? PB_RENK_VURGU
                         : ses ? PB_RENK_TEPE
                               : PB_RENK_KAYIT;

    if (pb_yaz(s_durum, s_son_durum, sizeof(s_son_durum), durum)) {
        lv_obj_set_style_text_color(s_durum, lv_color_hex(PB_RENK_SOLUK),
                                    LV_PART_MAIN);
    }
    if (vurgu != s_son_nokta_renk) {
        s_son_nokta_renk = vurgu;
        lv_obj_set_style_bg_color(s_nokta, lv_color_hex(vurgu), LV_PART_MAIN);
    }

    /* ── Üç aday ── */
    for (int i = 0; i < 3; i++) {
        aday_t *a = &s_aday[i];
        const char *ham = g->ilk3_ad[i];
        const float p = g->ilk3_olasilik[i];

        char buf[80];
        if (ham) pb_turkce_buyut(ham, buf, sizeof(buf));
        else     snprintf(buf, sizeof(buf), "—");
        pb_yaz(a->ad, a->son_ad, sizeof(a->son_ad), buf);

        pb_yaz(a->latin, a->son_latin, sizeof(a->son_latin),
               (ham && g->ilk3_latin[i]) ? g->ilk3_latin[i] : "");

        char y[12];
        if (ham) snprintf(y, sizeof(y), "%d%%", (int)(p * 100.0f + 0.5f));
        else     y[0] = '\0';
        pb_yaz(a->yuzde, a->son_yuzde, sizeof(a->son_yuzde), y);

        /* Çubuk genişliği — yalnızca değiştiyse. */
        int32_t w = ham ? (int32_t)(p * (float)CUBUK_W + 0.5f) : 0;
        if (w > CUBUK_W) w = CUBUK_W;
        if (w != a->son_cubuk_w) {
            a->son_cubuk_w = w;
            lv_obj_set_width(a->cubuk, w);
        }

        /* Renk: yalnızca 1. sıra ve yalnızca karar bir şey söylüyorsa
         * vurgulanıyor. Aksi hâlde üçü de soluk — ekran "tanıdım" demiyor. */
        const bool one_cikan = (i == 0) && (tur || belirsiz) && ham;
        const uint32_t renk = one_cikan ? (tur ? PB_RENK_TEPE : PB_RENK_VURGU)
                                        : PB_RENK_METIN;
        if (renk != a->son_renk) {
            a->son_renk = renk;
            lv_obj_set_style_text_color(a->ad, lv_color_hex(ham ? renk : PB_RENK_SILIK),
                                        LV_PART_MAIN);
            lv_obj_set_style_text_color(a->latin,
                lv_color_hex(one_cikan ? renk : PB_RENK_LATIN), LV_PART_MAIN);
            lv_obj_set_style_text_color(a->yuzde,
                lv_color_hex(one_cikan ? renk : PB_RENK_SOLUK), LV_PART_MAIN);
            lv_obj_set_style_bg_color(a->cubuk,
                lv_color_hex(one_cikan ? renk : PB_RENK_PASIF), LV_PART_MAIN);
            rozet_boya(a, renk, one_cikan);
        }
    }
}
