#include "api.h"
#include "uart.h"

kernel_api_t kapi = {
    .puts = uart_puts,
    .putc = uart_send
};
