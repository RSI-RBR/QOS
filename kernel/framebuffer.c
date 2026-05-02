#include "framebuffer.h"
#include "mailbox.h"
#include "uart.h"
#include "spinlock.h"

#define MAX_WIDTH 1920
#define MAX_HEIGHT 1080
#define MAX_TRACKED_PIXELS (MAX_WIDTH * MAX_HEIGHT)

static unsigned int width = 1920;
static unsigned int height = 1080;
static unsigned int pitch;
static unsigned int *fb;
//static unsigned char bytes_per_pixel = 4;

static unsigned char dirty_map[MAX_HEIGHT][MAX_WIDTH];
static unsigned int back_buffer[MAX_HEIGHT][MAX_WIDTH] __attribute__((aligned(64)));
//static unsigned int front_buffer[width][height] __attribute__((aligned(64)));


static unsigned long modified_pixel_count = 0;
static unsigned int modified_pixel_coords[2][MAX_TRACKED_PIXELS];
static spinlock_t g_fb_lock;

//static volatile unsigned int mbox[36] __attribute__((aligned(16)));

void fb_init(){
    spinlock_init(&g_fb_lock);
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
        // Use actual dimensions returned by firmware, not only requested values.
        if (mbox[5] > 0 && mbox[6] > 0){
            width = mbox[5];
            height = mbox[6];
        }
        if (width > MAX_WIDTH || height > MAX_HEIGHT){
            uart_puts("FB dims exceed static buffers; clamping.\n");
            uart_puts("Reported width=");
            uart_puthex(width);
            uart_puts(" height=");
            uart_puthex(height);
            uart_puts("\n");
            if (width > MAX_WIDTH){
                width = MAX_WIDTH;
            }
            if (height > MAX_HEIGHT){
                height = MAX_HEIGHT;
            }
        }
        uart_puts("FB active width=");
        uart_puthex(width);
        uart_puts(" height=");
        uart_puthex(height);
        uart_puts(" pitch=");
        uart_puthex(pitch);
        uart_puts("\n");
    }
}

static inline void fb_draw_pixel_fast(unsigned int x, unsigned int y, unsigned int colour){
    unsigned int *ptr = (unsigned int*)((unsigned char*)fb + y * pitch);
    ptr[x] = colour;
}

void fb_init_buffers(void){
    unsigned long irq = spin_lock_irqsave(&g_fb_lock);
    for (unsigned long y = 0; y < height; y++){
        for (unsigned long x = 0; x < width; x++){
//            front_buffer[x][y] = 0x00000000;
            back_buffer[y][x] = 0x00000000;
        }
    }
    for (unsigned long i = 0; i < MAX_TRACKED_PIXELS; i++){
        modified_pixel_coords[0][i] = 0;
        modified_pixel_coords[1][i] = 0;
    }
    spin_unlock_irqrestore(&g_fb_lock, irq);
}

void fb_edit_buffer_pixel(unsigned int x, unsigned int y, unsigned int colour){
    if (x >= width || y >= height) return;
    back_buffer[y][x] = colour;

    unsigned char already_modified = 0;
//    for (unsigned long i = 0; i < modified_pixel_count; i++){
//        if (modified_pixel_coords[0][i] == x && modified_pixel_coords[1][i] == y){
//            already_modified = 1;
//            break;
//        }
//    }

    if (!already_modified){
        if (modified_pixel_count >= MAX_TRACKED_PIXELS){
            return;
        }
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
        unsigned int x = modified_pixel_coords[0][i];
        unsigned int y = modified_pixel_coords[1][i];
        fb_draw_pixel(x, y, back_buffer[y][x]);
    }

    modified_pixel_count = 0;

}

void fb_edit_buffer_pixel_fast(unsigned int x, unsigned int y, unsigned int colour){
    if (x >= width || y >= height) return;

    back_buffer[y][x] = colour;

    if (!dirty_map[y][x]){
        dirty_map[y][x] = 1;

        if (modified_pixel_count >= MAX_TRACKED_PIXELS){
            return;
        }
        modified_pixel_coords[0][modified_pixel_count] = x;
        modified_pixel_coords[1][modified_pixel_count] = y;
        modified_pixel_count ++;
    }
}

void fb_update_buffer_pixels_fast(void){
    unsigned long irq = spin_lock_irqsave(&g_fb_lock);
    for (unsigned long i = 0; i < modified_pixel_count; i++){
        unsigned int x = modified_pixel_coords[0][i];
        unsigned int y = modified_pixel_coords[1][i];

        if (x >= width || y >= height){
            dirty_map[y < MAX_HEIGHT ? y : 0][x < MAX_WIDTH ? x : 0] = 0;
            continue;
        }
        fb_draw_pixel_fast(x, y, back_buffer[y][x]);

        dirty_map[y][x] = 0;
    }
    modified_pixel_count = 0;
    spin_unlock_irqrestore(&g_fb_lock, irq);
}

void fb_edit_buffer_rect_fast(unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int colour){
    unsigned long irq = spin_lock_irqsave(&g_fb_lock);
    for (unsigned int j = 0; j < h; j++){
        unsigned int py = y + j;
        if (py >= height) break;

        for (unsigned int i = 0; i < w; i++){
            unsigned int px = x + i;
            if (px >= width) break;
            back_buffer[py][px] = colour;

            if (!dirty_map[py][px]){
                dirty_map[py][px] = 1;
                if (modified_pixel_count >= MAX_TRACKED_PIXELS){
                    spin_unlock_irqrestore(&g_fb_lock, irq);
                    return;
                }
                modified_pixel_coords[0][modified_pixel_count] = px;
                modified_pixel_coords[1][modified_pixel_count] = py;
                modified_pixel_count ++;
            }
        }
    }
    spin_unlock_irqrestore(&g_fb_lock, irq);
}

void fb_clear(unsigned int color){
    unsigned long irq = spin_lock_irqsave(&g_fb_lock);
    for (unsigned int y = 0; y < height; y++){
        for (unsigned int x = 0; x < width; x++){
            fb_draw_pixel(x, y, color);
        }
    }
    spin_unlock_irqrestore(&g_fb_lock, irq);
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

unsigned int fb_get_pitch(){
    return pitch;
}

unsigned long fb_get_base(){
    return (unsigned long)fb;
}
