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

/* DMA parça boyutu. PIO'nun RX FIFO'su (join ile 8 kelime) parçalar arasındaki
 * boşlukta ~333 us tampon sağlıyor; parça başına dönüşüm işi bunun çok altında. */
#define PB_CHUNK_WORDS      1024

static uint32_t s_chunk[PB_CHUNK_WORDS];
static int      s_dma_chan = -1;
static bool     s_mclk_running = false;
static bool     s_rx_ready = false;

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

    s_dma_chan = dma_claim_unused_channel(false);
    if (s_dma_chan < 0) return false;

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
    return true;
}

/* PIO FDEBUG'daki RXSTALL biti: RX FIFO doluyken IN komutu tıkandı, yani
 * örnek düştü. Ölçümün güvenilir olup olmadığını söyleyen tek gerçek gösterge. */
static inline void fdebug_clear_rxstall(void) {
    PB_PIO->fdebug = (1u << PB_SM_RX);
}
static inline bool fdebug_rxstall(void) {
    return (PB_PIO->fdebug & (1u << PB_SM_RX)) != 0;
}

pb_capture_result_t pb_audio_capture(int16_t *dst, uint32_t n_samples) {
    pb_capture_result_t res = { .samples = 0, .fifo_overrun = false, .timed_out = false };
    if (!s_rx_ready || !dst || n_samples == 0) {
        res.timed_out = true;
        return res;
    }

    dma_channel_config c = dma_channel_get_default_config((uint)s_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(PB_PIO, PB_SM_RX, false));

    /* State machine'i DURDURMUYORUZ (yukarıdaki gerekçe). Yalnızca birikmiş
     * bayat örnekleri boşaltıyoruz; çerçeve kilidi bozulmadan kalıyor. */
    while (!pio_sm_is_rx_fifo_empty(PB_PIO, PB_SM_RX)) {
        (void)pio_sm_get(PB_PIO, PB_SM_RX);
    }
    fdebug_clear_rxstall();

    uint32_t done = 0;
    while (done < n_samples) {
        uint32_t want = n_samples - done;
        if (want > PB_CHUNK_WORDS) want = PB_CHUNK_WORDS;

        dma_channel_configure((uint)s_dma_chan, &c,
                              s_chunk,                       /* hedef */
                              &PB_PIO->rxf[PB_SM_RX],        /* kaynak */
                              want,
                              true);                         /* hemen başlat */

        /* Saat yoksa DMA sonsuza kadar bekler. ES8311 yanlış yapılandırıldıysa
         * ya da MCLK gitmiyorsa burada takılmak yerine hata döndürmek gerekir. */
        absolute_time_t deadline = make_timeout_time_ms(1000);
        while (dma_channel_is_busy((uint)s_dma_chan)) {
            if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) {
                dma_channel_abort((uint)s_dma_chan);
                res.samples = done;
                res.timed_out = true;
                return res;
            }
            tight_loop_contents();
        }

        /* PIO çerçeve başına tek mono örnek gönderiyor; 16 bitlik autopush
         * eşiği nedeniyle örnek kelimenin alt 16 bitinde. */
        for (uint32_t i = 0; i < want; i++) {
            dst[done + i] = (int16_t)(s_chunk[i] & 0xFFFFu);
        }
        done += want;
    }

    res.samples = done;
    res.fifo_overrun = fdebug_rxstall();
    return res;
}
