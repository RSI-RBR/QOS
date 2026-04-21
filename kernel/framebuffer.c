#include "framebuffer.h"
#include "mailbox.h"

static unsigned int width = 1920;
static unsigned int height = 1080;
static unsigned int pitch;
static unsigned int *fb;

//static volatile unsigned int mbox[36] __attribute__((aligned(16)));

void fb_init(){
    mbox[0] = 35 * 4;
    mbox[1] = 0;

    mbox[2] = 0x48003; // set phys width/height
    mbox[3] = 8;
    mbox[4] = 8;
    mbox[5] = width;
    mbox[6] = height;

    mbox[7] = 0x48004; // set virt width/height
    mbox[8] = 8;
    mbox[9] = 8;
    mbox[10] = width;
    mbox[11] = height;

    mbox[12] = 0x48005; // set depth
    mbox[13] = 4;
    mbox[14] = 4;
    mbox[15] = 32;

    mbox[16] = 0x40008; // pitch
    mbox[17] = 4;
    mbox[18] = 0;
    mbox[19] = 0;

    
    mbox[20] = 0x40001;
    mbox[21] = 8;
    mbox[22] = 8;
    mbox[23] = 16;
    mbox[24] = 0;

    mbox[25] = 0;

    if (mailbox_call(8)){
        fb = (unsigned int*)((unsigned long)(mbox[23] & 0x3FFFFFFF));
        pitch = mbox[19];
    }
}

void fb_clear(unsigned int color){
    for (unsigned int y = 0; y < height; y++){
        for (unsigned int x = 0; x < width; x++){
            fb_draw_pixel(x, y, color);
        }
    }
}

void fb_draw_pixel(int x, int y, unsigned int color){
    if (x < 0 || y < 0 || x >= (int)width || y >= (int)height){
        return;
    }

    unsigned int *ptr = (unsigned int*)((unsigned char*)fb + y * pitch);
    ptr[x] = color;
//    fb[y * (pitch / 4) + x] = color;
}

void fb_draw_rect(int x, int y, int w, int h, unsigned int color){
    for (int j = 0; j < h; j++){
        for (int i = 0; i < w; i++){
            fb_draw_pixel(x + i, y + j, color);
        }
    }
}

unsigned int fb_get_width(){
    return width;
}

unsigned int fb_get_height(){
    return height;
}
