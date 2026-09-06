#include "dsp/gate.h"

#include <math.h>
#include <string.h>

#include "dsp/fft.h"
#include "dsp/mel.h"      /* PB_SAMPLE_RATE */

/* Bant sınırlarının bin karşılığı — tamamı tam sayı aritmetiği, çünkü
 * s_prev'in boyutu derleme zamanında bilinmek zorunda. */
#define BIN_LO ((PB_GATE_F_LO * PB_FFT_SIZE) / PB_SAMPLE_RATE)
#define BIN_HI ((PB_GATE_F_HI * PB_FFT_SIZE) / PB_SAMPLE_RATE)

/* Taban izleme hızları (kare başına dB). 16 ms'lik karelerle:
 *   düşüş 0.50 dB/kare -> ~30 dB/s, sessizliğe hızla oturur
 *   yükseliş 0.02 dB/kare -> ~1.2 dB/s, uzun bir ötüş tabanı yukarı çekemez
 * Asimetri bilinçli; gerekçesi gate.h'de. */
#define FLOOR_DOWN_DB 0.50f
#define FLOOR_UP_DB   0.02f

/* Kapı eşiği: taban üstü kaç dB. 6 dB, enerjinin dört katına denk geliyor. */
#define TRIGGER_DB    6.0f

/* Akı eşiği. Normalize edilmiş bant şekli üzerinden hesaplandığı için
 * birimsiz; 0.15 sessiz kayıtlarda gürültünün epey üstünde kalıyor. */
#define TRIGGER_FLUX  0.15f

#define NBANDS (BIN_HI - BIN_LO + 1)

static float s_floor_db;
static bool  s_have_floor;
static float s_prev[NBANDS];
static bool  s_have_prev;

void pb_gate_reset(void) {
    s_floor_db = 0.0f;
    s_have_floor = false;   /* ayrı bayrak: 0 dB geçerli bir değer, sentinel olamaz */
    s_have_prev = false;
    memset(s_prev, 0, sizeof(s_prev));
}

pb_gate_result_t pb_gate_update(const float *power) {
    pb_gate_result_t r;
    memset(&r, 0, sizeof(r));

    /* Bant enerjisi */
    float toplam = 0.0f;
    for (int k = BIN_LO; k <= BIN_HI; k++) toplam += power[k];
    r.band_db = 10.0f * log10f(toplam + 1e-12f);

    /* Spektral akı: bant şekli normalize edilip ardışık kareler arasındaki
     * POZİTİF farklar toplanıyor. Sadece artışlara bakmak önemli — sesin
     * kesilmesi de büyük bir fark üretir ama ilgilendiğimiz şey başlangıç. */
    /* Şekil vektörü her karede GÜNCELLENİYOR — sessiz kareler dahil.
     * Önce yalnızca enerji varken güncelleniyordu; sessizlikten sonra gelen
     * ilk sesli karede karşılaştıracak önceki şekil olmuyordu ve akı sıfır
     * çıkıyordu. Yani kapı tam da yakalaması gereken anı kaçırıyordu.
     * Host testi ("ani ton kapiyi aciyor") bunu yakaladı.
     * Sessizlikte şekil düzgün dağılım kabul ediliyor: enerji bir banda
     * toplandığında akı doğal olarak yükseliyor. */
    float simdiki[NBANDS];
    const float duz = 1.0f / (float)NBANDS;
    if (toplam > 1e-12f) {
        const float inv = 1.0f / toplam;
        for (int i = 0; i < NBANDS; i++) simdiki[i] = power[BIN_LO + i] * inv;
    } else {
        for (int i = 0; i < NBANDS; i++) simdiki[i] = duz;
    }

    float akı = 0.0f;
    if (s_have_prev) {
        for (int i = 0; i < NBANDS; i++) {
            float d = simdiki[i] - s_prev[i];
            if (d > 0.0f) akı += d;
        }
    }
    memcpy(s_prev, simdiki, sizeof(s_prev));
    s_have_prev = true;
    r.flux = akı;

    /* Uyarlamalı taban */
    if (!s_have_floor) {
        s_floor_db = r.band_db;          /* ilk kare: doğrudan otur */
        s_have_floor = true;
    } else if (r.band_db < s_floor_db) {
        s_floor_db -= FLOOR_DOWN_DB;
        if (s_floor_db < r.band_db) s_floor_db = r.band_db;
    } else {
        s_floor_db += FLOOR_UP_DB;
        if (s_floor_db > r.band_db) s_floor_db = r.band_db;
    }
    r.floor_db = s_floor_db;

    /* İki ölçüt de gerekiyor: sabit uğultu enerjiyi yükseltir ama akıyı
     * yükseltmez, kuş ötüşü ikisini birden yükseltir. */
    r.active = (r.band_db > s_floor_db + TRIGGER_DB) && (r.flux > TRIGGER_FLUX);
    return r;
}
