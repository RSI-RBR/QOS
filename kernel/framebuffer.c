#include "framebuffer.h"
#include "mailbox.h"
#include "uart.h"
#include "spinlock.h"

#define MAX_WIDTH 1920
#define MAX_HEIGHT 1080
#define MAX_TRACKED_PIXELS (MAX_WIDTH * MAX_HEIGHT)
#define FB_MAX_REQUESTED_PAGES 4u
#define FB_LEGACY_SAFE_VIRTUAL_HEIGHT 4096u

static unsigned int width = 1920;
static unsigned int height = 1080;
static unsigned int virtual_height = 1080;
static unsigned int pitch;
static unsigned int *fb;
static unsigned long fb_bus;
static unsigned long fb_size;
static unsigned int fb_page_count = 1;
static unsigned int fb_display_page = 0;
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
    unsigned int requested_height = height;
    unsigned int requested_pages = FB_MAX_REQUESTED_PAGES;
    /*
     * Pi 3 legacy firmware commonly clamps oversized virtual dimensions. At
     * 1080p, four pages require virtual_height=4320, so request three pages
     * instead of letting firmware silently fall back to two.
     */
    while (requested_pages > 2u &&
           requested_height > 0u &&
           requested_height * requested_pages > FB_LEGACY_SAFE_VIRTUAL_HEIGHT){
        requested_pages--;
    }
    unsigned int requested_virtual_height = requested_height * requested_pages;

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
    mbox[11] = requested_virtual_height;

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
        // The firmware returns a GPU/DMA bus address. Keep it raw for DMA
        // destinations; only mask to an ARM physical address for CPU access.
        fb_bus = (unsigned long)mbox[23];
        fb = (unsigned int*)((unsigned long)(fb_bus & 0x3FFFFFFF));
        pitch = mbox[19];
        fb_size = (unsigned long)mbox[24];
        // Use actual dimensions returned by firmware, not only requested values.
        if (mbox[5] > 0 && mbox[6] > 0){
            width = mbox[5];
            height = mbox[6];
        }
        virtual_height = mbox[11] ? mbox[11] : height;
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
        fb_page_count = 1u;
        if (pitch > 0u && height > 0u && virtual_height >= height){
            unsigned long page_bytes = (unsigned long)pitch * (unsigned long)height;
            unsigned int pages_by_height = virtual_height / height;
            unsigned int pages_by_size = (unsigned int)(fb_size / page_bytes);
            unsigned int pages = pages_by_height < pages_by_size ? pages_by_height : pages_by_size;
            if (pages > requested_pages){
                pages = requested_pages;
            }
            if (pages > 0u){
                fb_page_count = pages;
            }
        }
        fb_display_page = 0u;
        uart_puts("FB active width=");
        uart_puthex(width);
        uart_puts(" height=");
        uart_puthex(height);
        uart_puts(" pitch=");
        uart_puthex(pitch);
        uart_puts(" pages=");
        uart_puthex(fb_page_count);
        uart_puts(" base=");
        uart_puthex((unsigned int)(unsigned long)fb);
        uart_puts(" page1=");
        uart_puthex((unsigned int)fb_get_page_base(1u));
        uart_puts(" page2=");
        uart_puthex((unsigned int)fb_get_page_base(2u));
        uart_puts(" page3=");
        uart_puthex((unsigned int)fb_get_page_base(3u));
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

unsigned long fb_get_bus_base(){
    return fb_bus;
}

unsigned long fb_get_total_size(void){
    if (pitch == 0u || height == 0u || fb_page_count == 0u){
        return 0UL;
    }
    return (unsigned long)pitch *
           (unsigned long)height *
           (unsigned long)fb_page_count;
}

unsigned int fb_get_page_count(void){
    return fb_page_count;
}

unsigned int fb_get_display_page(void){
    return fb_display_page;
}

unsigned long fb_get_page_base(unsigned int page){
    if (!fb || pitch == 0u || height == 0u || page >= fb_page_count){
        return 0UL;
    }
    return ((unsigned long)fb) + ((unsigned long)page * (unsigned long)pitch * (unsigned long)height);
}

unsigned long fb_get_page_bus_base(unsigned int page){
    if (fb_bus == 0UL || pitch == 0u || height == 0u || page >= fb_page_count){
        return 0UL;
    }
    return fb_bus + ((unsigned long)page * (unsigned long)pitch * (unsigned long)height);
}

int fb_wait_vsync(void){
    unsigned long irq = spin_lock_irqsave(&g_fb_lock);
    mbox[0] = 8 * 4;
    mbox[1] = 0;
    mbox[2] = 0x0004800E; // wait for vertical sync
    mbox[3] = 4;
    mbox[4] = 4;
    mbox[5] = 0;          // display 0
    mbox[6] = 0;
    mbox[7] = 0;

    int ok = mailbox_call(8);
    spin_unlock_irqrestore(&g_fb_lock, irq);
    return ok ? 0 : -1;
}

int fb_set_display_page(unsigned int page){
    if (page >= fb_page_count || pitch == 0u || height == 0u){
        return -1;
    }

    unsigned long irq = spin_lock_irqsave(&g_fb_lock);
    mbox[0] = 8 * 4;
    mbox[1] = 0;
    mbox[2] = 0x00048009; // set virtual framebuffer offset
    mbox[3] = 8;
    mbox[4] = 8;
    mbox[5] = 0;
    mbox[6] = page * height;
    mbox[7] = 0;

    int ok = mailbox_call(8);
    if (ok){
        fb_display_page = page;
    }
    spin_unlock_irqrestore(&g_fb_lock, irq);
    return ok ? 0 : -1;
}
