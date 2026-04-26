#include "api.h"

void program_main(kernel_api_t *api){
    api->puts("Hello from external program!\n");

    api->draw_rect(200, 200, 200, 100, 0x00FF0000);

    void *mem = api->malloc(256);

    if (mem){
        api->puts("Allocated memory\n");
        api->free(mem);
    }
    api->puts("Program main returning...\n");
    return;
}
