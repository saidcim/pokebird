/**
 * touch.h — AXS15231B kapasitif dokunmatik (I2C0, GPIO32/33, adres 0x3B)
 *
 * NEDEN SATICI `Touch.c`'Sİ DEĞİL — iki ayrı sebep:
 *
 * 1. Waveshare'in Touch.c'si `i2c_write_blocking` / `i2c_read_blocking`
 *    kullanıyor, zaman aşımı yok. Dokunmatik ACK vermezse çağrı geri dönmüyor
 *    ve TÜM cihaz kilitleniyor. Ses hattı gerçek zamanlı, göze alınamaz.
 *
 * 2. Daha önemlisi: Waveshare'in okuma protokolü YANLIŞ. Komut dizisinin
 *    7. baytı okunacak bayt sayısı; Waveshare oraya 0x0E yazıp 32 bayt
 *    okuyor. Bu uyuşmazlık çipin senkronunu bozuyor — kartta ölçüldü, ilk
 *    okumadan sonra hep 0xDB dönüyordu. Aynı panelin çalışan sürücüsü
 *    (rsvpnano/src/input/TouchHandler.cpp) 0x08 yazıp 8 bayt okuyor.
 *
 * KOORDİNATLAR: çip zaten yatay veriyor — ham x uzun eksen (0..640),
 * ham y kısa eksen (0..172). Aynalama gerekip gerekmediği `t` komutuyla
 * ölçülür, varsayılmaz.
 */
#ifndef POKEBIRD_TOUCH_H
#define POKEBIRD_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

/** Çipin bir okumada verdiği paket uzunluğu. Komuttaki uzunluk baytıyla
 *  AYNI olmak zorunda; uyuşmazsa çip senkronu kaybediyor. */
#define PB_TOUCH_PACKET_LEN 8

typedef struct {
    uint16_t raw_x;     /**< uzun eksen, 0..640 */
    uint16_t raw_y;     /**< kısa eksen, 0..172 */
} pb_touch_point_t;

typedef struct {
    bool             ok;        /**< I2C işlemi başarılı mı */
    uint8_t          fingers;   /**< 0 = dokunulmuyor */
    pb_touch_point_t p;         /**< 8 baytlık paket tek nokta taşıyor */
} pb_touch_state_t;

/** I2C0'ı başlat ve çipin yanıt verdiğini doğrula. */
bool pb_touch_init(void);

/** Anlık durumu oku. Hat yanıt vermezse .ok = false döner, bloklamaz. */
pb_touch_state_t pb_touch_read(void);

/** Son okumanın ham paketi — teşhis için. */
void pb_touch_last_raw(uint8_t out[PB_TOUCH_PACKET_LEN]);

#endif /* POKEBIRD_TOUCH_H */
