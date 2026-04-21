#include <stdint.h>

typedef struct {
    void (*puts)(const char *);
    void (*putc)(char);
} kernel_api_t;

void main(kernel_api_t *api){
    api->puts("Hello from external program");

    while(1);
}
