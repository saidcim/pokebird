#include "ui/sonuc_karti.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

/* ── Yerleşim ─────────────────────────────────────────────────────────────
 * Arayüz 200 x 172 (lv_port.c). Satır yükseklikleri: Montserrat 14 ≈ 17 px,
 * Montserrat 20 ≈ 24 px. Aşağıdaki y değerleri o iki sayıdan çıktı; toplam
 * 172'yi aşmıyor:
 *
 *     y   2   durum                          (14)   |  güven, sağda (20)
 *     y  30   TÜR ADI, 2 satıra kadar sarar  (20)
 *     y  80   2. ve 3. tahmin                (14, 2 satır)
 *     y 118   sayaçlar                       (14, 3 satır)
 */
#define KENAR       6
#define GENISLIK    (200 - 2 * KENAR)

#define RENK_ZEMIN     0x101820
#define RENK_SOLUK     0x8090A0
#define RENK_NORMAL    0xB0B8C0
#define RENK_SES       0x40E060   /* yeşil — kapı açık                      */
#define RENK_BELIRSIZ  0xF0A030   /* kehribar — eşikler arasında            */
#define RENK_TUR       0xF0C000   /* sarı — girme eşiğinin üstünde          */

static lv_obj_t *s_durum, *s_guven, *s_ad, *s_alt3, *s_sayac;

/* Etiketleri boşuna güncellememek için son yazılan metinler. lv_label_set_text
 * metin aynı olsa da nesneyi kirletiyor; kirli alan da kartın QSPI'ye yeniden
 * basılması demek. */
static char s_son_durum[24], s_son_guven[12], s_son_ad[64],
            s_son_alt3[96], s_son_sayac[96];

void pb_ascii_tr(const char *utf8, char *out, uint32_t n) {
    if (!out || n == 0) return;
    if (!utf8) { out[0] = '\0'; return; }

    uint32_t j = 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p && j + 1 < n; ) {
        if (*p < 0x80) { out[j++] = (char)*p++; continue; }

        char c = '?';
        if (p[1]) {
            const uint16_t iki = (uint16_t)((p[0] << 8) | p[1]);
            switch (iki) {
                case 0xC387: c = 'C'; break;   /* Ç */
                case 0xC3A7: c = 'c'; break;   /* ç */
                case 0xC396: c = 'O'; break;   /* Ö */
                case 0xC3B6: c = 'o'; break;   /* ö */
                case 0xC39C: c = 'U'; break;   /* Ü */
                case 0xC3BC: c = 'u'; break;   /* ü */
                case 0xC49E: c = 'G'; break;   /* Ğ */
                case 0xC49F: c = 'g'; break;   /* ğ */
                case 0xC4B0: c = 'I'; break;   /* İ */
                case 0xC4B1: c = 'i'; break;   /* ı */
                case 0xC59E: c = 'S'; break;   /* Ş */
                case 0xC59F: c = 's'; break;   /* ş */
                case 0xC382: c = 'A'; break;   /* Â */
                case 0xC3A2: c = 'a'; break;   /* â */
                case 0xC38E: c = 'I'; break;   /* Î */
                case 0xC3AE: c = 'i'; break;   /* î */
                case 0xC39B: c = 'U'; break;   /* Û */
                case 0xC3BB: c = 'u'; break;   /* û */
                default: break;
            }
        }
        out[j++] = c;
        /* Çok baytlı diziyi tümüyle atla (devam baytları 10xxxxxx). */
        p++;
        while ((*p & 0xC0) == 0x80) p++;
    }
    out[j] = '\0';
}

/** Metin değiştiyse yaz, değişmediyse dokunma. */
static bool yaz(lv_obj_t *o, char *son, uint32_t n, const char *metin) {
    if (strncmp(son, metin, n - 1) == 0) return false;
    snprintf(son, n, "%s", metin);
    lv_label_set_text(o, son);
    return true;
}

static lv_obj_t *etiket(lv_obj_t *scr, const lv_font_t *font, uint32_t renk,
                        int32_t x, int32_t y) {
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(renk), LV_PART_MAIN);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    lv_label_set_text(l, "");
    return l;
}

void pb_sonuc_karti_olustur(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(RENK_ZEMIN), LV_PART_MAIN);

    s_durum = etiket(scr, &lv_font_montserrat_14, RENK_NORMAL, KENAR, 2);

    /* Güven sağ üstte: tür adı uzun olduğunda bile yeri sabit kalsın. */
    s_guven = lv_label_create(scr);
    lv_obj_set_style_text_font(s_guven, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_guven, lv_color_hex(RENK_TUR), LV_PART_MAIN);
    lv_obj_align(s_guven, LV_ALIGN_TOP_RIGHT, -KENAR, 2);
    lv_label_set_text(s_guven, "");

    /* Tür adı iki satıra sarabilir: en uzun ad 24 karakter
     * ("Uzun Kuyruklu Bastankara") ve Montserrat 20'de tek satıra sığmıyor. */
    s_ad = etiket(scr, &lv_font_montserrat_20, RENK_TUR, KENAR, 30);
    lv_obj_set_width(s_ad, GENISLIK);
    lv_label_set_long_mode(s_ad, LV_LABEL_LONG_MODE_WRAP);

    s_alt3 = etiket(scr, &lv_font_montserrat_14, RENK_SOLUK, KENAR, 80);
    lv_obj_set_width(s_alt3, GENISLIK);
    lv_label_set_long_mode(s_alt3, LV_LABEL_LONG_MODE_CLIP);

    s_sayac = etiket(scr, &lv_font_montserrat_14, RENK_SOLUK, KENAR, 118);
    lv_obj_set_width(s_sayac, GENISLIK);
    lv_label_set_long_mode(s_sayac, LV_LABEL_LONG_MODE_CLIP);

    s_son_durum[0] = s_son_guven[0] = s_son_ad[0] = '\0';
    s_son_alt3[0] = s_son_sayac[0] = '\0';
}

void pb_sonuc_karti_guncelle(const pb_sonuc_gorunum_t *g) {
    if (!g || !s_durum) return;

    const bool tur = (g->kip == PB_KARAR_TUR);
    const bool belirsiz = (g->kip == PB_KARAR_BELIRSIZ);

    /* ── Durum satırı ── */
    const char *durum_metni =
        tur ? "TANINDI" : belirsiz ? "olabilir..." :
        (g->kip == PB_KARAR_SES) ? "SES ALGILANDI" : "dinliyor...";
    const uint32_t durum_renk =
        tur ? RENK_TUR : belirsiz ? RENK_BELIRSIZ :
        (g->kip == PB_KARAR_SES) ? RENK_SES : RENK_NORMAL;
    if (yaz(s_durum, s_son_durum, sizeof(s_son_durum), durum_metni)) {
        lv_obj_set_style_text_color(s_durum, lv_color_hex(durum_renk),
                                    LV_PART_MAIN);
    }

    /* ── Güven ── */
    char tmp[96];
    if (g->tur_ad) snprintf(tmp, sizeof(tmp), "%%%d", (int)(g->guven * 100.0f + 0.5f));
    else           tmp[0] = '\0';
    if (yaz(s_guven, s_son_guven, sizeof(s_son_guven), tmp)) {
        lv_obj_set_style_text_color(s_guven,
            lv_color_hex(tur ? RENK_TUR : RENK_BELIRSIZ), LV_PART_MAIN);
    }

    /* ── Tür adı ── */
    if (g->tur_ad) pb_ascii_tr(g->tur_ad, tmp, sizeof(tmp));
    else           snprintf(tmp, sizeof(tmp), "-");
    if (yaz(s_ad, s_son_ad, sizeof(s_son_ad), tmp)) {
        lv_obj_set_style_text_color(s_ad,
            lv_color_hex(tur ? RENK_TUR : belirsiz ? RENK_BELIRSIZ : RENK_SOLUK),
            LV_PART_MAIN);
    }

    /* ── 2. ve 3. tahmin ──
     * ARCHITECTURE §4: "arayüz tek cevap değil ilk 3 tahmin gösterecek" —
     * top-1 %70,4 ama top-3 %82,2, yani ikinci ve üçüncü satır kullanıcıya
     * gerçekten bilgi veriyor. */
    char a2[40] = "", a3[40] = "";
    if (g->ilk3_ad[1]) pb_ascii_tr(g->ilk3_ad[1], a2, sizeof(a2));
    if (g->ilk3_ad[2]) pb_ascii_tr(g->ilk3_ad[2], a3, sizeof(a3));
    snprintf(tmp, sizeof(tmp), "2. %s %%%d\n3. %s %%%d",
             a2[0] ? a2 : "-", (int)(g->ilk3_olasilik[1] * 100.0f + 0.5f),
             a3[0] ? a3 : "-", (int)(g->ilk3_olasilik[2] * 100.0f + 0.5f));
    yaz(s_alt3, s_son_alt3, sizeof(s_son_alt3), tmp);

    /* ── Sayaçlar ── */
    snprintf(tmp, sizeof(tmp), "%lu kare/s  %ld dB\ncikarim %lu (%lu pencere)\noverrun %lu",
             (unsigned long)g->kare_hiz, (long)g->bant_db,
             (unsigned long)g->cikarim, (unsigned long)g->birlesen,
             (unsigned long)g->overrun);
    yaz(s_sayac, s_son_sayac, sizeof(s_son_sayac), tmp);
}
