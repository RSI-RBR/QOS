#include "api.h"
#include "uart.h"
#include "memory.h"

kernel_api_t kapi = {
    .puts = uart_puts,
    .putc = uart_send,
    .malloc = kmalloc,
    .free = kfree
};
