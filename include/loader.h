#ifndef LOADER_H
#define LOADER_H

#include "api.h"

typedef void (*program_entry_t)(kernel_api_t *);

program_entry_t load_program_from_sd(void);

#endif
