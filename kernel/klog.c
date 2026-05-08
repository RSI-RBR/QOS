#include "klog.h"
#include "uart.h"

void klog_set_terminal_ready(int ready){
    (void)ready;
}

void klog_send(char c){
    uart_send(c);
}

void klog_puts(const char* s){
    if (!s){
        return;
    }
    uart_puts(s);
}

void klog_puthex(unsigned int val){
    static const char hex[] = "0123456789ABCDEF";
    char out[9];
    for (int i = 0; i < 8; i++){
        unsigned int shift = (unsigned int)(28 - (i * 4));
        out[i] = hex[(val >> shift) & 0xFu];
    }
    out[8] = 0;
    klog_puts(out);
}

void klog_putdec(unsigned long val){
    char tmp[21];
    char out[21];
    int n = 0;
    int o = 0;

    if (val == 0UL){
        uart_send('0');
        return;
    }

    while (val > 0UL && n < (int)sizeof(tmp)){
        tmp[n++] = (char)('0' + (val % 10UL));
        val /= 10UL;
    }
    while (n > 0 && o < (int)sizeof(out) - 1){
        out[o++] = tmp[--n];
    }
    out[o] = 0;
    klog_puts(out);
}
