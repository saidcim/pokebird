/**
 * mel.h — Log-mel öznitelik çıkarımı ve halka tamponu
 *
 * Plan §3'teki parametreler:
 *   24 kHz mono · FFT 512 (Hann) · hop 384 (16 ms) · 64 mel bandı
 *   150 Hz – 11.5 kHz · 3 s pencere = 187 kare
 *
 * KRİTİK BELLEK KARARI: 3 saniyelik ham ses TUTULMUYOR (144 KB olurdu).
 * Mel kareleri gelen sese göre ARTIMLI hesaplanıp 64×187'lik int8 halka
 * tamponda tutuluyor — 12 KB. Tek başına ~130 KB kazandıran karar bu.
 *
 * REFERANSLA UYUM: filtre bankası HTK mel ölçeği ve alan normalizasyonu
 * OLMADAN kuruluyor. Python karşılığı birebir şudur:
 *
 *     librosa.filters.mel(sr=24000, n_fft=512, n_mels=64,
 *                         fmin=150, fmax=11500, htk=True, norm=None)
 *
 * Pencere de periyodik Hann (`sym=False`). Bu iki ayrıntı tutmazsa cihazdaki
 * öznitelikler eğitimdekilerle uyuşmaz ve model sessizce kötü çalışır —
 * hata ayıklaması en pahalı sınıftan olur.
 */
#ifndef POKEBIRD_MEL_H
#define POKEBIRD_MEL_H

#include <stdbool.h>
#include <stdint.h>

#define PB_SAMPLE_RATE   24000
#define PB_MEL_BANDS     64
#define PB_MEL_HOP       384          /* 16 ms */
#define PB_MEL_FRAMES    187          /* 3 s / 16 ms */
#define PB_MEL_FMIN      150.0f
#define PB_MEL_FMAX      11500.0f

/* int8 saklama: log-güç dB olarak bu aralığa doğrusal eşleniyor.
 * -90 dBFS taban, M1'de ölçülen ~-36 dBFS oda gürültüsünün epey altında;
 * 0 dBFS tavan tam ölçek. Çözünürlük ~0.35 dB — kuş sesi için fazlasıyla. */
#define PB_MEL_DB_MIN    (-90.0f)
#define PB_MEL_DB_MAX    (0.0f)

/** Filtre bankasını kur. Diğer çağrılardan önce bir kez. */
void pb_mel_init(void);

/**
 * Tek kare log-mel üret (halka tamponuna dokunmaz).
 * @param samples PB_FFT_SIZE adet int16
 * @param out     PB_MEL_BANDS adet int8, dB→int8 eşlemesi yukarıda
 */
void pb_mel_frame(const int16_t *samples, int8_t *out);

/** Kareyi hesaplayıp halka tamponuna it. */
void pb_mel_push(const int16_t *samples);

/** Halka tamponunu sıfırla. */
void pb_mel_reset(void);

/** Tampona itilmiş toplam kare sayısı (3 s dolduğunu anlamak için). */
uint32_t pb_mel_frame_count(void);

/**
 * Son itilen karenin int8 değerlerini kopyala — canlı gösterim için.
 * Mel karesini yeniden HESAPLAMAZ; halkadan okur.
 * @param out PB_MEL_BANDS adet int8
 * @return    henüz hiç kare itilmediyse false
 */
bool pb_mel_last_frame(int8_t *out);

/**
 * Son 3 saniyeyi, model girdisi olarak normalize edilmiş hâlde ver.
 *
 * Pencere içi ortalama/varyans normalizasyonu burada yapılıyor (plan §3):
 * kayıt seviyesi ve mikrofon kazancı cihazdan cihaza değişiyor, mutlak dB
 * değerleri modele verilirse bu değişkenlik doğrudan doğruluğa yansır.
 *
 * @param out PB_MEL_BANDS * PB_MEL_FRAMES adet int8, kare sırası eskiden yeniye
 * @return    henüz 187 kare birikmediyse false (çıktı yazılmaz)
 */
bool pb_mel_window(int8_t *out);

/** int8 saklama değerini dB'ye çevir — test ve teşhis için. */
float pb_mel_q_to_db(int8_t q);

#endif /* POKEBIRD_MEL_H */
