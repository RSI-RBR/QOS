#include "api.h"
#include "process.h"


static void api_draw_pixel(int x, int y, unsigned int color){
    fb_draw_pixel(x, y, color);
}

static void api_draw_rect(int x , int y, int w, int h, unsigned int color){
    fb_draw_rect(x, y, w, h, color);
}

static void api_clear(unsigned int color){
    fb_clear(color);
}

static void api_sleep(unsigned int ms){
    unsigned long target = system_ticks + ms;

    while (system_ticks < target){
        process_yield();
    }
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

    .draw_pixel = api_draw_pixel,
    .draw_rect = api_draw_rect,
    .clear = api_clear,

    .sleep = api_sleep,

    .exit = kexit
};
