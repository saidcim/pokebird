/**
 * audio_i2s.c — ES8311 mikrofonundan I2S yakalama (PIO + DMA)
 *
 * PIO programları Waveshare'in RP2350-Touch-LCD-3.5 örneğinden uyarlandı (MIT);
 * ayrıntı için audio_i2s.pio başlığına bakın.
 */
#include "hal/audio_i2s.h"

#include <assert.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

#include "audio_i2s.pio.h"
#include "board_config.h"

/* audio_i2s.pio içindeki `wait ... gpio N` komutları MUTLAK GPIO numarası
 * kullanır ve C tarafından parametreleştirilemez. Pinler değişirse .pio dosyası
 * da elle güncellenmeli; bu kontroller sessizce yanlış pini dinlememizi önler. */
static_assert(PB_PIN_I2S_SCLK == 4, "audio_i2s.pio 'wait gpio 4' ile BCLK'yi bekliyor");
static_assert(PB_PIN_I2S_LRCK == 5, "audio_i2s.pio 'wait gpio 5' ile LRCK'yi bekliyor");

#define PB_PIO              pio1
#define PB_SM_MCLK          0
#define PB_SM_RX            1

/* ── Halka tamponu ─────────────────────────────────────────────────────────
 *
 * DMA'nın adres sarma (ring) özelliği kullanılıyor: yazma adresinin alt
 * bitleri maskeleniyor, böylece kanal tampon sonuna gelince kendiliğinden
 * başa dönüyor. Bunun iki şartı var — tampon boyutu ikinin kuvveti ve tampon
 * kendi boyutuna hizalı olmalı.
 *
 * 4096 örnek = 16 KB = 24 kHz'de 170 ms. Mel hattının bir karesi 16 ms;
 * yani tüketici on kare geri kalsa bile örnek kaybolmuyor. USB üzerinden
 * ham kayıt aktarırken (`r` komutu) yaşanan duraklamalar için de pay bu. */
#define PB_RING_WORDS       PB_AUDIO_RING_SAMPLES
#define PB_RING_MASK        (PB_RING_WORDS - 1)
#define PB_RING_ADDR_BITS   14                      /* 1<<14 = 16384 bayt */

static_assert((PB_RING_WORDS & PB_RING_MASK) == 0, "halka boyutu ikinin kuvveti olmali");
static_assert((1u << PB_RING_ADDR_BITS) == PB_RING_WORDS * sizeof(uint32_t),
              "PB_RING_ADDR_BITS halka boyutuyla uyusmuyor");

/* DMA yazıyor, CPU okuyor: derleyicinin okumaları önbelleğe almasını
 * engellemek için volatile. */
static volatile uint32_t s_ring[PB_RING_WORDS]
    __attribute__((aligned(1u << PB_RING_ADDR_BITS)));

/* Kontrol kanalının veri kanalına geri yazdığı sayaç değeri. Bilerek RAM'de:
 * DMA'nın flash'tan (XIP) okuması gereksiz bir bağımlılık olurdu. */
static uint32_t s_reload_words = PB_RING_WORDS;

static uint32_t s_read_idx = 0;
static int      s_dma_data = -1;
static int      s_dma_ctrl = -1;
static bool     s_mclk_running = false;
static bool     s_rx_ready = false;
static bool     s_stream_running = false;

/* ── MCLK ──────────────────────────────────────────────────────────────── */

bool pb_audio_mclk_start(const pb_audio_cfg_t *cfg) {
    if (s_mclk_running) return true;
    if (!cfg || cfg->mclk_freq == 0) return false;

    uint offset = pio_add_program(PB_PIO, &mclk_pio_program);
    mclk_pio_program_init(PB_PIO, PB_SM_MCLK, offset, PB_PIN_I2S_MCLK);

    /* mclk_pio döngüsü 5 komut sürüyor, dolayısıyla bir MCLK periyodu
     * 5 x clkdiv sistem saati kadar. 150 MHz / (4.8828125 x 5) = 6.144 MHz. */
    float div = ((float)clock_get_hz(clk_sys) / (float)cfg->mclk_freq) / 5.0f;
    pio_sm_set_clkdiv(PB_PIO, PB_SM_MCLK, div);
    pio_sm_set_enabled(PB_PIO, PB_SM_MCLK, true);

    s_mclk_running = true;
    return true;
}

/* ── Yakalama ──────────────────────────────────────────────────────────── */

bool pb_audio_i2s_init(const pb_audio_cfg_t *cfg) {
    if (!pb_audio_mclk_start(cfg)) return false;
    if (s_rx_ready) return true;

    uint offset = pio_add_program(PB_PIO, &i2s_rx_pio_program);
    i2s_rx_pio_program_init(PB_PIO, PB_SM_RX, offset,
                            PB_PIN_I2S_DSOUT, PB_PIN_I2S_SCLK, PB_PIN_I2S_LRCK);
    /* RX state machine, ES8311'in ürettiği saati takip ediyor; kendi
     * bölücüsüyle yavaşlatılmamalı. */
    pio_sm_set_clkdiv(PB_PIO, PB_SM_RX, 1.0f);

    s_dma_data = dma_claim_unused_channel(false);
    s_dma_ctrl = dma_claim_unused_channel(false);
    if (s_dma_data < 0 || s_dma_ctrl < 0) return false;

    /* RX state machine BİR KEZ burada başlatılır ve bir daha durdurulmaz.
     *
     * Neden: her yakalamada durdurup pio_sm_restart() ile yeniden başlatmak,
     * I2S çerçeve kilidinin her seferinde yeniden kurulmasına yol açıyordu.
     * LRCK ve BCLK kenarları neredeyse aynı anda değiştiği için bu yeniden
     * kilitlenme yarış hâline geliyor ve kilit bazen yanlış yuvaya oturuyordu;
     * sonuç, ölçümlerin rastgele bir kısmında tamamen gürültü okumaktı
     * (ilintisiz, 16 bitin tamamı rastgele — basit bit kayması değil).
     *
     * Kilit bir kez doğru kurulduğunda kendiliğinden korunuyor: program her
     * çerçevede LRCK düşen kenarında yeniden hizalanıyor. */
    pio_sm_clear_fifos(PB_PIO, PB_SM_RX);
    pio_sm_restart(PB_PIO, PB_SM_RX);
    pio_sm_exec(PB_PIO, PB_SM_RX, pio_encode_jmp(offset));  /* PC'yi başa al */
    pio_sm_set_enabled(PB_PIO, PB_SM_RX, true);

    s_rx_ready = true;
    return pb_audio_stream_start();
}

/* PIO FDEBUG'daki RXSTALL biti: RX FIFO doluyken IN komutu tıkandı, yani
 * örnek düştü. Ölçümün güvenilir olup olmadığını söyleyen tek gerçek gösterge. */
static inline void fdebug_clear_rxstall(void) {
    PB_PIO->fdebug = (1u << PB_SM_RX);
}
static inline bool fdebug_rxstall(void) {
    return (PB_PIO->fdebug & (1u << PB_SM_RX)) != 0;
}

/* ── Sürekli yakalama: kendini yenileyen DMA ───────────────────────────────
 *
 * İki kanal kullanılıyor:
 *   veri     — PIO RX FIFO -> halka tamponu, DREQ ile hızlanıyor, sarma açık.
 *              Bittiğinde ZİNCİRLE kontrol kanalını tetikliyor.
 *   kontrol  — tek kelime yazar: veri kanalının sayaç register'ının TETİKLEYEN
 *              takma adına (al1_transfer_count_trig) halka boyutunu koyar,
 *              böylece veri kanalı anında yeniden başlar.
 *
 * Sonuç: CPU hiç karışmadan sonsuza kadar dönen bir yakalama. Yazma adresi
 * sarma sayesinde tam tur atıp başa döndüğü için kontrol kanalının adresi
 * ayrıca sıfırlamasına gerek yok. İki tur arasındaki birkaç saat çevrimlik
 * boşluğu PIO'nun RX FIFO'su (join ile 8 kelime, ~333 us) fazlasıyla kapatıyor.
 */

static bool stream_configure(void) {
    dma_channel_config dc = dma_channel_get_default_config((uint)s_dma_data);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_ring(&dc, true, PB_RING_ADDR_BITS);   /* yazma adresi sarar */
    channel_config_set_dreq(&dc, pio_get_dreq(PB_PIO, PB_SM_RX, false));
    channel_config_set_chain_to(&dc, (uint)s_dma_ctrl);
    dma_channel_configure((uint)s_dma_data, &dc,
                          (void *)s_ring,                  /* hedef  */
                          &PB_PIO->rxf[PB_SM_RX],          /* kaynak */
                          PB_RING_WORDS,
                          false);                          /* başlatma */

    dma_channel_config cc = dma_channel_get_default_config((uint)s_dma_ctrl);
    channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
    channel_config_set_read_increment(&cc, false);
    channel_config_set_write_increment(&cc, false);
    channel_config_set_chain_to(&cc, (uint)s_dma_ctrl);     /* kendine = zincir yok */
    dma_channel_configure((uint)s_dma_ctrl, &cc,
                          &dma_hw->ch[s_dma_data].al1_transfer_count_trig,
                          &s_reload_words,
                          1,
                          false);
    return true;
}

/** Halkada DMA'nın şu an yazdığı konum (kelime indeksi). */
static inline uint32_t ring_write_index(void) {
    uint32_t off = (uint32_t)((uintptr_t)dma_hw->ch[s_dma_data].write_addr -
                              (uintptr_t)s_ring);
    return (off >> 2) & PB_RING_MASK;
}

bool pb_audio_stream_start(void) {
    if (!s_rx_ready) return false;
    if (s_stream_running) return true;
    if (!stream_configure()) return false;

    /* Bayat örnekleri at: state machine BAŞTAN BERİ çalışıyor, FIFO'da
     * bekleyen kelimeler olabilir. State machine durdurulmuyor (§ yukarıda). */
    while (!pio_sm_is_rx_fifo_empty(PB_PIO, PB_SM_RX)) {
        (void)pio_sm_get(PB_PIO, PB_SM_RX);
    }
    fdebug_clear_rxstall();

    s_read_idx = 0;                       /* yazma da tamponun başından başlıyor */
    dma_channel_start((uint)s_dma_data);
    s_stream_running = true;
    return true;
}

void pb_audio_stream_stop(void) {
    if (!s_stream_running) return;

    /* ÖNCE zinciri kır. İptal edilen bir kanal zincirini tetikleyebiliyor;
     * zincir dururken iptal edilirse kontrol kanalı veri kanalını hemen
     * yeniden başlatır ve durdurma işe yaramaz. al1_ctrl tetiklemeyen takma
     * ad, çalışırken yazmak güvenli. */
    uint32_t ctrl = dma_hw->ch[s_dma_data].al1_ctrl;
    ctrl &= ~DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS;
    ctrl |= ((uint32_t)s_dma_data << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB);
    dma_hw->ch[s_dma_data].al1_ctrl = ctrl;

    dma_channel_abort((uint)s_dma_data);
    dma_channel_abort((uint)s_dma_ctrl);
    s_stream_running = false;
}

void pb_audio_stream_flush(void) {
    if (!s_stream_running) return;
    s_read_idx = ring_write_index();
    fdebug_clear_rxstall();
}

uint32_t pb_audio_stream_available(void) {
    if (!s_stream_running) return 0;
    return (ring_write_index() - s_read_idx) & PB_RING_MASK;
}

pb_capture_result_t pb_audio_stream_read(int16_t *dst, uint32_t n_samples,
                                         uint32_t timeout_ms) {
    pb_capture_result_t res = { .samples = 0, .fifo_overrun = false, .timed_out = false };
    if (!s_stream_running || !dst || n_samples == 0 || n_samples > PB_AUDIO_MAX_READ) {
        res.timed_out = true;
        return res;
    }

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms ? timeout_ms : 1000);
    uint32_t got = 0;

    while (got < n_samples) {
        uint32_t avail = pb_audio_stream_available();

        if (avail == 0) {
            /* Saat yoksa (ES8311 BCLK/LRCK üretmiyorsa) halka hiç dolmaz;
             * burada sonsuza kadar beklemek yerine hata döndürüyoruz. */
            if (time_reached(deadline)) { res.timed_out = true; break; }
            tight_loop_contents();
            continue;
        }

        /* Tüketici halkanın dörtte üçü kadar geri kaldıysa en eski örnekler
         * ezilmek üzere: en tazeye atla ve bunu bildir. Sessizce süreksiz
         * veri döndürmek, ölçümü sessizce bozardı. */
        if (avail > (PB_RING_WORDS - PB_RING_WORDS / 4)) {
            res.fifo_overrun = true;
            s_read_idx = (ring_write_index() - PB_AUDIO_MAX_READ) & PB_RING_MASK;
            avail = PB_AUDIO_MAX_READ;
        }

        uint32_t take = n_samples - got;
        if (take > avail) take = avail;

        /* PIO çerçeve başına tek mono örnek gönderiyor; 16 bitlik autopush
         * eşiği nedeniyle örnek kelimenin alt 16 bitinde. */
        for (uint32_t i = 0; i < take; i++) {
            dst[got + i] = (int16_t)(s_ring[(s_read_idx + i) & PB_RING_MASK] & 0xFFFFu);
        }
        s_read_idx = (s_read_idx + take) & PB_RING_MASK;
        got += take;
    }

    if (fdebug_rxstall()) {          /* DMA yetişemedi: PIO FIFO taştı */
        res.fifo_overrun = true;
        fdebug_clear_rxstall();
    }
    res.samples = got;
    return res;
}

pb_capture_result_t pb_audio_capture(int16_t *dst, uint32_t n_samples) {
    pb_capture_result_t res = { .samples = 0, .fifo_overrun = false, .timed_out = false };
    if (!dst || n_samples == 0) {
        res.timed_out = true;
        return res;
    }

    pb_audio_stream_flush();

    uint32_t done = 0;
    while (done < n_samples) {
        uint32_t want = n_samples - done;
        if (want > PB_AUDIO_MAX_READ) want = PB_AUDIO_MAX_READ;

        pb_capture_result_t part = pb_audio_stream_read(dst + done, want, 1000);
        res.samples      += part.samples;
        res.fifo_overrun |= part.fifo_overrun;
        res.timed_out    |= part.timed_out;
        done             += part.samples;

        if (part.timed_out) break;
    }
    return res;
}
