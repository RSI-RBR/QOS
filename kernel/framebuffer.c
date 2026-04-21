#include "framebuffer.h"
#include "mailbox.h"

static unsigned int width = 1024;
static unsigned int height = 768;
static unsigned int *fb;

static volatile unsigned int mbox[36] __attribute__((aligned(16)));

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

    mbox[16] = 0x40001; // allocate framebuffer
    mbox[17] = 8;
    mbox[18] = 8;
    mbox[19] = 16;
    mbox[20] = 0;

    mbox[21] = 0;

    if (mailbox_call(8)){
        fb = (unsigned int*)((unsigned long)mbox[19]);
    }
}

void fb_clear(unsigned int color){
    for (unsigned int y = 0; y < height; y++){
        for (unsigned int x = 0; x < width; x++){
            fb[y * width + x] = color;
        }
    }
}
