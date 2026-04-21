#ifndef API_H
#define API_H

typedef struct {
    void (*puts)(const char*);
    void (*putc)(char);

    void* (*malloc)(unsigned long);
    void  (*free)(void*);
} kernel_api_t;

#endif
