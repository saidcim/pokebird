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
 *
 * ⚠ 24 KB YETMEDİ — ÖLÇÜLDÜ, tahmin değil. Eski yorum "tek ekranlık basit bir
 * arayüz için bol" diyordu ve arayüz İKİ ekrana çıkınca yanlış oldu.
 * `tools/arayuz_onizle` (host, cihazla aynı lv_conf) `lv_mem_monitor` ile
 * ölçtü:
 *
 *     ekran 0 (dinleme) kurulduktan sonra:  16.384 / 20.624 bayt  = %80 dolu
 *     ekran 1 (gunluk) kurulurken           havuz tukendi
 *
 * (24 KB'ın ~20,6 KB'ı kullanılabilir; gerisi LVGL'in kendi defteri.)
 * Havuz tükenince `lv_obj_create` NULL dönüyor ve çağıran onu denetlemiyor —
 * host'ta segfault, kartta ise sessizce eksik/bozuk çizilen bir ekran.
 *
 * 64 KB seçildi: iki ekran ~33 KB, kalanı etiket metni değiştikçe oluşan
 * parçalanma ve ileride eklenecek ekranlar için pay. Bedeli karşılanabilir —
 * arayüzün dilim yoluna geçmesi bss'ten 19,9 KB kazandırmıştı ve ana SRAM'de
 * ~153 KB boş var. */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN
#define LV_MEM_SIZE             (64 * 1024)

/* Zaman tabanı: v9'da makro yok, çalışma anında `lv_tick_set_cb()` ile
 * veriliyor (bkz. ui/lv_port.c). v8'deki LV_TICK_CUSTOM burada işe yaramaz. */

/* ── Çizim ─────────────────────────────────────────────────────────────── */
/* Tek çekirdek, yardımcı çizim birimi yok. */
#define LV_USE_DRAW_SW          1
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_DRAW_THREAD_STACK_SIZE (2 * 1024)

/* ⚠ 1 OLMAK ZORUNDA — eski değeri 0'dı ve gerekçesi EKSİKTİ.
 *
 * Eski yorum "karmaşık efektler (gölge, degrade maskesi) gerekmez" diyordu.
 * Doğru ama yetersiz: bu bayrak **YUVARLAK KÖŞELERİ de** kapatıyor. Yarıçapı
 * sıfırdan büyük her dikdörtgen sessizce HİÇ ÇİZİLMİYOR — hata vermiyor,
 * sadece görünmüyor.
 *
 * Eski metin ağırlıklı kartta hiç yuvarlak nesne olmadığı için fark
 * edilmemişti. Yeni arayüz güven çubukları (yarıçap 3), sıra rozetleri
 * (daire) ve sayfa noktaları üzerine kurulu; hepsi kaybolmuştu.
 *
 * `tools/arayuz_onizle` ile YAKALANDI: ayraç çizgileri (yarıçap 0) çiziliyor,
 * çubuklar ve rozetler çizilmiyordu. Kartta da aynısı oluyordu. */
#define LV_DRAW_SW_COMPLEX      1

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
