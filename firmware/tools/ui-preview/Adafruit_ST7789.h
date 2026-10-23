#pragma once
#include <cstdint>
extern uint16_t preview_pixels[240*240];
class Adafruit_ST7789 {
public:
    void drawRGBBitmap(int x,int y,const uint16_t* data,int w,int h) {
        for(int j=0;j<h;++j)for(int i=0;i<w;++i)
            if(x+i>=0 && x+i<240 && y+j>=0 && y+j<240)preview_pixels[(y+j)*240+x+i]=data[j*w+i];
    }
};
