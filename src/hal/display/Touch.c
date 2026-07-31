/*****************************************************************************
* | File      	:   TOUCH.c
* | Author      :   Waveshare Team
* | Function    :   TOUCH Interface Functions
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
#include "Touch.h"
#include "DEV_Config.h"

Touch_Struct TOUCH;
uint8_t read_touchpad_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e, 0x0, 0x0, 0x0};
uint8_t read_touchpad_data[32] = {0};

/******************************************************************************
function :	Send n byte of data to the specified register of TOUCH
parameter:
******************************************************************************/
static void Touch_I2C_Write_nByte(uint8_t buf[], uint8_t len) {
    i2c_write_blocking(TOUCH_I2C_PORT, TOUCH_I2C_ADDR, buf, len, true);
}

/******************************************************************************
function :	Read n byte of data from the specified register of TOUCH
parameter:
******************************************************************************/
static void Touch_I2C_Read_nByte(uint8_t buf[], uint8_t len) {
    i2c_read_blocking(TOUCH_I2C_PORT, TOUCH_I2C_ADDR, buf, len, false);
}

/******************************************************************************
function :	Read the current status of TOUCH
parameter:
******************************************************************************/
void Touch_Read_State(){
    Touch_I2C_Write_nByte(read_touchpad_cmd,11);
    Touch_I2C_Read_nByte(read_touchpad_data,32);
    TOUCH.Finger_Num = read_touchpad_data[1]; 
    uint16_t pointX;
    uint16_t pointY;
    pointX = (((uint16_t)read_touchpad_data[2] & 0x0f) << 8) | (uint16_t)read_touchpad_data[3];
    pointY = (((uint16_t)read_touchpad_data[4] & 0x0f) << 8) | (uint16_t)read_touchpad_data[5];
    if(pointX > 640) pointX = 640;
    if(pointY > 172) pointY = 172;
    TOUCH.Point1_x = pointY;
    TOUCH.Point1_y = 640-pointX;
    if(TOUCH.Finger_Num > 1)
    {
        pointX = (((uint16_t)read_touchpad_data[8] & 0x0f) << 8) | (uint16_t)read_touchpad_data[9];
        pointY = (((uint16_t)read_touchpad_data[10] & 0x0f) << 8) | (uint16_t)read_touchpad_data[11];
        if(pointX > 640) pointX = 640;
        if(pointY > 172) pointY = 172;
        TOUCH.Point2_x = pointY;
        TOUCH.Point2_y = 640-pointX;
    }
}

