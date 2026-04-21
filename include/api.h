#ifndef API_H
#define API_H

typedef struct{
    void (*puts)(const char *);
    void (*putc)(char);

} kernel_api_t;

extern kernel_api_t kapi;

#endif
