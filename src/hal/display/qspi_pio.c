/*****************************************************************************
* | File      	:   qspi_pio.c
* | Author      :   Waveshare Team
* | Function    :   QSPI Interface Functions
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2025-03-20
* | Info        :   
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of theex Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
******************************************************************************/
#include "qspi_pio.h"
#include "pico/stdlib.h"
#include "hardware/timer.h"

/* POKEBIRD teshis sayaclari — bkz. lastsession.md §9n.
 *
 * QSPI_WaitIdle'in 50 ms'lik zaman asimi SESSIZ: zaman asimina girerse
 * hicbir sey beklemeden doner ve §5.9'un hatasi geri gelir (CS, veri hatta
 * cikmadan yukselir). "Bekleme gercekten calisiyor mu" sorusunu ekrana
 * bakmadan yanitlayabilmek icin sayiliyor.
 *
 * Bedeli: cagri basina bir 32 bit timer okumasi + birkac sayac. `w` komutu
 * bunlari okuyor; `o`/`a` sonrasi da bakilabilir. */
volatile uint32_t pb_qspi_wait_cagri;       /* toplam QSPI_WaitIdle cagrisi   */
volatile uint32_t pb_qspi_wait_asim;        /* zaman asimina giren cagri      */
volatile uint32_t pb_qspi_wait_sm_kapali;   /* girerken SM etkin degildi      */
volatile uint32_t pb_qspi_wait_fifo_dolu;   /* girerken TX FIFO bos DEGILDI   */
volatile uint32_t pb_qspi_wait_kalinti;     /* CIKARKEN FIFO hala bos degil   */
volatile uint32_t pb_qspi_wait_bekledi;     /* dongu en az bir kez dondu      */
volatile uint32_t pb_qspi_wait_fifo_azami;  /* girerkenki en yuksek FIFO      */
volatile uint32_t pb_qspi_wait_donme_azami; /* en cok dongu sayisi (tek cagri)*/
volatile uint32_t pb_qspi_wait_us_azami;    /* en uzun tek bekleme (us)       */
volatile uint32_t pb_qspi_wait_us_top;      /* toplam bekleme (us)            */

void pb_qspi_sayaclari_sifirla(void) {
    pb_qspi_wait_cagri = 0;
    pb_qspi_wait_asim = 0;
    pb_qspi_wait_sm_kapali = 0;
    pb_qspi_wait_fifo_dolu = 0;
    pb_qspi_wait_kalinti = 0;
    pb_qspi_wait_bekledi = 0;
    pb_qspi_wait_fifo_azami = 0;
    pb_qspi_wait_donme_azami = 0;
    pb_qspi_wait_us_azami = 0;
    pb_qspi_wait_us_top = 0;
}

pio_qspi_t qspi = {
    .pio = pio0,
    .sm = 0,
    .sm_4wire = 0,
    .sm_1wire = 1,
    .pin_cs = PIN_CS,
    .pin_sclk = PIN_SCLK,
    .pin_dio0 = PIN_DIO0,
    .pin_dio1 = PIN_DIO1,
    .pin_dio2 = PIN_DIO2,
    .pin_dio3 = PIN_DIO3,
    .pin_pwr_en = PIN_PWR_EN,
    .pin_rst = PIN_RST
};

/******************************************************************************
function : QSPI related GPIO initialization
parameter:
    qspi : QSPI structure
******************************************************************************/	
void QSPI_GPIO_Init(pio_qspi_t qspi){
    gpio_init(qspi.pin_cs);
    gpio_pull_down(qspi.pin_cs);
    gpio_set_dir(qspi.pin_cs,GPIO_OUT);
    gpio_put(qspi.pin_cs,1);

    gpio_init(qspi.pin_pwr_en);
    gpio_set_dir(qspi.pin_pwr_en,GPIO_OUT);
    gpio_put(qspi.pin_pwr_en,1);

    gpio_init(qspi.pin_rst);
    gpio_set_dir(qspi.pin_rst,GPIO_OUT);
}

/******************************************************************************
function : QSPI Select
parameter:
    qspi : QSPI structure
******************************************************************************/	
void QSPI_Select(pio_qspi_t qspi){
    gpio_put(qspi.pin_cs,0);
}

/******************************************************************************
function : QSPI Wait Idle  (POKEBIRD eklemesi)

PIO'nun kuyruga alinan son biti de gercekten hatta cikarmasini bekler.

NEDEN GEREKLI — bu, ekranin hic calismamasinin sebebiydi:
`pio_sm_put_blocking()` yalnizca FIFO DOLU iken bekler; veriyi FIFO'ya
koyar koymaz doner. Ayni sekilde `dma_channel_is_busy()` yanlisa dondugunde
baytlar FIFO'ya yazilmis olur, hatta cikmis olmaz. Orijinal QSPI_Deselect
CS'i hemen yukseltiyordu; PIO ise o sirada hala kaydiriyordu. Panel her
islemi yarida kesilmis goruyor ve TEK BIR KOMUTU BILE kabul etmiyordu.
(Olcum: panel 60 Hz tariyor ama TEOFF'a tepki vermiyor — bkz. `v` teshisi.)

TXSTALL bayragi, SM bos OSR/FIFO ile bir OUT'ta takildiginda kurulur; yani
"kaydirilacak bit kalmadi" demektir. Once bayragi temizleyip sonra kurulmasini
bekliyoruz.

Zaman asimi var: SM kapaliysa TXSTALL hic kurulmaz, sonsuz donguye girmeyelim.
******************************************************************************/
void QSPI_WaitIdle(pio_qspi_t qspi){
    const uint32_t stall = 1u << (PIO_FDEBUG_TXSTALL_LSB + qspi.sm);

    /* --- TESHIS (§9n) — fonksiyon gercekten bekliyor mu? -------------------
     * Girerken FIFO'da bayt varsa bekleme GEREKLI demektir; cikarken hala
     * varsa bekleme ISE YARAMAMIS demektir. Ikisi de sayiliyor. */
    pb_qspi_wait_cagri++;
    if (!((qspi.pio->ctrl >> qspi.sm) & 1u)) pb_qspi_wait_sm_kapali++;
    uint32_t giris_fifo = pio_sm_get_tx_fifo_level(qspi.pio, qspi.sm);
    if (giris_fifo) {
        pb_qspi_wait_fifo_dolu++;
        if (giris_fifo > pb_qspi_wait_fifo_azami) pb_qspi_wait_fifo_azami = giris_fifo;
    }
    uint32_t t0 = timer_hw->timerawl;
    uint32_t donme = 0;

    qspi.pio->fdebug = stall;                       /* bayragi temizle */
    absolute_time_t bitis = make_timeout_time_ms(50);
    while (!(qspi.pio->fdebug & stall)) {
        if (time_reached(bitis)) { pb_qspi_wait_asim++; break; }  /* SM kapali/tikali */
        donme++;
        tight_loop_contents();
    }

    uint32_t us = timer_hw->timerawl - t0;
    pb_qspi_wait_us_top += us;
    if (us > pb_qspi_wait_us_azami) pb_qspi_wait_us_azami = us;
    if (donme) {
        pb_qspi_wait_bekledi++;
        if (donme > pb_qspi_wait_donme_azami) pb_qspi_wait_donme_azami = donme;
    }
    if (!pio_sm_is_tx_fifo_empty(qspi.pio, qspi.sm)) pb_qspi_wait_kalinti++;
}

/******************************************************************************
function : QSPI Deselect
parameter:
    qspi : QSPI structure
******************************************************************************/
void QSPI_Deselect(pio_qspi_t qspi){
    /* CS'i yukseltmeden ONCE son bitin hatta cikmasini bekle (bkz. yukarisi).
     * Bekleme burada yapiliyor ki her cagri yeri — satici dosyalari dahil —
     * tek bir duzeltmeden faydalansin. */
    QSPI_WaitIdle(qspi);
    gpio_put(qspi.pin_cs,1);
}

/******************************************************************************
function : QSPI PIO initialization
parameter:
    qspi : QSPI structure
******************************************************************************/	
/* POKEBIRD: 4 telli programin PIO komut bellegindeki yeri. QSPI_PIO_Restore
 * bunu kullaniyor — pio_add_program'i TEKRAR cagirmak komut bellegini
 * tuketir (32 komutluk yer var, her cagri 2 komut daha yakiyor). */
static uint s_qspi_offset;
static bool s_qspi_program_yuklu = false;

void QSPI_PIO_Init(pio_qspi_t qspi){
    uint offset = pio_add_program(qspi.pio, &qspi_4wire_data_program);
    s_qspi_offset = offset;
    s_qspi_program_yuklu = true;
    qspi_4wire_data_program_init(qspi.pio, qspi.sm_4wire, offset, PIN_SCLK, PIN_DIO0, 4);

    // offset = pio_add_program(qspi.pio, &qspi_1write_cmd_program);
    // qspi_1write_cmd_program_init(qspi.pio, qspi.sm_1wire, offset, PIN_SCLK, PIN_DIO0, 1);
    // pio_sm_clear_fifos(qspi.pio, qspi.sm_1wire);

    pio_sm_set_enabled(qspi.pio, qspi.sm_4wire, false);  
    pio_sm_set_enabled(qspi.pio, qspi.sm_1wire, false);  
}

/******************************************************************************
function : QSPI PIO'yu bit-bang testinden sonra geri al  (POKEBIRD eklemesi)

Bit-bang teshisi (cmd_display_test'in 4. varyanti, `v`'nin 6. adimi) SCLK ve
D0..D3'u gpio_set_function(SIO) ile PIO'nun elinden aliyor ve SM'i kapatiyor.
Geri vermeyi kimse yapmiyordu: bit-bang'den SONRA calistirilan her ekran testi
sahte bicimde "bozuk" gorunuyordu (lastsession.md §9n'deki uyari).

pio_add_program'i TEKRAR CAGIRMIYORUZ; yalnizca pin islevleri, SM
yapilandirmasi ve FIFO'lar sifirlaniyor.
******************************************************************************/
void QSPI_PIO_Restore(pio_qspi_t qspi){
    if (!s_qspi_program_yuklu) { QSPI_PIO_Init(qspi); }

    /* CS yeniden duz GPIO cikisi (bit-bang de oyle birakiyor ama emin olalim) */
    gpio_init(qspi.pin_cs);
    gpio_set_dir(qspi.pin_cs, GPIO_OUT);
    gpio_put(qspi.pin_cs, 1);

    /* Pinleri ve SM'i PIO'ya geri ver; program_init SM'i etkin birakiyor. */
    qspi_4wire_data_program_init(qspi.pio, qspi.sm_4wire, s_qspi_offset,
                                 PIN_SCLK, PIN_DIO0, 4);
}

/******************************************************************************
function : QSPI PIO one-line mode, generally used to send commands
parameter:
    qspi : QSPI structure
******************************************************************************/	
void QSPI_1Wrie_Mode(pio_qspi_t *qspi){
    pio_sm_set_enabled(qspi->pio, qspi->sm_4wire, false);  
    pio_sm_set_enabled(qspi->pio, qspi->sm_1wire, true);  
    qspi->sm = qspi->sm_1wire;
}

/******************************************************************************
function : QSPI PIO four-wire mode, generally used to send data
parameter:
    qspi : QSPI structure
******************************************************************************/	
void QSPI_4Wrie_Mode(pio_qspi_t *qspi){
    pio_sm_set_enabled(qspi->pio, qspi->sm_4wire, true); 
    pio_sm_set_enabled(qspi->pio, qspi->sm_1wire, false);   
    qspi->sm = qspi->sm_4wire;
}

/******************************************************************************
function : QSPI PIO sends data
parameter:
    qspi : QSPI structure
******************************************************************************/	
static void QSPI_PIO_Write(pio_qspi_t qspi, uint32_t val){
    pio_sm_put_blocking(qspi.pio, qspi.sm, val << 24);
}

/******************************************************************************
function : QSPI PIO 1-wire mode sends data
parameter:
    qspi : QSPI structure
******************************************************************************/	
void QSPI_DATA_Write(pio_qspi_t qspi, uint32_t val){
    // QSPI_PIO_Write(qspi,val);
    uint8_t cmd_buf[4];
    uint8_t buf_temp;
    for (int i = 0; i < 4; ++i)
    {
        uint8_t bit1 = (val & (1 << (2 * i))) ? 1 : 0;
        uint8_t bit2 = (val & (1 << (2 * i + 1))) ? 1 : 0;
        cmd_buf[3 - i] = bit1 | (bit2 << 4);
    }

    for (int i = 0; i < 4 ; i++)
    {
        QSPI_PIO_Write(qspi,cmd_buf[i]);
    }
}

/******************************************************************************
function : QSPI PIO 1-wire mode sends data
parameter:
    qspi : QSPI structure
******************************************************************************/	
void QSPI_CMD_Write(pio_qspi_t qspi, uint32_t val){
    // QSPI_PIO_Write(qspi,val);
    uint8_t cmd_buf[4];
    uint8_t buf_temp;
    for (int i = 0; i < 4; ++i)
    {
        uint8_t bit1 = (val & (1 << (2 * i))) ? 1 : 0;
        uint8_t bit2 = (val & (1 << (2 * i + 1))) ? 1 : 0;
        cmd_buf[3 - i] = bit1 | (bit2 << 4);
    }

    for (int i = 0; i < 4 ; i++)
    {
        QSPI_PIO_Write(qspi,cmd_buf[i]);
    }
}

/******************************************************************************
function : QSPI PIO 1-wire mode configuration register
parameter:
    qspi : QSPI structure
    addr : Register address
******************************************************************************/	
void QSPI_REGISTER_Write(pio_qspi_t qspi, uint32_t addr){
    //1 WIRE CMD
    QSPI_CMD_Write(qspi,0x02);

    //1 WIRE ADDR
    QSPI_DATA_Write(qspi,0x00);
    QSPI_DATA_Write(qspi,addr);
    QSPI_DATA_Write(qspi,0x00);
    // WAIT_TIME();
}

/******************************************************************************
function : QSPI RGB pixel interface one line to send address
parameter:
    qspi : QSPI structure
    addr : RGB pixel interface register address
******************************************************************************/	
void QSPI_Pixel_Write(pio_qspi_t qspi, uint32_t addr){
    //1 WIRE CMD
    QSPI_CMD_Write(qspi,0x32);
    
    //1 WIRE ADDR
    QSPI_DATA_Write(qspi,0x00);
    QSPI_DATA_Write(qspi,addr);
    QSPI_DATA_Write(qspi,0x00);
    // WAIT_TIME();
}
