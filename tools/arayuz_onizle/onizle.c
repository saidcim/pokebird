/**
 * onizle.c — Arayüzü HOST'ta gerçek LVGL ile çizip PNG'ye döker
 *
 * NEDEN VAR: arayüzün nasıl göründüğünü anlamanın tek yolu karta yükleyip
 * kullanıcıdan bakmasını istemekti. Her tur pahalı ve yavaş; tasarım
 * iterasyonu böyle yapılamıyor.
 *
 * Burada CİHAZDAKİ ekran kodunun TA KENDİSİ derleniyor (ekran_dinleme.c,
 * ekran_gunluk.c, tema.c, metin.c ve üretilmiş yazı tipleri) — taklit değil.
 * Fark yalnızca alt katmanda: panel yerine bellekteki bir framebuffer var,
 * Pico SDK yerine küçük bir saat sahtesi (shim/pico/stdlib.h).
 *
 * Dolayısıyla bu önizleme YERLEŞİMİ, YAZI TİPİNİ, RENKLERİ ve METİN
 * SARMASINI birebir gösterir. GÖSTERMEDİĞİ şey panele basma yolu: dilim
 * sınırları, kayma, QSPI zamanlaması. Onlar hâlâ kartta doğrulanmalı.
 *
 *   cmake -S tools/arayuz_onizle -B tools/arayuz_onizle/build -G Ninja
 *   cmake --build tools/arayuz_onizle/build
 *   ./tools/arayuz_onizle/build/arayuz_onizle
 *   python tools/arayuz_onizle/ppm_png.py
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "hal/display/lcd_blit.h"
#include "ui/ekran_dinleme.h"
#include "ui/ekran_gunluk.h"
#include "ui/spectrogram.h"
#include "ui/tema.h"

uint32_t pb_onizle_ms = 0;

#define W PB_EKRAN_W
#define H PB_EKRAN_H

static uint16_t s_fb[W * H];
static lv_display_t *s_disp;

/* Cihazdaki gibi PARTIAL kip: LVGL şeritler hâlinde çiziyor, flush_cb onları
 * framebuffer'a işliyor. DIRECT kip yerine bu seçildi çünkü cihazın çizim
 * yolu da bu — aynı kod yolundan geçmek önizlemenin değerini artırıyor. */
#define SERIT_H 43
static uint16_t s_draw[W * SERIT_H];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
    const uint16_t *src = (const uint16_t *)(void *)px;

    int32_t adim = a->x2 - a->x1 + 1;
    lv_draw_buf_t *db = lv_display_get_buf_active(d);
    if (db && db->header.stride) adim = (int32_t)(db->header.stride / 2);

    for (int32_t y = a->y1; y <= a->y2; y++) {
        for (int32_t x = a->x1; x <= a->x2; x++) {
            if (x < 0 || x >= W || y < 0 || y >= H) continue;
            s_fb[y * W + x] = src[(size_t)(y - a->y1) * adim + (x - a->x1)];
        }
    }
    lv_display_flush_ready(d);
}

static uint32_t tick_cb(void) { return pb_onizle_ms; }

/* ── Sahte panel — spektrogram GERÇEK koduyla çiziliyor ───────────────────
 *
 * Spektrogram LVGL'den geçmiyor, panele DOĞRUDAN yazıyor (kendi hızlı sütun
 * yolu). Önizlemede sağ taraf bu yüzden boş kalıyordu. `pb_lcd_blit`in
 * karşılığını buraya koyunca cihazın `spectrogram.c`'si olduğu gibi
 * derlenip aynı framebuffer'a çiziyor — renk eşlemesi ve imleç sütunu dâhil.
 *
 * Cihazdaki yön çevriminin AYNISI (bkz. ui/spectrogram.c başlığı):
 *     panel satırı r  ->  ui_x = r
 *     panel sütunu c  ->  ui_y = 171 - c
 */
void pb_lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 const uint16_t *buf) {
    for (uint32_t r = 0; r < h; r++) {
        const uint32_t ux = y + r;
        if (ux >= (uint32_t)W) continue;
        for (uint32_t c = 0; c < w; c++) {
            const uint32_t nc = x + c;
            if (nc >= (uint32_t)H) continue;
            s_fb[(H - 1 - nc) * W + ux] = buf[r * w + c];
        }
    }
}

/** Gerçekçi bir ötüş deseni — heceler, süpüren temel frekans, iki harmonik
 *  ve gürültü tabanı. `C` demosundaki desenin aynısı: düz renk OLMAMASI şart
 *  (§9o), yoksa hem kayma gizlenir hem de gerçek çıktı hakkında fikir vermez. */
static void spektrogram_doldur(void) {
    pb_spec_init();
    for (uint32_t sutun = 0; sutun < PB_SPEC_WIDTH; sutun++) {
        uint8_t bins[64];
        const uint32_t faz = sutun % 48;
        const bool sessiz = (faz >= 34);
        const int temel = 13 + (int)(faz < 17 ? faz : 34 - faz);
        for (int b = 0; b < 64; b++) {
            int v = 10 + (int)((sutun * 7u + (uint32_t)b * 13u) % 9u);
            if (!sessiz) {
                for (int hrm = 1; hrm <= 3; hrm++) {
                    const int merkez = temel * hrm;
                    const int d = b - merkez;
                    if (merkez < 64 && d > -4 && d < 4) {
                        const int g = (d < 0 ? -d : d);
                        v += (240 / hrm) - g * 40;
                    }
                }
            }
            bins[b] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        pb_spec_push_column(bins, 64);
    }
}

/** Framebuffer'ı PPM (P6, 8 bit RGB) olarak yaz — ppm_png.py PNG'ye çeviriyor. */
static void dok(const char *ad) {
    FILE *f = fopen(ad, "wb");
    if (!f) { printf("acilamadi: %s\n", ad); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        const uint16_t p = s_fb[i];
        /* RGB565 -> RGB888, üst bitleri alta kopyalayarak (tam beyaz beyaz
         * kalsın, 0xF8 gibi yaklaşık bir değer değil). */
        const uint8_t r = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
        const uint8_t g = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
        const uint8_t b = (uint8_t)((p & 0x1F) * 255 / 31);
        fputc(r, f); fputc(g, f); fputc(b, f);
    }
    fclose(f);
    printf("  yazildi: %s\n", ad);
}

/** LVGL yığın kullanımı — cihazla AYNI lv_conf (LV_MEM_SIZE) geçerli, yani
 *  burada dolan havuz kartta da dolar. */
static void bellek(const char *nerede) {
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    printf("  [LVGL yigin] %-22s kullanilan %6u / %6u bayt  (%%%u dolu, "
           "en buyuk bos blok %u)\n",
           nerede, (unsigned)(m.total_size - m.free_size), (unsigned)m.total_size,
           (unsigned)m.used_pct, (unsigned)m.free_biggest_size);
}

/* LVGL ONCE, spektrogram SONRA: onizlemede dilim sahipligi taklit
 * edilmiyor, yani LVGL 640'in tamamini boyuyor ve sirasi ters olursa
 * seridi siler. Cihazda bu sorun yok (lv_port.c dilim maskesi). */
static void ciz(lv_obj_t *scr, const char *ad, bool spektro) {
    lv_screen_load(scr);
    lv_obj_invalidate(scr);
    lv_refr_now(s_disp);
    if (spektro) spektrogram_doldur();
    dok(ad);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* çökerse çıktı kaybolmasın */
    printf("Arayuz onizlemesi (%dx%d)\n", W, H);

    lv_init();
    lv_tick_set_cb(tick_cb);

    s_disp = lv_display_create(W, H);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, s_draw, NULL, sizeof(s_draw),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    printf("  ekran kuruldu\n");

    /* ── EKRAN 0 · DİNLEME ────────────────────────────────────────────────
     * Gerçekçi ve ZOR bir örnek: uzun bir tür adı, üç aday, karar TANINDI. */
    lv_obj_t *dinleme = pb_ekran_dinleme_olustur();
    bellek("ekran0 kuruldu");

    pb_sonuc_gorunum_t g;
    memset(&g, 0, sizeof(g));
    g.kip = PB_KARAR_TUR;
    g.tur_ad = "Bülbül";
    g.guven = 0.94f;
    g.ilk3_ad[0] = "Bülbül";
    g.ilk3_ad[1] = "Karatavuk";
    g.ilk3_ad[2] = "Kızılgerdan";
    g.ilk3_latin[0] = "Luscinia megarhynchos";
    g.ilk3_latin[1] = "Turdus merula";
    g.ilk3_latin[2] = "Erithacus rubecula";
    g.ilk3_olasilik[0] = 0.94f;
    g.ilk3_olasilik[1] = 0.61f;
    g.ilk3_olasilik[2] = 0.38f;
    g.kare_hiz = 63;
    g.cikarim = 12;
    g.overrun = 0;
    pb_ekran_dinleme_kayit_ayarla(true);
    pb_ekran_dinleme_guncelle(&g);
    ciz(dinleme, "ekran0_tanindi.ppm", true);

    /* Dinleme kipi — hiçbir tür yokken ekran ne gösteriyor. */
    pb_sonuc_gorunum_t b;
    memset(&b, 0, sizeof(b));
    b.kip = PB_KARAR_DINLIYOR;
    b.kare_hiz = 63;
    pb_ekran_dinleme_kayit_ayarla(true);
    pb_ekran_dinleme_guncelle(&b);
    ciz(dinleme, "ekran0_dinliyor.ppm", true);

    /* BOSTA — cihaz acilista dinlemiyor, kullanici butona basacak. */
    pb_ekran_dinleme_kayit_ayarla(false);
    pb_ekran_dinleme_guncelle(&b);
    ciz(dinleme, "ekran0_bosta.ppm", true);

    /* En UZUN tür adı — sarma/kesme en kötü durumu. */
    pb_sonuc_gorunum_t u;
    memset(&u, 0, sizeof(u));
    u.kip = PB_KARAR_BELIRSIZ;
    u.tur_ad = "Uzun Kuyruklu Baştankara";
    u.guven = 0.42f;
    u.ilk3_ad[0] = "Uzun Kuyruklu Baştankara";
    u.ilk3_ad[1] = "Ak Karınlı Ebabil";
    u.ilk3_ad[2] = "İskender Papağanı";
    u.ilk3_latin[0] = "Aegithalos caudatus";
    u.ilk3_latin[1] = "Apus melba";
    u.ilk3_latin[2] = "Psittacula eupatria";
    u.ilk3_olasilik[0] = 0.42f;
    u.ilk3_olasilik[1] = 0.29f;
    u.ilk3_olasilik[2] = 0.11f;
    pb_ekran_dinleme_kayit_ayarla(true);
    pb_ekran_dinleme_guncelle(&u);
    ciz(dinleme, "ekran0_uzun_ad.ppm", true);

    /* ── EKRAN 1 · GÜNLÜK ─────────────────────────────────────────────── */
    lv_obj_t *gunluk = pb_ekran_gunluk_olustur();
    bellek("ekran1 kuruldu");

    pb_onizle_ms = 0;
    ciz(gunluk, "ekran1_bos.ppm", false);

    /* Saati elle ilerleterek üç farklı yaş üret. */
    pb_onizle_ms = 10u * 1000u;
    pb_ekran_gunluk_ekle("İskender Papağanı", "Psittacula eupatria", 0.78f);
    pb_onizle_ms = 40u * 60u * 1000u;
    pb_ekran_gunluk_ekle("Martı", "Larus michahellis", 0.87f);
    pb_onizle_ms = 95u * 60u * 1000u;
    pb_ekran_gunluk_ekle("Bülbül", "Luscinia megarhynchos", 0.94f);

    pb_onizle_ms = 96u * 60u * 1000u;
    pb_ekran_gunluk_tazele(63, 12, 0);
    ciz(gunluk, "ekran1_dolu.ppm", false);

    printf("bitti\n");
    return 0;
}
