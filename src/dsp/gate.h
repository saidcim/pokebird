/**
 * gate.h — Aşama 0: ucuz "ilgilenmeye değer bir şey var mı" kapısı
 *
 * Şehirde cihaz zamanının çok büyük kısmında ilginç bir şey duymuyor. Ağır
 * işi (mel + iki sinir ağı) her karede çalıştırmak hem pil hem CPU israfı.
 * Bu kapı, plan §3'e göre iki ucuz ölçüte bakıyor:
 *
 *   1. 2–10 kHz bandındaki enerji, UYARLAMALI gürültü tabanına göre.
 *      Sabit eşik işe yaramaz: oda, sokak ve park gürültüsü birbirinden
 *      onlarca dB farklı (M1'de oda tabanı -36 dBFS ölçüldü, fanla birlikte).
 *   2. Spektral akı — bandın şekli ne kadar hızlı değişiyor. Sabit uğultu
 *      (klima, trafik) enerjiyi yükseltir ama akıyı yükseltmez; kuş ötüşü
 *      ikisini birden yükseltir.
 *
 * Taban tek yönlü hızlarla izleniyor: sessizliğe hızlı iner, gürültüye yavaş
 * çıkar. Tersi olsaydı uzun bir ötüş tabanı kendi üstüne çeker ve kuşu
 * kendi kendine gizlerdi.
 */
#ifndef POKEBIRD_GATE_H
#define POKEBIRD_GATE_H

#include <stdbool.h>
#include <stdint.h>

/* Tam sayı: bin sınırları derleme zamanı sabiti olmalı, yoksa bant tamponu
 * "variably modified at file scope" olur ve statik ayrılamaz. */
#define PB_GATE_F_LO 2000
#define PB_GATE_F_HI 10000

typedef struct {
    bool  active;      /**< kapı açık mı */
    float band_db;     /**< bu karedeki bant enerjisi (dBFS) */
    float floor_db;    /**< izlenen gürültü tabanı (dBFS) */
    float flux;        /**< spektral akı (0..) */
} pb_gate_result_t;

/** Durumu sıfırla. Taban ilk karelerde hızla yakalar. */
void pb_gate_reset(void);

/**
 * Bir kareyi değerlendir.
 * @param power pb_fft_power() çıktısı, PB_FFT_POWER_BINS adet
 */
pb_gate_result_t pb_gate_update(const float *power);

#endif /* POKEBIRD_GATE_H */
