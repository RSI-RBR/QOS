#include "klog.h"
#include "spinlock.h"
#include "terminal.h"
#include "uart.h"

#define KLOG_BUF_SIZE 8192u
#define KLOG_DUMP_CHUNK 512u

static char g_klog_buf[KLOG_BUF_SIZE];
static char g_klog_snapshot[KLOG_BUF_SIZE + 1u];
static unsigned int g_klog_head = 0u;
static unsigned int g_klog_len = 0u;
static spinlock_t g_klog_lock;

static void klog_append_unlocked(char c){
    g_klog_buf[g_klog_head] = c;
    g_klog_head++;
    if (g_klog_head >= KLOG_BUF_SIZE){
        g_klog_head = 0u;
    }
    if (g_klog_len < KLOG_BUF_SIZE){
        g_klog_len++;
    }
}

static void klog_append(const char* s){
    if (!s || !spin_trylock(&g_klog_lock)){
        return;
    }
    while (*s){
        klog_append_unlocked(*s++);
    }
    spin_unlock(&g_klog_lock);
}

static unsigned long klog_cstr_len(const char* s){
    unsigned long n = 0u;
    if (!s){
        return 0u;
    }
    while (s[n]){
        n++;
    }
    return n;
}

static void klog_term_puts(int pid, const char* s){
    terminal_write_for_pid(pid, s, klog_cstr_len(s));
}

void klog_set_terminal_ready(int ready){
    (void)ready;
}

void klog_send(char c){
    uart_send(c);
    if (spin_trylock(&g_klog_lock)){
        klog_append_unlocked(c);
        spin_unlock(&g_klog_lock);
    }
}

void klog_puts(const char* s){
    if (!s){
        return;
    }
    uart_puts(s);
    klog_append(s);
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
        klog_send('0');
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

void klog_dump_to_terminal_for_pid(int pid){
    unsigned int n = 0u;
    unsigned int start = 0u;

    if (pid < 0){
        return;
    }

    if (!spin_trylock(&g_klog_lock)){
        klog_term_puts(pid, "security log busy\n");
        return;
    }

    if (g_klog_len == 0u){
        spin_unlock(&g_klog_lock);
        klog_term_puts(pid, "security log empty\n");
        return;
    }

    start = (g_klog_head + KLOG_BUF_SIZE - g_klog_len) % KLOG_BUF_SIZE;
    while (n < g_klog_len && n < KLOG_BUF_SIZE){
        g_klog_snapshot[n] = g_klog_buf[(start + n) % KLOG_BUF_SIZE];
        n++;
    }
    g_klog_snapshot[n] = 0;
    spin_unlock(&g_klog_lock);

    klog_term_puts(pid, "---- security log ----\n");
    for (unsigned int off = 0u; off < n; off += KLOG_DUMP_CHUNK){
        unsigned int chunk = n - off;
        if (chunk > KLOG_DUMP_CHUNK){
            chunk = KLOG_DUMP_CHUNK;
        }
        terminal_write_for_pid(pid, &g_klog_snapshot[off], chunk);
    }
    if (n > 0u && g_klog_snapshot[n - 1u] != '\n'){
        terminal_write_for_pid(pid, "\n", 1u);
    }
    klog_term_puts(pid, "---- end security log ----\n");
}
