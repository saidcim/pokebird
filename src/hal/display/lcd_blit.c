#include "lcd_blit.h"

#include "DEV_Config.h"
#include "LCD_3in49.h"
#include "qspi_pio.h"
#include "hardware/dma.h"

/* Bayt sırası çevrilmiş satır tamponu. Panelin bir satırı en fazla PB_PANEL_W
 * piksel; sütun yazarken de aynı tampon kullanılıyor (h=1). */
static uint16_t s_row[PB_PANEL_W];

/* ── Panelin yazma imleci ─────────────────────────────────────────────────
 *
 * Bu panel RASET'i (0x2B) YOK SAYIYOR — kartta ölçüldü, `z` komutu,
 * lastsession.md §9n. Satır konumunu yalnızca iki şey belirliyor:
 *   0x2C RAMWR   -> imleç sütun penceresinin EN ÜST satırına döner
 *   0x3C RAMWRC  -> imleç bir önceki yazmanın bittiği yerden DEVAM eder
 *
 * Dolayısıyla y>0 olan bir dikdörtgene yazmanın tek yolu ya imlecin zaten
 * orada olması (RAMWRC, bedava) ya da yukarısını atlama verisiyle geçmek
 * (RAMWR + atlama, ÜZERİNE YAZAR). İmleci burada takip ediyoruz ki ardışık
 * yazımlar — LVGL'in yukarıdan aşağı flush'ı, spektrogramın kayan sütunu —
 * atlama bedeli ödemesin.
 *
 * İmleç yalnızca bu dosyadan yapılan yazımları biliyor. Panele başka bir
 * yerden komut yollayan her kod (teşhis komutları) `pb_lcd_imlec_gecersiz()`
 * çağırmak zorunda. */
static bool     s_imlec_gecerli = false;
static uint32_t s_imlec_x1, s_imlec_x2, s_imlec_satir;

void pb_lcd_imlec_gecersiz(void) { s_imlec_gecerli = false; }

static void caset_ic(uint32_t x1, uint32_t x2) {
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x2A);           /* CASET — bu panelde çalışan tek pencere */
    QSPI_DATA_Write(qspi, (x1 >> 8) & 0xff);
    QSPI_DATA_Write(qspi, x1 & 0xff);
    QSPI_DATA_Write(qspi, (x2 >> 8) & 0xff);
    QSPI_DATA_Write(qspi, x2 & 0xff);
    QSPI_Deselect(qspi);
}

static void akis_basla_ic(uint8_t ramwr) {
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, ramwr);          /* 0x2C baştan, 0x3C devam */
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));
}

/* Dışarıya açık hâlleri imleci KENDİLİĞİNDEN geçersiz kılıyor. Teşhis
 * komutları paneli elle sürüyor; her çağrı yerinde geçersiz kılmayı
 * hatırlamak zorunda kalmak sessiz hataya davetiyeydi (bir sonraki blit
 * imlecin yanlış yerde olduğunu bilmeden RAMWRC ile devam ederdi). */
void pb_lcd_sutun_penceresi(uint32_t x1, uint32_t x2) {
    caset_ic(x1, x2);
    s_imlec_gecerli = false;
}

void pb_lcd_akis_basla(uint8_t ramwr) {
    akis_basla_ic(ramwr);
    s_imlec_gecerli = false;
}

/** s_row'daki n pikseli (zaten bayt sırası çevrilmiş) panele DMA ile yaz. */
static void satiri_gonder(uint32_t n) {
    dma_channel_configure(dma_tx, &c,
                          &qspi.pio->txf[qspi.sm],
                          s_row,
                          n * 2,              /* bayt sayısı (8-bit aktarım) */
                          true);
    while (dma_channel_is_busy(dma_tx)) tight_loop_contents();
}

void pb_lcd_akis_renk(uint16_t renk, uint32_t piksel) {
    const uint16_t be = (uint16_t)((renk >> 8) | (renk << 8));
    for (uint32_t i = 0; i < PB_PANEL_W; i++) s_row[i] = be;

    while (piksel) {
        uint32_t n = (piksel > PB_PANEL_W) ? PB_PANEL_W : piksel;
        satiri_gonder(n);
        piksel -= n;
    }
}

void pb_lcd_akis_satir(const uint16_t *src, uint32_t n) {
    if (!src || n == 0 || n > PB_PANEL_W) return;
    for (uint32_t i = 0; i < n; i++) {
        /* Panel big-endian RGB565 istiyor */
        s_row[i] = (uint16_t)((src[i] >> 8) | (src[i] << 8));
    }
    satiri_gonder(n);
}

void pb_lcd_akis_bitir(void) {
    QSPI_Deselect(qspi);
}

/**
 * İmleci (x, y) satırına getir ve akışı başlat (CS aşağıda döner).
 *
 * İmleç zaten oradaysa RAMWRC ile bedava devam eder. Değilse RAMWR'den
 * başlayıp aradaki y satırı `atlama_renk` ile geçer — **bu, o sütun
 * aralığında y satırın ÜZERİNE YAZAR.** Bilinçli bir bedel: panel başka
 * türlü konumlandırmayı desteklemiyor.
 */
static void imleci_konumla(uint32_t x, uint32_t y, uint32_t w, uint16_t atlama_renk) {
    if (s_imlec_gecerli && s_imlec_x1 == x && s_imlec_x2 == x + w - 1 &&
        s_imlec_satir == y) {
        akis_basla_ic(0x3C);                     /* RAMWRC — atlama yok */
        return;
    }
    caset_ic(x, x + w - 1);
    akis_basla_ic(0x2C);                         /* RAMWR — satır 0 */
    if (y) pb_lcd_akis_renk(atlama_renk, y * w);
}

static void imleci_isaretle(uint32_t x, uint32_t w, uint32_t satir) {
    s_imlec_gecerli = true;
    s_imlec_x1 = x;
    s_imlec_x2 = x + w - 1;
    s_imlec_satir = satir;
}

void pb_lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 const uint16_t *buf) {
    if (!buf || w == 0 || h == 0) return;
    if (x >= PB_PANEL_W || y >= PB_PANEL_H) return;
    if (x + w > PB_PANEL_W) w = PB_PANEL_W - x;
    if (y + h > PB_PANEL_H) h = PB_PANEL_H - y;

    imleci_konumla(x, y, w, 0x0000);

    for (uint32_t row = 0; row < h; row++) {
        pb_lcd_akis_satir(buf + (size_t)row * w, w);
    }

    pb_lcd_akis_bitir();
    imleci_isaretle(x, w, y + h);
}

void pb_lcd_blit_strided(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         const uint16_t *buf, int32_t col_step, int32_t row_step) {
    if (!buf || w == 0 || h == 0) return;
    if (x >= PB_PANEL_W || y >= PB_PANEL_H) return;
    /* Kırpma yapmıyoruz: adımlar negatif olabildiği için kırpılmış bir
     * dikdörtgenin kaynak başlangıcı da kaymalı ve bunu çağıran taraf
     * bilmeden yapmak sessiz hataya davetiye. Sınır dışı istek reddedilir. */
    if (x + w > PB_PANEL_W || y + h > PB_PANEL_H) return;

    imleci_konumla(x, y, w, 0x0000);

    for (uint32_t row = 0; row < h; row++) {
        const uint16_t *src = buf + (int32_t)row * row_step;
        for (uint32_t i = 0; i < w; i++) {
            uint16_t px = src[(int32_t)i * col_step];
            s_row[i] = (uint16_t)((px >> 8) | (px << 8));
        }
        satiri_gonder(w);
    }

    pb_lcd_akis_bitir();
    imleci_isaretle(x, w, y + h);
}

void pb_lcd_fill(uint16_t color) {
    /* Tek geçiş: sütun penceresi tam genişlik, RAMWR, bütün ekran.
     * Eski hâli 640 ayrı pencere+RAMWR yapıyordu ve her satır aynı ÜST
     * satıra biniyordu (§9n'in baş belirtisi: "ekran temizlenmiyor"). */
    caset_ic(0, PB_PANEL_W - 1);
    akis_basla_ic(0x2C);
    pb_lcd_akis_renk(color, (uint32_t)PB_PANEL_W * PB_PANEL_H);
    pb_lcd_akis_bitir();
    imleci_isaretle(0, PB_PANEL_W, PB_PANEL_H);
}

void pb_lcd_duz_akit(uint16_t renk, uint32_t piksel) {
    pb_lcd_akis_basla(0x2c);
    pb_lcd_akis_renk(renk, piksel);
    pb_lcd_akis_bitir();
    pb_lcd_imlec_gecersiz();
}
