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

/** Tüm paneli tek renkle doldur. */
void pb_lcd_fill(uint16_t color);

#endif /* POKEBIRD_LCD_BLIT_H */
