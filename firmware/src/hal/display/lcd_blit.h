/**
 * lcd_blit.h — Küçük tampondan ekrana dikdörtgen aktarımı
 *
 * NEDEN KENDİ FONKSİYONUMUZ:
 * Waveshare'in LCD_3IN49_DisplayWindows() fonksiyonu, verilen `Image`
 * işaretçisini TAM EKRAN framebuffer sanıyor:
 *     pixel_offset = (i * LCD_3IN49.WIDTH + Xstart) * 2;
 * Yani küçük bir tampon verirseniz sınırların dışını okuyor. Bizim tasarımımız
 * ise tam framebuffer'ı bilerek reddediyor (220 KB, SRAM'in %42'si — plan §5).
 * Bu yüzden tamponu düz, ardışık piksel dizisi olarak ele alan kendi
 * fonksiyonumuz gerekiyor.
 *
 * KOORDİNATLAR — DİKKAT:
 * Bu fonksiyon panelin DOĞAL yönünde çalışır: X 0..171, Y 0..639.
 * Arayüzün yatay (640x172) koordinatları ui/ katmanında çevrilir.
 *
 * BAYT SIRASI:
 * Panel RGB565'i big-endian bekliyor. Fonksiyon çeviriyi kendi yapıyor;
 * çağıran normal (little-endian) uint16_t verir.
 */
#ifndef POKEBIRD_LCD_BLIT_H
#define POKEBIRD_LCD_BLIT_H

#include <stdint.h>

#define PB_PANEL_W 172
#define PB_PANEL_H 640

/**
 * ⚠ PANELİN SÖZLEŞMESİ — RASET (0x2B) YOK SAYILIYOR (ölçüldü, §9n)
 *
 * Bu panelde satır penceresi diye bir şey yok. Yazma imlecinin satırını
 * yalnızca iki komut belirliyor:
 *   0x2C RAMWR   → imleç sütun penceresinin EN ÜST satırına döner
 *   0x3C RAMWRC  → imleç bir önceki yazmanın bittiği yerden DEVAM eder
 * Sütun aralığı CASET (0x2A) ile ayarlanıyor ve o çalışıyor.
 *
 * Sonuç: **y>0 olan bir dikdörtgene rastgele erişim ücretsiz değil.** Sürücü
 * imleci takip ediyor; imleç zaten hedef satırdaysa RAMWRC ile bedava devam
 * eder, değilse RAMWR'den başlayıp aradaki satırları SİYAHLA geçer — yani
 * **o sütun aralığında yukarısı silinir.** Doğru kullanım: bir kareyi
 * yukarıdan aşağı, sırayla çizmek. Bunu ihlal eden kod sessizce değil,
 * gözle görülür biçimde bozulur.
 *
 * `buf`'taki w*h pikseli panelin (x,y) konumuna yaz.
 * `buf` satır sıralı ve ardışık olmalı (w piksel, sonra bir sonraki satır).
 * Piksel biçimi: normal RGB565 (little-endian uint16_t).
 */
void pb_lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 const uint16_t *buf);

/**
 * Adımlı (strided) blit — kaynağı gezerken satır ve sütun adımı verilebilir.
 *
 * NEDEN: LVGL arayüzü YATAY (640x172), panel ise DİKEY (172x640). Aradaki 90°
 * çevrim normalde tamponun devriğini (transpose) almayı, yani ikinci bir
 * tampon kadar daha RAM'i gerektirir — bizde o RAM yok (plan §5).
 *
 * Bunun yerine kaynağı devrik SIRAYLA okuyoruz: panelin bir yatay satırı,
 * LVGL tamponunun bir dikey sütunudur. Negatif adım da geçerli; aynalama
 * bununla hallediliyor. Ek tampon maliyeti SIFIR.
 *
 * piksel(satır r, sütun c) = buf[r*row_step + c*col_step]
 *
 * `x/y/w/h` panelin doğal koordinatlarında. Sınır dışı istek sessizce
 * kırpılmaz, REDDEDİLİR: negatif adımlarda kırpma kaynak başlangıcını da
 * kaydırmayı gerektirir ve bunu çağıran bilmeden yapmak sessiz hataya yol açar.
 */
void pb_lcd_blit_strided(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         const uint16_t *buf, int32_t col_step, int32_t row_step);

/** Tüm paneli tek renkle doldur. */
void pb_lcd_fill(uint16_t color);

/**
 * Pencereyi AYARLAMADAN düz renk piksel akıt — melez yol testi için (§9n).
 *
 * NEDEN AYRI: `pb_lcd_blit` pencereyi kendi ayarlıyor, dolayısıyla "pencere
 * komutu" ile "piksel verisi" aynı yoldan (PIO) gidiyor. Ekran hatasında
 * ikisini ayırmak gerekiyor: pencereyi bit-bang, pikselleri PIO ile (ya da
 * tersi) yollayıp hangisinin düştüğünü görebilmek için. Pencereyi çağıran
 * ayarlar; bu fonksiyon yalnızca RAMWR + veri yolluyor.
 */
void pb_lcd_stream_flat(uint16_t renk, uint32_t piksel);

/**
 * Ham piksel akışı — panelin GERÇEK sözleşmesini ifade eden üçlü.
 *
 * Bu panelde (AXS15231B) satır penceresi (RASET, 0x2B) YOK SAYILIYOR; yazma
 * imlecinin satırını yalnızca RAMWR/RAMWRC belirliyor:
 *   0x2C (RAMWR)  — imleci sütun penceresinin EN ÜSTÜNE alır
 *   0x3C (RAMWRC) — bir önceki yazmanın bittiği yerden DEVAM eder
 * Sütun aralığı CASET (0x2A) ile ayrı ayarlanır. Kaynak: panelin çalışan iki
 * bağımsız sürücüsü (rsvpnano, ESP32 ve RP2350-PIO) — ikisi de RASET
 * yollamıyor. Ayrıntı lastsession.md §9n.
 *
 * `basla` CS'i indirip komutu yollar, `renk` düz renk akıtır (kaç kez
 * çağrılırsa), `bitir` CS'i kaldırır. Pencereyi çağıran ayarlar.
 */
void pb_lcd_stream_begin(uint8_t ramwr);
void pb_lcd_stream_color(uint16_t renk, uint32_t piksel);
/** Tek satır (n piksel, normal RGB565) akıt; bayt sırasını kendi çevirir. */
void pb_lcd_stream_row(const uint16_t *src, uint32_t n);
void pb_lcd_stream_end(void);

/** Sütun aralığı (CASET, 0x2A). Kapsayıcı: x1 ve x2 dahil. */
void pb_lcd_column_window(uint32_t x1, uint32_t x2);

/**
 * Panele bu dosyanın dışından komut/veri yollayan her kod bunu çağırmalı.
 * Sürücü imlecin nerede olduğunu takip ediyor; başkası panele yazınca bu
 * bilgi yanlışa döner ve bir sonraki blit sessizce yanlış yere düşer.
 */
void pb_lcd_cursor_invalidate(void);

/** Sonraki `adet` satırı DMA'ya giderken seri porta ASCII dök (teşhis). */
void pb_lcd_request_row_dump(int adet);

#endif /* POKEBIRD_LCD_BLIT_H */
