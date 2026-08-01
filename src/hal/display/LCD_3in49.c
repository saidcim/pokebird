/*****************************************************************************
* | File      	:   LCD_3IN49.c
* | Author      :   Waveshare Team
* | Function    :   LCD Interface Functions
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
#include "DEV_Config.h"
#include "LCD_3in49.h"
/* POKEBIRD: Waveshare'in orijinali bunu kendi DEV_Config.h'i uzerinden
 * dolayli olarak aliyordu; bizim uyum katmanimiz DMA getirmedigi icin
 * dogrudan dahil ediyoruz. */
#include "hardware/dma.h"

LCD_3IN49_ATTRIBUTES LCD_3IN49;

/********************************************************************************
function:	Sets the start position and size of the display area
parameter:
        qspi    ：  qspi structure
		Xstart 	:   X direction Start coordinates
		Ystart  :   Y direction Start coordinates
		Xend    :   X direction end coordinates
		Yend    :   Y direction end coordinates
********************************************************************************/
void LCD_3IN49_SetWindows(uint32_t Xstart, uint32_t Ystart, uint32_t Xend, uint32_t Yend){
    QSPI_Select(qspi); 
    QSPI_REGISTER_Write(qspi, 0x2a); 
    QSPI_DATA_Write(qspi, Xstart>>8);
    QSPI_DATA_Write(qspi, Xstart&0xff);
    QSPI_DATA_Write(qspi, (Xend-1)>>8);
    QSPI_DATA_Write(qspi, (Xend-1)&0xff);
    QSPI_Deselect(qspi); 
    
    QSPI_Select(qspi); 
    QSPI_REGISTER_Write(qspi, 0x2b);
    QSPI_DATA_Write(qspi, Ystart>>8);
    QSPI_DATA_Write(qspi, Ystart&0xff);
    QSPI_DATA_Write(qspi, (Yend-1)>>8);
    QSPI_DATA_Write(qspi, (Yend-1)&0xff);
    QSPI_Deselect(qspi); 
    
    QSPI_Select(qspi); 
    QSPI_REGISTER_Write(qspi, 0x2c);
    QSPI_Deselect(qspi); 
}

typedef struct {
    int cmd;                /*<! The specific LCD command */
    const uint8_t *data;    /*<! Buffer that holds the command specific data */
    size_t data_bytes;      /*<! Size of `data` in memory, in bytes */
    unsigned int delay_ms;  /*<! Delay in milliseconds after this command */
} sh8601_lcd_init_cmd_t;

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = 
{
    {0xBB, (uint8_t []){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5}, 8, 0},
    {0xA0, (uint8_t []){0x00, 0x30, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, 17, 0},
    {0xA2, (uint8_t []){0x30, 0x19, 0x60, 0x64, 0x9B, 0x22, 0x38, 0x80, 0xAC, 0x28, 0x7F, 0x7F, 0x7F, 0x20, 0xF8, 0x10, 0x02, 0xFF, 0xFF, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xC0, 0x20, 0x7F, 0xFF, 0x00, 0x54}, 31, 0},
    {0xD0, (uint8_t []){0x80, 0xAC, 0x21, 0x24, 0x08, 0x09, 0x10, 0x01, 0x80, 0x12, 0xC2, 0x00, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x40, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x40, 0x10, 0x00, 0x03, 0x7D, 0x12}, 30, 0},
    {0xA3, (uint8_t []){0xA0, 0x06, 0xAA, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55}, 22, 0},
    {0xC1, (uint8_t []){0x33, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0D, 0x00, 0xFF, 0x40}, 30, 0},
    {0xC3, (uint8_t []){0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01}, 11, 0},
    {0xC4, (uint8_t []){0x00, 0x24, 0x33, 0x80, 0x11, 0xEA, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50}, 29, 0},
    {0xC5, (uint8_t []){0x18, 0x00, 0x00, 0x03, 0xFE, 0x08, 0x68, 0x30, 0x10, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x08, 0x68, 0x30, 0x10, 0x10, 0x00}, 23, 0},
    {0xC6, (uint8_t []){0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x00, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22}, 20, 0},
    {0xC7, (uint8_t []){0x50, 0x32, 0x28, 0x00, 0xA2, 0x80, 0x8F, 0x00, 0x80, 0xFF, 0x07, 0x11, 0x9F, 0x6F, 0xFF, 0x24, 0x0C, 0x0D, 0x0E, 0x0F}, 20, 0},
    {0xC9, (uint8_t []){0x33, 0x44, 0x44, 0x01}, 4, 0},
    {0xCF, (uint8_t []){0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0xF8, 0x00, 0x66, 0x0D, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x04, 0x04, 0x12, 0xA0, 0x08}, 28, 0},
    {0xD5, (uint8_t []){0x50, 0x60, 0x8A, 0x00, 0x35, 0x04, 0x60, 0x10, 0x03, 0x03, 0x03, 0x00, 0x04, 0x02, 0x13, 0x46, 0x03, 0x03, 0x03, 0x03, 0x86, 0x00, 0x00, 0x00, 0x80, 0x52, 0x7C, 0x00, 0x00, 0x00}, 30, 0},
    {0xD6, (uint8_t []){0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x00, 0x00, 0x01, 0x83, 0x03, 0x03, 0x33, 0x03, 0x03, 0x33, 0x3F, 0x03, 0x03, 0x03, 0x20, 0x20, 0x00, 0x24, 0x51, 0x23, 0x01, 0x00}, 30, 0},
    {0xD7, (uint8_t []){0x18, 0x1A, 0x1B, 0x1F, 0x0A, 0x08, 0x0E, 0x0C, 0x00, 0x1F, 0x1D, 0x1F, 0x50, 0x60, 0x04, 0x00, 0x1F, 0x1F, 0x1F}, 19, 0},
    {0xD8, (uint8_t []){0x18, 0x1A, 0x1B, 0x1F, 0x0B, 0x09, 0x0F, 0x0D, 0x01, 0x1F, 0x1D, 0x1F}, 12, 0},
    {0xD9, (uint8_t []){0x0F, 0x09, 0x0B, 0x1F, 0x18, 0x19, 0x1F, 0x01, 0x1E, 0x1D, 0x1F}, 11, 0},
    {0xDD, (uint8_t []){0x0E, 0x08, 0x0A, 0x1F, 0x18, 0x19, 0x1F, 0x00, 0x1E, 0x1A, 0x1F}, 11, 0},
    {0xDF, (uint8_t []){0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90}, 8, 0},
    {0xE0, (uint8_t []){0x35, 0x08, 0x19, 0x1C, 0x0C, 0x09, 0x13, 0x2A, 0x54, 0x21, 0x0B, 0x15, 0x13, 0x25, 0x27, 0x08, 0x00}, 17, 0},
    {0xE1, (uint8_t []){0x3E, 0x08, 0x19, 0x1C, 0x0C, 0x08, 0x13, 0x2A, 0x54, 0x21, 0x0B, 0x14, 0x13, 0x26, 0x27, 0x08, 0x0F}, 17, 0},
    {0xE2, (uint8_t []){0x19, 0x20, 0x0A, 0x11, 0x09, 0x06, 0x11, 0x25, 0xD4, 0x22, 0x0B, 0x13, 0x12, 0x2D, 0x32, 0x2F, 0x03}, 17, 0},
    {0xE3, (uint8_t []){0x38, 0x20, 0x0A, 0x11, 0x09, 0x06, 0x11, 0x25, 0xC4, 0x21, 0x0A, 0x12, 0x11, 0x2C, 0x32, 0x2F, 0x27}, 17, 0},
    {0xE4, (uint8_t []){0x19, 0x20, 0x0D, 0x14, 0x0D, 0x08, 0x12, 0x2A, 0xD4, 0x26, 0x0E, 0x15, 0x13, 0x34, 0x39, 0x2F, 0x03}, 17, 0},
    {0xE5, (uint8_t []){0x38, 0x20, 0x0D, 0x13, 0x0D, 0x07, 0x12, 0x29, 0xC4, 0x25, 0x0D, 0x15, 0x12, 0x33, 0x39, 0x2F, 0x27}, 17, 0},
    {0xA4, (uint8_t []){0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30}, 16, 0},
    {0xA4, (uint8_t []){0x85, 0x85, 0x95, 0x85}, 4, 0},
    {0xBB, (uint8_t []){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 0},

    // {0x11,(uint8_t []){0x00},0,100},
    // {0x29,(uint8_t []){0x00},0,100},
};

/* POKEBIRD — satici dizisindeki eksik: standart DCS kuyrugu
 *
 * Waveshare'in tablosu 0x11 (SLPOUT) ve 0x29 (DISPON) yorum satiri olarak
 * bitiyor ve 0x3A (COLMOD, piksel bicimi) hic yok. COLMOD ayarlanmazsa panel
 * reset varsayilaninda kaliyor; biz piksel basina 2 bayt (RGB565) gonderdigimiz
 * icin panelin bekledigi bayt sayisi tutmuyor ve GRAM'a kayik veri yaziliyor.
 * Belirti: her pikselin farkli renk oldugu, hic gitmeyen karincalanma.
 *
 * Ayni panelin calisan ESP32 surucusu (rsvpnano/src/display/axs15231b.cpp)
 * tam olarak bu diziyi yolluyor.
 */
static const sh8601_lcd_init_cmd_t lcd_dcs_tail_cmds[] =
{
    {0x11, (uint8_t []){0x00}, 0, 120},   /* SLPOUT  — uyku modundan cik      */
    {0x36, (uint8_t []){0x00}, 1, 0},     /* MADCTL  — donus/aynalama yok     */
    {0x3A, (uint8_t []){0x55}, 1, 0},     /* COLMOD  — 16 bit/piksel RGB565   */
    {0x29, (uint8_t []){0x00}, 0, 120},   /* DISPON  — paneli tara ve goster  */
};

/* COLMOD'suz kuyruk: karincalanmanin gercekten piksel biciminden geldigini
 * kanitlayan kontrol grubu (bkz. LCD_3IN49_INIT_NO_COLMOD). */
static const sh8601_lcd_init_cmd_t lcd_dcs_tail_no_colmod[] =
{
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x36, (uint8_t []){0x00}, 1, 0},
    {0x29, (uint8_t []){0x00}, 0, 120},
};

/* POKEBIRD — veri yolu teşhisi için: paneli tek, veri almayan bir DCS komutu
 * yolla (0x28 DISPOFF, 0x29 DISPON, 0x20/0x21 INVOFF/INVON gibi). Komutlar
 * panele ulaşıyor mu sorusunu gözle yanıtlatmak için kullanılıyor. */
void LCD_3IN49_SendSimpleCmd(uint8_t cmd)
{
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, cmd);
    QSPI_Deselect(qspi);
}

static void LCD_3IN49_SendCmds(const sh8601_lcd_init_cmd_t *cmds, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        const sh8601_lcd_init_cmd_t *cmd = &cmds[i];

        // Send commands and data
        QSPI_Select(qspi);
        QSPI_REGISTER_Write(qspi, cmd->cmd);

        // Write data byte
        for (size_t j = 0; j < cmd->data_bytes; j++) {
            QSPI_DATA_Write(qspi, cmd->data[j]);
        }
        QSPI_Deselect(qspi);

        // Delay
        if (cmd->delay_ms > 0) {
            sleep_ms(cmd->delay_ms);
        }
    }
}

/******************************************************************************
function :	Initialize the lcd register
parameter:
        qspi    ：  qspi structure
******************************************************************************/
static void LCD_3IN49_InitReg(){
    LCD_3IN49_SendCmds(lcd_init_cmds, sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]));
}

/********************************************************************************
function :	Reset the lcd
parameter:
        qspi    ：  qspi structure
********************************************************************************/
static void LCD_3IN49_Reset(pio_qspi_t qspi){
    gpio_put(qspi.pin_rst,1);
    DEV_Delay_ms(200);
    gpio_put(qspi.pin_rst,0);
    DEV_Delay_ms(200);
    gpio_put(qspi.pin_rst,1);
    DEV_Delay_ms(200);
}

/********************************************************************************
function :	Initialize the lcd
parameter:
        qspi    ：  qspi structure
********************************************************************************/
void LCD_3IN49_Init()
{
    LCD_3IN49_InitVariant(LCD_3IN49_INIT_FULL);
}

/******************************************************************************
function :	POKEBIRD — secilebilir baslatma varyanti
parameter:
        variant :  LCD_3IN49_INIT_* (bkz. LCD_3in49.h)

Varyantlar sadece hata ayiklama icin var; uretim yolu LCD_3IN49_INIT_FULL.
Ekranda ne oldugunu seri porttan goremedigimiz icin (bkz. lastsession.md §5.9)
hipotezleri tek tek denemek yerine hepsini tek firmware'e koyup kullaniciya
sorabilelim diye ayrildi.
******************************************************************************/
void LCD_3IN49_InitVariant(int variant)
{
    //Hardware reset
    LCD_3IN49_Reset(qspi);

    if (variant != LCD_3IN49_INIT_MINIMAL) {
        //Set the initialization register
        LCD_3IN49_InitReg(qspi);
    }

    if (variant == LCD_3IN49_INIT_NO_COLMOD) {
        LCD_3IN49_SendCmds(lcd_dcs_tail_no_colmod,
                           sizeof(lcd_dcs_tail_no_colmod) / sizeof(lcd_dcs_tail_no_colmod[0]));
    } else {
        LCD_3IN49_SendCmds(lcd_dcs_tail_cmds,
                           sizeof(lcd_dcs_tail_cmds) / sizeof(lcd_dcs_tail_cmds[0]));
    }

    LCD_3IN49.HEIGHT  = LCD_3IN49_HEIGHT;
    LCD_3IN49.WIDTH   = LCD_3IN49_WIDTH;
}

/******************************************************************************
function :	Clear screen
parameter:
******************************************************************************/
void LCD_3IN49_Clear(UWORD Color) {
    // Color data
    UWORD i;
	UWORD image[LCD_3IN49.WIDTH];
	for(i=0;i<LCD_3IN49.WIDTH;i++){
		image[i] = Color>>8 | (Color&0xff)<<8;
	}
	UBYTE *partial_image = (UBYTE *)(image);

    // Send command in one-line mode
    // QSPI_1Wrie_Mode(&qspi);
    LCD_3IN49_SetWindows(0,0,LCD_3IN49.WIDTH,LCD_3IN49.HEIGHT);
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi,0x2c);

    // Four-wire mode sends RGB data
    // QSPI_4Wrie_Mode(&qspi);
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));
    for (int i = 0; i <= LCD_3IN49.HEIGHT; i++) {
        dma_channel_configure(dma_tx, 
                            &c,
                            &qspi.pio->txf[qspi.sm],  // Destination pointer (PIO TX FIFO)
                            partial_image,            // Source pointer (data buffer)
                            LCD_3IN49.WIDTH*2,     // Data length (unit: number of transmissions)
                            true);                    // Start transferring immediately
        
        // Waiting for DMA transfer to complete
        while(dma_channel_is_busy(dma_tx));
    }

    QSPI_Deselect(qspi);
}

/******************************************************************************
function :	Send data to LCD to complete full screen refresh
parameter:
        Image   ：  Image data
******************************************************************************/
void LCD_3IN49_Display(UWORD *Image)
{
    // Send command in one-line mode
    // QSPI_1Wrie_Mode(&qspi);
    LCD_3IN49_SetWindows(0,0,LCD_3IN49.WIDTH,LCD_3IN49.HEIGHT);
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi,0x2c);

    // Four-wire mode sends RGB data
    // QSPI_4Wrie_Mode(&qspi);
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));
    dma_channel_configure(dma_tx, 
                        &c,
                        &qspi.pio->txf[qspi.sm],  // Destination pointer (PIO TX FIFO)
                        (UBYTE *)Image,           // Source pointer (data buffer)
                        (LCD_3IN49.WIDTH+1)*(LCD_3IN49.HEIGHT+1)*2,   // Data length (unit: number of transmissions)
                        true);                    // Start transferring immediately
    
    // Waiting for DMA transfer to complete
    while(dma_channel_is_busy(dma_tx));
    QSPI_Deselect(qspi);             
}

/******************************************************************************
function :	Send data to LCD to complete partial refresh
parameter:
		Xstart 	:   X direction Start coordinates
		Ystart  :   Y direction Start coordinates
		Xend    :   X direction end coordinates
		Yend    :   Y direction end coordinates
        Image   ：  Image data
******************************************************************************/
void LCD_3IN49_DisplayWindows(uint32_t Xstart, uint32_t Ystart, uint32_t Xend, uint32_t Yend, UWORD *Image) {
    // Send command in one-line mode
    // QSPI_1Wrie_Mode(&qspi);
    LCD_3IN49_SetWindows(Xstart, Ystart, Xend, Yend);
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, 0x2c);

    // Four-wire mode sends RGB data
    // QSPI_4Wrie_Mode(&qspi);
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));

    int i;
    uint32_t pixel_offset;
    UBYTE *partial_image;
    for (i = Ystart; i < Yend; i++) {
        pixel_offset = (i * LCD_3IN49.WIDTH + Xstart) * 2;
        partial_image = (UBYTE *)Image + pixel_offset;

        dma_channel_configure(dma_tx, 
                            &c,
                            &qspi.pio->txf[qspi.sm],  // Destination pointer (PIO TX FIFO)
                            partial_image,            // Source pointer (data buffer)
                            (Xend-Xstart)*2,          // Data length (unit: number of transmissions)
                            true);                    // Start transferring immediately

        // Waiting for DMA transfer to complete
        while(dma_channel_is_busy(dma_tx));
    }

    QSPI_Deselect(qspi);
}
