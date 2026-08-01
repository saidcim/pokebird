/**
 * lv_conf.h — LVGL v9.3 yapılandırması (PokeBird)
 *
 * `third_party/lvgl/lv_conf_template.h`'nin tamamını kopyalamak yerine
 * yalnızca varsayılandan SAPTIĞIMIZ ayarlar burada; gerisi LVGL'in kendi
 * varsayılanlarından geliyor (`lv_conf_internal.h` tanımsız her makroyu
 * varsayılanıyla dolduruyor). Böylece LVGL yükseltmesinde bu dosya küçük
 * kalıyor ve hangi kararın bilinçli olduğu görülüyor.
 *
 * EN SERT KISIT: 520 KB SRAM, PSRAM YOK. Plan §5'teki bütçe LVGL'e ~26 KB
 * çizim tamponu ayırıyor; nesne yığını genel yığından geliyor.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* ── Renk ve bellek ────────────────────────────────────────────────────── */

/* Panel RGB565. Bayt sırası çevrimi bizim blit'imizde yapılıyor
 * (bkz. hal/display/lcd_blit.c), LVGL'e swap ettirmiyoruz. */
#define LV_COLOR_DEPTH 16

/* LVGL'in kendi yığını. Nesneler, stiller, animasyonlar buradan.
 * 24 KB, 640x172'lik tek ekranlık basit bir arayüz için bol; darlık
 * olursa lv_mem_monitor() ile ölçüp büyütün, tahminle değil. */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN
#define LV_MEM_SIZE             (24 * 1024)

/* Zaman tabanı: v9'da makro yok, çalışma anında `lv_tick_set_cb()` ile
 * veriliyor (bkz. ui/lv_port.c). v8'deki LV_TICK_CUSTOM burada işe yaramaz. */

/* ── Çizim ─────────────────────────────────────────────────────────────── */
/* Tek çekirdek, yardımcı çizim birimi yok. */
#define LV_USE_DRAW_SW          1
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_DRAW_THREAD_STACK_SIZE (2 * 1024)

/* Karmaşık efektler (gölge, degrade maskesi) hem RAM hem CPU yiyor;
 * bu arayüzde ihtiyaç yok. */
#define LV_DRAW_SW_COMPLEX      0

/* ── Kapatılan özellikler — flash ve RAM tasarrufu ─────────────────────── */
#define LV_USE_LOG              0
#define LV_USE_ASSERT_NULL      1
#define LV_USE_ASSERT_MALLOC    1
#define LV_USE_ASSERT_STYLE     0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ       0

#define LV_USE_SYSMON           0

/* Dosya sistemi/görsel kod çözücüler M7'de (SD kart) gerekirse açılır.
 * (v9 adları: LODEPNG / TJPGD — v8'deki LV_USE_PNG / LV_USE_JPEGDEC değil.) */
#define LV_USE_FS_STDIO         0
#define LV_USE_LODEPNG          0
#define LV_USE_BMP              0
#define LV_USE_TJPGD            0
#define LV_USE_GIF              0
#define LV_USE_QRCODE           0

/* ── Yazı tipleri ──────────────────────────────────────────────────────── */
/* Ekran 172 px yüksekliğinde bir şerit: küçük ve orta boy iki tip yeter.
 * Her yazı tipi flash'ta yer kaplıyor, gereksizleri kapalı. */
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_DEFAULT         &lv_font_montserrat_14

/* ── Bileşenler ────────────────────────────────────────────────────────── */
/* M2b'de gerekenler: etiket, düğme, çubuk, liste benzeri düzenler.
 * Kullanılmayanlar kapalı; ihtiyaç doğdukça tek satırla açılır. */
#define LV_USE_LABEL            1
#define LV_USE_BUTTON           1
#define LV_USE_BAR              1
#define LV_USE_IMAGE            1
#define LV_USE_LINE             1
#define LV_USE_CANVAS           0   /* spektrogram doğrudan QSPI'ye yazıyor */
#define LV_USE_CHART            0
#define LV_USE_KEYBOARD         0
#define LV_USE_TEXTAREA         0
#define LV_USE_CALENDAR         0
#define LV_USE_ANIMIMG          0

/* Kapattıklarımıza BAĞLI olup varsayılanı açık olanlar. Bunlar kapatılmazsa
 * LVGL derleme sırasında #error veriyor (spinbox -> textarea, lottie ->
 * canvas). Bir bileşeni kapatırken bağımlısını da kapatmak gerekiyor. */
#define LV_USE_SPINBOX          0   /* textarea gerektirir */
#define LV_USE_LOTTIE           0   /* canvas + ThorVG gerektirir */

#define LV_USE_THEME_DEFAULT    1
#define LV_USE_THEME_SIMPLE     0
#define LV_USE_FLEX             1
#define LV_USE_GRID             0

/* Örnekler ve demolar derlenmesin. */
#define LV_BUILD_EXAMPLES       0

#endif /* LV_CONF_H */
