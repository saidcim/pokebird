#include "ui/ekran_gunluk.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ui/metin.h"
#include "ui/tema.h"

/* ── Yerleşim (tam 640) ───────────────────────────────────────────────────
 *     y   8   başlık "BUGÜN · TODAY"          sağda kayıt sayısı
 *     y  30   ayraç
 *     y  38   1. kayıt  \
 *     y  76   2. kayıt   >  satır yüksekliği 38 (ad + bilimsel ad alt alta)
 *     y 114   3. kayıt  /
 *     y 150   sayaçlar (göz gerektirmeyen doğrulama)
 *     y 162   sayfa noktaları
 */
#define SATIR_Y0    38
#define SATIR_ADIM  38
#define GOSTERILEN  3

#define SOL         20
#define SAG         620
#define SURE_X      SOL
#define SURE_W      86
#define AD_X        (SOL + 96)
#define AD_W        300
#define CUBUK_X     440
#define CUBUK_W     120
#define YUZDE_X     572
#define YUZDE_W     48

/* ── Kayıt halkası ────────────────────────────────────────────────────────
 * SD kart günlüğü gelene kadar (M7 adım 5) tespitler RAM'de duruyor. Sekiz
 * yeter: ekranda üçü görünüyor, kalanı "kaydırınca daha fazlası" için değil,
 * yalnızca en yenisinin doğru seçilebilmesi için. Maliyet ~800 bayt. */
#define HALKA 8

typedef struct {
    char     ad[48];
    char     latin[40];
    float    guven;
    uint32_t ms;            /* açılıştan bu yana */
    bool     dolu;
} kayit_t;

static kayit_t s_kayit[HALKA];
static uint32_t s_bas;          /* en yeni kaydın indeksi + 1 (mod HALKA) */
static uint32_t s_toplam;

typedef struct {
    lv_obj_t *sure, *ad, *latin, *cubuk, *yuzde;
    char son_sure[24], son_ad[64], son_latin[48], son_yuzde[12];
    int32_t son_cubuk_w;
} satir_t;

static lv_obj_t *s_ekran, *s_sayi, *s_bos, *s_sayac;
static satir_t   s_satir[GOSTERILEN];
static char      s_son_sayi[24], s_son_sayac[64];

static void satir_kur(satir_t *s, int32_t y)
{
    s->sure = pb_etiket(s_ekran, &pb_font_mono_10, PB_RENK_SILIK, SURE_X, y + 4);
    lv_obj_set_width(s->sure, SURE_W);

    s->ad = pb_etiket(s_ekran, &pb_font_kalin_13, PB_RENK_METIN, AD_X, y);
    lv_obj_set_width(s->ad, AD_W);
    lv_label_set_long_mode(s->ad, LV_LABEL_LONG_MODE_DOTS);

    s->latin = pb_etiket(s_ekran, &pb_font_mono_10, PB_RENK_LATIN, AD_X + 1, y + 15);
    lv_obj_set_width(s->latin, AD_W);
    lv_label_set_long_mode(s->latin, LV_LABEL_LONG_MODE_DOTS);

    pb_kutu(s_ekran, CUBUK_X, y + 8, CUBUK_W, 5, PB_RENK_SATIR, 3);
    s->cubuk = pb_kutu(s_ekran, CUBUK_X, y + 8, 0, 5, PB_RENK_VURGU, 3);

    s->yuzde = pb_etiket(s_ekran, &pb_font_kalin_13, PB_RENK_VURGU, YUZDE_X, y + 1);
    lv_obj_set_width(s->yuzde, YUZDE_W);
    lv_obj_set_style_text_align(s->yuzde, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    s->son_sure[0] = s->son_ad[0] = s->son_latin[0] = s->son_yuzde[0] = '\0';
    s->son_cubuk_w = -1;
}

lv_obj_t *pb_ekran_gunluk_olustur(void)
{
    s_ekran = pb_ekran_yeni();

    lv_obj_t *baslik = pb_etiket(s_ekran, &pb_font_kalin_13, PB_RENK_METIN, SOL, 8);
    lv_obj_set_style_text_letter_space(baslik, 3, LV_PART_MAIN);
    lv_label_set_text(baslik, "BUGÜN · TODAY");

    s_sayi = pb_etiket(s_ekran, &pb_font_dar_11, PB_RENK_SILIK,
                       SAG - 120, 10);
    lv_obj_set_width(s_sayi, 120);
    lv_obj_set_style_text_align(s_sayi, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    pb_kutu(s_ekran, SOL, 30, SAG - SOL, 1, PB_RENK_CIZGI, 0);

    for (int i = 0; i < GOSTERILEN; i++) {
        satir_kur(&s_satir[i], SATIR_Y0 + i * SATIR_ADIM);
    }

    /* Boş durum — cihaz yeni açıldığında ekranın anlamsız görünmemesi için. */
    s_bos = pb_etiket(s_ekran, &pb_font_dar_11, PB_RENK_SILIK, SOL, 84);
    lv_obj_set_width(s_bos, SAG - SOL);
    lv_obj_set_style_text_align(s_bos, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_bos, "Henüz kayıt yok — dinleniyor");

    /* Sayaçlar tasarımda YOK; buraya konuldu çünkü §9p'de bilerek ekrana
     * yazılmışlardı (göz gerektirmeyen doğrulama). Dinleme ekranını
     * kalabalıklaştırmamak için ikinci ekranın altına alındılar. */
    s_sayac = pb_etiket(s_ekran, &pb_font_mono_10, PB_RENK_KENAR, SOL, 150);
    lv_obj_set_width(s_sayac, 400);

    pb_sayfa_noktalari(s_ekran, 1);

    s_son_sayi[0] = s_son_sayac[0] = '\0';
    return s_ekran;
}

void pb_ekran_gunluk_ekle(const char *ad, const char *latin, float guven)
{
    if (!ad || !ad[0]) return;

    const uint32_t simdi = to_ms_since_boot(get_absolute_time());

    /* Aynı tür üst üste gelirse yeni satır AÇMA, en üsttekini tazele: karar
     * kuralı bir türü PB_KARAR_TUT_MS boyunca ekranda tutuyor ve o süre
     * boyunca aynı tespit tekrar tekrar düşerse günlük tek olayla dolardı. */
    if (s_toplam > 0) {
        kayit_t *ust = &s_kayit[(s_bas + HALKA - 1) % HALKA];
        if (strncmp(ust->ad, ad, sizeof(ust->ad) - 1) == 0) {
            if (guven > ust->guven) ust->guven = guven;
            ust->ms = simdi;
            return;
        }
    }

    kayit_t *k = &s_kayit[s_bas];
    snprintf(k->ad, sizeof(k->ad), "%s", ad);
    snprintf(k->latin, sizeof(k->latin), "%s", latin ? latin : "");
    k->guven = guven;
    k->ms = simdi;
    k->dolu = true;

    s_bas = (s_bas + 1) % HALKA;
    if (s_toplam < 0xFFFFFFFFu) s_toplam++;
}

/** "az önce" / "3 dk önce" / "2 sa önce" — RTC yok, açılıştan bu yana. */
static void sure_yaz(uint32_t gecen_ms, char *out, uint32_t n)
{
    const uint32_t sn = gecen_ms / 1000u;
    if (sn < 60u)        snprintf(out, n, "az once");
    else if (sn < 3600u) snprintf(out, n, "%lu dk once", (unsigned long)(sn / 60u));
    else                 snprintf(out, n, "%lu sa once", (unsigned long)(sn / 3600u));
}

void pb_ekran_gunluk_tazele(uint32_t kare_hiz, uint32_t cikarim, uint32_t overrun)
{
    if (!s_ekran) return;

    const uint32_t simdi = to_ms_since_boot(get_absolute_time());

    char buf[80];
    snprintf(buf, sizeof(buf), "%lu kayıt", (unsigned long)s_toplam);
    pb_yaz(s_sayi, s_son_sayi, sizeof(s_son_sayi), buf);

    if (s_toplam == 0) {
        lv_obj_clear_flag(s_bos, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_bos, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < GOSTERILEN; i++) {
        satir_t *s = &s_satir[i];
        /* i=0 en yeni: halkada bir geri. */
        const kayit_t *k = &s_kayit[(s_bas + HALKA - 1 - (uint32_t)i) % HALKA];

        if (!k->dolu || (uint32_t)i >= s_toplam) {
            pb_yaz(s->sure,  s->son_sure,  sizeof(s->son_sure),  "");
            pb_yaz(s->ad,    s->son_ad,    sizeof(s->son_ad),    "");
            pb_yaz(s->latin, s->son_latin, sizeof(s->son_latin), "");
            pb_yaz(s->yuzde, s->son_yuzde, sizeof(s->son_yuzde), "");
            if (s->son_cubuk_w != 0) { s->son_cubuk_w = 0; lv_obj_set_width(s->cubuk, 0); }
            continue;
        }

        sure_yaz(simdi - k->ms, buf, sizeof(buf));
        pb_yaz(s->sure, s->son_sure, sizeof(s->son_sure), buf);

        pb_turkce_buyut(k->ad, buf, sizeof(buf));
        pb_yaz(s->ad, s->son_ad, sizeof(s->son_ad), buf);

        pb_yaz(s->latin, s->son_latin, sizeof(s->son_latin), k->latin);

        snprintf(buf, sizeof(buf), "%d%%", (int)(k->guven * 100.0f + 0.5f));
        pb_yaz(s->yuzde, s->son_yuzde, sizeof(s->son_yuzde), buf);

        int32_t w = (int32_t)(k->guven * (float)CUBUK_W + 0.5f);
        if (w > CUBUK_W) w = CUBUK_W;
        if (w != s->son_cubuk_w) { s->son_cubuk_w = w; lv_obj_set_width(s->cubuk, w); }
    }

    snprintf(buf, sizeof(buf), "%lu kare/s · cikarim %lu · overrun %lu",
             (unsigned long)kare_hiz, (unsigned long)cikarim,
             (unsigned long)overrun);
    pb_yaz(s_sayac, s_son_sayac, sizeof(s_son_sayac), buf);
}
