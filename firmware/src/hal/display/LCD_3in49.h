/*****************************************************************************
* | File      	:   LCD_3IN49.h
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
#ifndef _LCD_3IN49_H_
#define _LCD_3IN49_H_

#include "DEV_Config.h"   /* POKEBIRD: UWORD/UBYTE tipleri */

#include "qspi_pio.h"

// #define LCD_3IN49_WIDTH 368
// #define LCD_3IN49_HEIGHT 448

// #define LCD_3IN49_WIDTH 280
// #define LCD_3IN49_HEIGHT 456

#define LCD_3IN49_WIDTH 172
#define LCD_3IN49_HEIGHT 640

#define HORIZONTAL 0
#define VERTICAL   1

#define WHITE         0xFFFF
#define BLACK		  0x0000
#define BLUE 		  0x001F
#define BRED 	      0XF81F
#define GRED 		  0XFFE0
#define GBLUE		  0X07FF
#define RED  		  0xF800
#define MAGENTA		  0xF81F
#define GREEN		  0x07E0
#define CYAN 		  0x7FFF
#define YELLOW		  0xFFE0
#define BROWN		  0XBC40
#define BRRED		  0XFC07
#define GRAY 	      0X8430
#define DARKBLUE	  0X01CF
#define LIGHTBLUE	  0X7D7C
#define GRAYBLUE      0X5458
#define LIGHTGREEN    0X841F
#define LGRAY 		  0XC618
#define LGRAYBLUE     0XA651
#define LBBLUE        0X2B12

typedef struct{
    UWORD WIDTH;
    UWORD HEIGHT;
    UBYTE SCAN_DIR;
}LCD_3IN49_ATTRIBUTES;
extern LCD_3IN49_ATTRIBUTES LCD_3IN49;

/* POKEBIRD — baslatma varyantlari (yalnizca hata ayiklama; bkz. LCD_3in49.c) */
#define LCD_3IN49_INIT_FULL        0  /* satici tablosu + SLPOUT/MADCTL/COLMOD/DISPON */
#define LCD_3IN49_INIT_MINIMAL     1  /* sadece DCS kuyrugu (rsvpnano ile ayni)       */
#define LCD_3IN49_INIT_NO_COLMOD   2  /* satici tablosu + SLPOUT/DISPON, COLMOD YOK   */

void LCD_3IN49_Init();
void LCD_3IN49_InitVariant(int variant);
void LCD_3IN49_SendSimpleCmd(uint8_t cmd);
void LCD_3IN49_SetWindows(uint32_t Xstart, uint32_t Ystart, uint32_t Xend, uint32_t Yend);
void LCD_3IN49_Display(UWORD *Image);
void LCD_3IN49_DisplayWindows(uint32_t Xstart, uint32_t Ystart, uint32_t Xend, uint32_t Yend, UWORD *Image);
void LCD_3IN49_Clear(UWORD Color);

#endif // !_LCD_3IN49_H_
