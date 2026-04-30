#include "api.h"
#include "process.h"


//static void api_draw_pixel(int x, int y, unsigned int color){
//    fb_draw_pixel(x, y, color);
//}

static void api_edit_buffer_pixel(unsigned int x, unsigned int y, unsigned int colour){
    fb_edit_buffer_pixel_fast(x, y, colour);
}

static void api_edit_buffer_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int colour){
    fb_edit_buffer_rect_fast(x, y, w, h, colour);
}

static void api_update_buffer_pixels(void){
    fb_update_buffer_pixels_fast();
}

//static void api_draw_rect(int x , int y, int w, int h, unsigned int color){
//    fb_draw_rect(x, y, w, h, color);
//}

static void api_clear(unsigned int color){
    fb_clear(color);
}

static unsigned int api_get_screen_width(void){
    return fb_get_width();
}

static unsigned int api_get_screen_height(void){
    return fb_get_height();
}

static void api_sleep(unsigned int ms){
    process_sleep(ms);
}

//volatile int program_should_exit = 0;

void kexit(int code){
    uart_puts("User called exit.\n");

//    program_should_exit = 1;

//    while(1){}
    return;
}

kernel_api_t kapi = {
    .puts = uart_puts,
    .putc = uart_send,
    
    .malloc = kmalloc,
    .free = kfree_secure,

//    .draw_pixel = api_draw_pixel,
//    .draw_rect = api_draw_rect,
    .edit_buffer_pixel = api_edit_buffer_pixel,
    .edit_buffer_rect = api_edit_buffer_rect,
    .update_buffer_pixels = api_update_buffer_pixels,
    .clear = api_clear,
    .get_screen_width = api_get_screen_width,
    .get_screen_height = api_get_screen_height,

    .sleep = api_sleep,

    .exit = kexit
};
