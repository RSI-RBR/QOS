#include "framebuffer.h"
#include "mailbox.h"

#define MAX_WIDTH 1920
#define MAX_HEIGHT 1080

static unsigned int width = 1920;
static unsigned int height = 1080;
static unsigned int pitch;
static unsigned int *fb;
static unsigned char bytes_per_pixel = 4;

static unsigned int back_buffer[MAX_WIDTH][MAX_HEIGHT] __attribute__((aligned(64)));
//static unsigned int front_buffer[width][height] __attribute__((aligned(64)));


static unsigned long modified_pixel_count = 0;
static unsigned int modified_pixel_coords[2][1920*1080];

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

void fb_init_buffers(void){
    for (unsigned long y = 0; y < height; y++){
        for (unsigned long x = 0; x < width; x++){
//            front_buffer[x][y] = 0x00000000;
            back_buffer[x][y] = 0x00000000;
        }
    }
    for (unsigned long i = 0; i < 1920*1080; i++){
        modified_pixel_coords[0][i] = 0;
        modified_pixel_coords[1][i] = 0;
    }
}

void fb_edit_buffer_pixel(unsigned int x, unsigned int y, unsigned int colour){
    if (x >= width || y >= height) return;
    back_buffer[x][y] = colour;

    unsigned char already_modified = 0;
    for (unsigned long i = 0; i < modified_pixel_count; i++){
        if (modified_pixel_coords[0][i] == x && modified_pixel_coords[1][i] == y){
            already_modified = 1;
            break;
        }
    }

    if (!already_modified){
        modified_pixel_coords[0][modified_pixel_count] = x;
        modified_pixel_coords[1][modified_pixel_count] = y;
        modified_pixel_count ++;
    }

    return;
}

void fb_edit_buffer_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int colour){
    for (unsigned int j = 0; j < h; j++){
        for (unsigned int i = 0; i < w; i++){
            fb_edit_buffer_pixel(x + i, y + j, colour);
        }
    }
}

void fb_update_buffer_pixels(void){
    for (unsigned long i = 0; i < modified_pixel_count; i++){
        fb_draw_pixel(modified_pixel_coords[0][i], modified_pixel_coords[1][i], back_buffer[modified_pixel_coords[0][i]][modified_pixel_coords[1][i]]);
    }

    modified_pixel_count = 0;

}

void fb_clear(unsigned int color){
    for (unsigned int y = 0; y < height; y++){
        for (unsigned int x = 0; x < width; x++){
            fb_draw_pixel(x, y, color);
        }
    }
}

void fb_draw_pixel(unsigned int x, unsigned int y, unsigned int color){
    if (x >= width || y >= height){
        return;
    }

    unsigned int *ptr = (unsigned int*)((unsigned char*)fb + y * pitch);
    ptr[x] = color;
//    fb[y * (pitch / 4) + x] = color;
}

void fb_draw_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int color){
    for (unsigned int j = 0; j < h; j++){
        for (unsigned int i = 0; i < w; i++){
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
