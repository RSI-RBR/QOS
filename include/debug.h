#include "uart.h"

#define barrier() asm volatile("dmb sy" ::: "memory")

void check_stack(void);
