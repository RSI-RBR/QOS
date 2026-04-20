#include "program.h"

static void prog_hello(kernel_api_t *api){
    api->puts("Hello program!\n");
}

static void prog_test(kernel_api_t *api){
    api->puts("Test program running\n");
    api->puts("Line 2\n");
}

program_t programs[] = {
    {"hello", prog_hello},
    {"test",  prog_test},
};

int program_count = sizeof(programs)/sizeof(programs[0]);
