/*****************************************************************************
* | File      	:   Touch.h
* | Author      :   Waveshare Team
* | Function    :   Touch Interface Functions
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
#ifndef _TOUCH_H_
#define _TOUCH_H_

#include <stdint.h>
#include "hardware/i2c.h"
#include "pico/time.h"

#define TOUCH_I2C_ADDR  0x3B

typedef struct{
	uint8_t  Finger_Num;
	uint16_t Point1_x;
	uint16_t Point1_y;
	uint16_t Point2_x;
	uint16_t Point2_y;
} Touch_Struct;

extern Touch_Struct TOUCH;

void Touch_Read_State();
#endif // !_TOUCH_H_
