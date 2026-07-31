#include "pages/topbar/topbar_assets.h"
#include "pages/topbar/topbar_bitmap.h"
#include "devices/epd_xingtai/epd_xingtai.h"
namespace {
void pixel(uint8_t *f,int x,int y){if(x<0||x>=XingtaiEpd::WIDTH||y<0||y>=XingtaiEpd::HEIGHT)return;f[y*(XingtaiEpd::WIDTH/8)+x/8]|=0x80>>(x%8);}
void line(uint8_t *f,int x,int y,int w){for(int i=0;i<w;++i)pixel(f,x+i,y);}
void bitmap(uint8_t *f,int x,int y,const uint8_t *data){
    for(int row=0;row<TopbarBitmap::HEIGHT;++row){
        for(int col=0;col<TopbarBitmap::WIDTH;++col){
            if(data[row*TopbarBitmap::ROW_BYTES+col/8]&(0x80>>(col%8))) pixel(f,x+col,y+row);
        }
    }
}
}
namespace Topbar {
void drawHome(uint8_t*f,int x,int y){bitmap(f,x,y,TopbarBitmap::HOME);}
void drawWifi(uint8_t*f,int x,int y){bitmap(f,x,y,TopbarBitmap::WIFI);}
void drawBle(uint8_t*f,int x,int y){line(f,x+8,y,1);line(f,x+8,y+15,1);line(f,x+4,y+4,8);line(f,x+4,y+12,8);line(f,x+4,y+4,1);line(f,x+4,y+12,1);line(f,x+8,y+7,1);}
void draw4G(uint8_t*f,int x,int y){line(f,x+1,y+3,3);line(f,x+1,y+3,1);line(f,x+1,y+3,1);line(f,x+1,y+8,3);line(f,x+4,y+3,1);line(f,x+4,y+8,1);line(f,x+7,y+3,1);line(f,x+7,y+3,5);line(f,x+7,y+8,1);line(f,x+10,y+3,1);line(f,x+10,y+8,1);line(f,x+10,y+3,4);line(f,x+10,y+6,3);line(f,x+15,y+3,1);line(f,x+15,y+8,1);line(f,x+15,y+3,1);}
void drawBattery(uint8_t*f,int x,int y){bitmap(f,x,y,TopbarBitmap::BATTERY);}
}
