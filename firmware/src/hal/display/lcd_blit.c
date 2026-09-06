#include "lcd_blit.h"

#include <stdio.h>

#include "DEV_Config.h"
#include "LCD_3in49.h"
#include "qspi_pio.h"
#include "hardware/dma.h"

/* Bayt sırası çevrilmiş satır tamponu. Panelin bir satırı en fazla PB_PANEL_W
 * piksel; sütun yazarken de aynı tampon kullanılıyor (h=1). */
static uint16_t s_row[PB_PANEL_W];

/* ── Panelin yazma imleci ─────────────────────────────────────────────────
 *
 * Bu panelde RASET (0x2B) YOK SAYILIYOR — kartta ölçüldü (`z`), §9n.
 * Satır konumunu yalnızca iki komut belirliyor:
 *   0x2C RAMWR   -> imleç sütun penceresinin EN ÜST satırına döner
 *   0x3C RAMWRC  -> imleç bir önceki yazmanın bittiği yerden DEVAM eder
 * Sütun aralığı CASET (0x2A) ile ayarlanıyor ve o çalışıyor.
 *
 * Satır, pencere genişliği kadar piksel yazıldıkça ilerliyor. Pencere
 * DARALTILIRSA satır daha ucuza ilerletilebiliyor ve pencere yeniden
 * genişletilip RAMWRC ile devam edildiğinde satır KORUNUYOR (`j` ile
 * ölçüldü). Konumlandırma bu yüzden ucuz: y satır ilerletmek 2*y piksel.
 *
 * ⚠ 2 PİKSEL HİZALAMA: panel sütun aralığını 2 piksele yuvarlıyor. `j`
 * ölçtü — 1 piksellik pencere (66..66) fiilen 66..67 oluyor, 300 piksel
 * 300 değil 150 satır ilerletiyor ve sonraki yazma bir piksel kaymış hizadan
 * devam ederek dişli/noktalı çıkıyor. Bu yüzden HER pencere x1 çift, x2 tek
 * olacak şekilde genişletiliyor. */
static bool     s_cursor_valid = false;
static uint32_t s_cursor_x1, s_cursor_x2, s_cursor_row;

/* ── Atlama şeridi ────────────────────────────────────────────────────────
 *
 * Konumlandırma panelin 0. ve 1. sütununu kullanıyor: oraya y satır kadar
 * veri yazılıyor ki imleç y'ye gelsin. Ham hâliyle bu, o iki sütunu
 * SİLERDİ. Onun yerine iki sütunun GERÇEK içeriğini burada tutuyoruz ve
 * atlarken aynısını geri yazıyoruz — atlama böylece tamamen GÖRÜNMEZ oluyor
 * ve ekrandan tek piksel bile feda edilmiyor.
 *
 * Bedeli 640*2*2 = 2.560 bayt. Alternatifi iki sütunu arayüzden düşürmekti
 * (172 -> 170), o da ölçülmüş yön eşlemesini ve spektrogram bantlarını
 * baştan kurmayı gerektirirdi.
 *
 * Panele bu dosyanın dışından yazan her kod şeridi geçersiz kılıyor; şerit
 * geçersizken atlama siyah yazar (yalnızca teşhis komutlarından sonra olur,
 * uygulama zaten ardından yeniden çiziyor). */
static uint16_t s_strip[PB_PANEL_H][2];      /* big-endian, panele gittiği hâliyle */
static bool     s_strip_valid = false;

void pb_lcd_cursor_invalidate(void) {
    s_cursor_valid = false;
    s_strip_valid = false;
}

static void caset_inner(uint32_t x1, uint32_t x2) {
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x2A);           /* CASET — bu panelde çalışan tek pencere */
    QSPI_DATA_Write(qspi, (x1 >> 8) & 0xff);
    QSPI_DATA_Write(qspi, x1 & 0xff);
    QSPI_DATA_Write(qspi, (x2 >> 8) & 0xff);
    QSPI_DATA_Write(qspi, x2 & 0xff);
    QSPI_Deselect(qspi);
}

static void stream_begin_inner(uint8_t ramwr) {
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, ramwr);          /* 0x2C baştan, 0x3C devam */
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));
}

/* Dışarıya açık hâlleri imleci ve şeridi KENDİLİĞİNDEN geçersiz kılıyor.
 * Teşhis komutları paneli elle sürüyor; her çağrı yerinde geçersiz kılmayı
 * hatırlamak zorunda kalmak sessiz hataya davetiyeydi (bir sonraki blit
 * imlecin yanlış yerde olduğunu bilmeden RAMWRC ile devam ederdi). */
void pb_lcd_column_window(uint32_t x1, uint32_t x2) {
    caset_inner(x1, x2);
    pb_lcd_cursor_invalidate();
}

void pb_lcd_stream_begin(uint8_t ramwr) {
    stream_begin_inner(ramwr);
    pb_lcd_cursor_invalidate();
}

/* Son yazılım aşamasının dökümü: DMA'ya giden `s_row`'un kendisi. Buraya
 * kadar her şey ölçüldü (kaynak veri, devrik okuma, satır adımı, hizalama,
 * CS zamanlaması); geriye doğrulanmamış tek aşama buydu. */
static int s_row_dump = 0;
void pb_lcd_request_row_dump(int count) { s_row_dump = count; }

/* ── Aktarım yığını — rsvpnano'nun tekniği ────────────────────────────────
 *
 * Çalışan referans sürücü (rsvpnano/src/drivers/display/axs15231b_pio/
 * axs15231b_pio.cpp, `pushColors`) dikdörtgeni satır satır DEĞİL, bitişik bir
 * tamponda toplayıp **tek DMA** ile yolluyor:
 *
 *     const size_t byteCount = width * height * sizeof(uint16_t);
 *     dma_channel_configure(..., data, byteCount, true);
 *
 * Bizim eski yolumuz her satır için ayrı DMA açıyor ve aralarda CPU bayt
 * sırasını çevirirken hat CS aşağıdayken boşta kalıyordu. Kartta ölçüldü
 * (`S` komutu): dar pencerede satır satır yazınca içerik her satırda kayıyor
 * (merdiven), tam genişlikte kaymıyor. Bayt sırası, pencere komutu ve CS
 * zamanlaması referansla zaten aynıydı; ayrıldığımız tek yer buydu.
 *
 * Yığın 4096 piksel = 8 KB. LVGL'in en geniş flush'ı 172 sütun; 4096/172 = 23
 * satır, tipik dar bant (32 sütun) için 128 satır — yani neredeyse her
 * dikdörtgen TEK DMA'ya sığıyor. */
#define PB_STACK_PIXELS 4096
static uint16_t s_heap[PB_STACK_PIXELS];

static void yigini_gonder(uint32_t pixel) {
    dma_channel_configure(dma_tx, &c,
                          &qspi.pio->txf[qspi.sm],
                          s_heap,
                          pixel * 2,
                          true);
    while (dma_channel_is_busy(dma_tx)) tight_loop_contents();
}

/** s_row'daki n pikseli (zaten bayt sırası çevrilmiş) panele DMA ile yaz. */
static void satiri_gonder(uint32_t n) {
    if (s_row_dump > 0) {
        s_row_dump--;
        printf("#SATIR %lu ", (unsigned long)n);
        for (uint32_t i = 0; i < n; i++) {
            /* s_row big-endian; parlaklık için geri çevir */
            uint16_t px = (uint16_t)((s_row[i] >> 8) | (s_row[i] << 8));
            uint32_t l = ((px >> 11) & 0x1F) + ((px >> 6) & 0x1F) + (px & 0x1F);
            putchar(l < 6 ? '.' : (l < 24 ? '+' : '#'));
        }
        putchar('\n');
    }
    dma_channel_configure(dma_tx, &c,
                          &qspi.pio->txf[qspi.sm],
                          s_row,
                          n * 2,              /* bayt sayısı (8-bit aktarım) */
                          true);
    while (dma_channel_is_busy(dma_tx)) tight_loop_contents();
}

void pb_lcd_stream_color(uint16_t color, uint32_t pixel) {
    const uint16_t be = (uint16_t)((color >> 8) | (color << 8));
    for (uint32_t i = 0; i < PB_PANEL_W; i++) s_row[i] = be;

    while (pixel) {
        uint32_t n = (pixel > PB_PANEL_W) ? PB_PANEL_W : pixel;
        satiri_gonder(n);
        pixel -= n;
    }
}

void pb_lcd_stream_row(const uint16_t *src, uint32_t n) {
    if (!src || n == 0 || n > PB_PANEL_W) return;
    for (uint32_t i = 0; i < n; i++) {
        /* Panel big-endian RGB565 istiyor */
        s_row[i] = (uint16_t)((src[i] >> 8) | (src[i] << 8));
    }
    satiri_gonder(n);
}

void pb_lcd_stream_end(void) {
    QSPI_Deselect(qspi);
}

/**
 * İmleci `y` satırına getir — 0. ve 1. sütunu kullanarak, GÖRÜNMEZ biçimde.
 *
 * Pencere 2 piksel olduğu için y satır ilerletmek 2*y piksele mal oluyor
 * (tam genişlikte 172*y olurdu). Yazılan veri şeridin gerçek içeriği,
 * dolayısıyla ekranda hiçbir şey değişmiyor.
 */
static void strip_ile_atla(uint32_t y) {
    caset_inner(0, 1);
    stream_begin_inner(0x2C);                        /* satır 0 */

    const uint32_t row_start = PB_PANEL_W / 2; /* s_row'a sığan satır sayısı */
    uint32_t written = 0;
    while (written < y) {
        uint32_t n = y - written;
        if (n > row_start) n = row_start;
        for (uint32_t r = 0; r < n; r++) {
            s_row[2 * r]     = s_strip_valid ? s_strip[written + r][0] : 0;
            s_row[2 * r + 1] = s_strip_valid ? s_strip[written + r][1] : 0;
        }
        satiri_gonder(n * 2);
        written += n;
    }
    pb_lcd_stream_end();
}

/**
 * İmleci (hizalanmış pencere x1..x2, satır y) konumuna getir ve akışı
 * başlat (CS aşağıda döner).
 */
static void imleci_konumla(uint32_t x1, uint32_t x2, uint32_t y) {
    if (s_cursor_valid && s_cursor_x1 == x1 && s_cursor_x2 == x2 &&
        s_cursor_row == y) {
        stream_begin_inner(0x3C);                    /* RAMWRC — hiç bedeli yok */
        return;
    }
    if (y == 0) {
        caset_inner(x1, x2);
        stream_begin_inner(0x2C);
        return;
    }
    strip_ile_atla(y);                          /* imleç -> satır y */
    caset_inner(x1, x2);
    stream_begin_inner(0x3C);                        /* satırı koruyarak devam */
}

static void imleci_isaretle(uint32_t x1, uint32_t x2, uint32_t row) {
    s_cursor_valid = true;
    s_cursor_x1 = x1;
    s_cursor_x2 = x2;
    s_cursor_row = row;
}

/**
 * Pencereyi 2 piksele hizala. Dönen aralık x1 çift, x2 tek.
 * `sol` ve `sag`: kaç piksellik kenar dolgusu gerektiği (0 veya 1).
 */
static void pencereyi_hizala(uint32_t x, uint32_t w,
                             uint32_t *x1, uint32_t *x2,
                             uint32_t *left, uint32_t *right) {
    *x1 = x & ~1u;
    *x2 = (x + w - 1) | 1u;
    if (*x2 >= PB_PANEL_W) *x2 = PB_PANEL_W - 1;   /* 171 zaten tek */
    *left = x - *x1;
    *right = *x2 - (x + w - 1);
}

void pb_lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 const uint16_t *buf) {
    if (!buf || w == 0 || h == 0) return;
    if (x >= PB_PANEL_W || y >= PB_PANEL_H) return;
    if (x + w > PB_PANEL_W) w = PB_PANEL_W - x;
    if (y + h > PB_PANEL_H) h = PB_PANEL_H - y;

    uint32_t x1, x2, left, right;
    pencereyi_hizala(x, w, &x1, &x2, &left, &right);
    const uint32_t pw = x2 - x1 + 1;

    imleci_konumla(x1, x2, y);

    /* Satır satır DEĞİL, yığın yığın: bitişik tampon + tek DMA (yukarıya bak) */
    uint32_t row_start = PB_STACK_PIXELS / pw;
    if (row_start == 0) row_start = 1;

    for (uint32_t row0 = 0; row0 < h; row0 += row_start) {
        uint32_t n = h - row0;
        if (n > row_start) n = row_start;

        uint16_t *dst = s_heap;
        for (uint32_t r = 0; r < n; r++) {
            const uint16_t *src = buf + (size_t)(row0 + r) * w;
            for (uint32_t i = 0; i < w; i++) {
                dst[left + i] = (uint16_t)((src[i] >> 8) | (src[i] << 8));
            }
            /* Hizalama dolgusu: kenar pikseli kopyalanıyor. Hizalı çağrılarda
             * (LVGL dahil, bkz. lv_port.c'deki alan_yuvarla) hiç çalışmaz. */
            if (left) dst[0] = dst[1];
            if (right) dst[pw - 1] = dst[pw - 2];
            if (x1 == 0 && y + row0 + r < PB_PANEL_H) {
                s_strip[y + row0 + r][0] = dst[0];
                s_strip[y + row0 + r][1] = dst[1];
            }
            dst += pw;
        }
        yigini_gonder(n * pw);
    }

    pb_lcd_stream_end();
    imleci_isaretle(x1, x2, y + h);
}

void pb_lcd_blit_strided(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         const uint16_t *buf, int32_t col_step, int32_t row_step) {
    if (!buf || w == 0 || h == 0) return;
    if (x >= PB_PANEL_W || y >= PB_PANEL_H) return;
    /* Kırpma yapmıyoruz: adımlar negatif olabildiği için kırpılmış bir
     * dikdörtgenin kaynak başlangıcı da kaymalı ve bunu çağıran taraf
     * bilmeden yapmak sessiz hataya davetiye. Sınır dışı istek reddedilir. */
    if (x + w > PB_PANEL_W || y + h > PB_PANEL_H) return;

    uint32_t x1, x2, left, right;
    pencereyi_hizala(x, w, &x1, &x2, &left, &right);
    const uint32_t pw = x2 - x1 + 1;

    imleci_konumla(x1, x2, y);

    uint32_t row_start = PB_STACK_PIXELS / pw;
    if (row_start == 0) row_start = 1;

    for (uint32_t row0 = 0; row0 < h; row0 += row_start) {
        uint32_t n = h - row0;
        if (n > row_start) n = row_start;

        uint16_t *dst = s_heap;
        for (uint32_t r = 0; r < n; r++) {
            const uint16_t *src = buf + (int32_t)(row0 + r) * row_step;
            for (uint32_t i = 0; i < w; i++) {
                uint16_t px = src[(int32_t)i * col_step];
                dst[left + i] = (uint16_t)((px >> 8) | (px << 8));
            }
            if (left) dst[0] = dst[1];
            if (right) dst[pw - 1] = dst[pw - 2];
            if (x1 == 0 && y + row0 + r < PB_PANEL_H) {
                s_strip[y + row0 + r][0] = dst[0];
                s_strip[y + row0 + r][1] = dst[1];
            }
            dst += pw;
        }
        yigini_gonder(n * pw);
    }

    pb_lcd_stream_end();
    imleci_isaretle(x1, x2, y + h);
}

void pb_lcd_fill(uint16_t color) {
    /* Tek geçiş: sütun penceresi tam genişlik, RAMWR, bütün ekran.
     * Eski hâli 640 ayrı pencere+RAMWR yapıyordu ve her satır aynı ÜST
     * satıra biniyordu (§9n'in baş belirtisi: "ekran temizlenmiyor"). */
    caset_inner(0, PB_PANEL_W - 1);
    stream_begin_inner(0x2C);
    pb_lcd_stream_color(color, (uint32_t)PB_PANEL_W * PB_PANEL_H);
    pb_lcd_stream_end();

    const uint16_t be = (uint16_t)((color >> 8) | (color << 8));
    for (uint32_t r = 0; r < PB_PANEL_H; r++) { s_strip[r][0] = be; s_strip[r][1] = be; }
    s_strip_valid = true;
    imleci_isaretle(0, PB_PANEL_W - 1, PB_PANEL_H);
}

void pb_lcd_stream_flat(uint16_t color, uint32_t pixel) {
    pb_lcd_stream_begin(0x2c);
    pb_lcd_stream_color(color, pixel);
    pb_lcd_stream_end();
    pb_lcd_cursor_invalidate();
}
