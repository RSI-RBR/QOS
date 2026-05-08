#include "klog.h"
#include "process.h"
#include "string.h"
#include "terminal.h"
#include "uart.h"

static int g_klog_terminal_ready = 0;

void klog_set_terminal_ready(int ready){
    g_klog_terminal_ready = ready ? 1 : 0;
}

void klog_send(char c){
    if (!g_klog_terminal_ready){
        uart_send(c);
        return;
    }

    char s[2];
    s[0] = c;
    s[1] = 0;
    klog_puts(s);
}

void klog_puts(const char* s){
    if (!s){
        return;
    }

    if (!g_klog_terminal_ready){
        uart_puts(s);
        return;
    }

    int len = kstrlen(s);
    int pid = process_current_pid();
    if (pid >= 0){
        terminal_write_for_pid(pid, s, (unsigned long)len);
    } else{
        terminal_write(terminal_get_active(), -1, s, (unsigned long)len);
    }
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
        klog_puts("0");
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
