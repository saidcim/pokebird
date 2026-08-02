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
void pb_lcd_duz_akit(uint16_t renk, uint32_t piksel);

#endif /* POKEBIRD_LCD_BLIT_H */
