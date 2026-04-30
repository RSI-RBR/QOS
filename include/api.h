#ifndef API_H
#define API_H

#include "uart.h"
#include "memory.h"
#include "framebuffer.h"
#include "timer.h"

typedef struct {
    void (*puts)(const char*);
    void (*putc)(char);

    void* (*malloc)(unsigned long);
    void  (*free)(void*, long unsigned int);

    // Graphics
//    void (*draw_pixel)(int x, int y, unsigned int color);
//    void (*draw_rect)(int x, int y, int w, int h, unsigned int color);
    void (*edit_buffer_pixel)(unsigned int x, unsigned int y, unsigned int colour);
    void (*edit_buffer_rect)(unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int colour);
    void (*update_buffer_pixels)(void);

    void (*clear)(unsigned int color);

    void (*sleep)(unsigned int ms);

    void (*exit) (int code);
    
} kernel_api_t;

extern kernel_api_t kapi;

//extern volatile int program_should_exit;

#endif
