#ifndef PROGRAM_H
#define PROGRAM_H

#include "api.h"

typedef void (*program_entry_t)(kernel_api_t *api);

typedef struct {
    const char *name;
    program_entry_t entry;
} program_t;

#endif
