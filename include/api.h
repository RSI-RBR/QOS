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
    void (*draw_pixel)(int x, int y, unsigned int color);
    void (*draw_rect)(int x, int y, int w, int h, unsigned int color);
    void (*clear)(unsigned int color);

    void (*sleep)(unsigned int ms);

    void (*exit) (int code);
    
} kernel_api_t;

extern kernel_api_t kapi;

//extern volatile int program_should_exit;

#endif
