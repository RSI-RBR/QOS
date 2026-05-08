#include "panic.h"
#include "uart.h"
#include "cpu.h"

// Freestanding stack protector hook expected by GCC.
unsigned long __stack_chk_guard = 0x9E3779B185EBCA87UL;

static int g_canary_seeded = 0;

static unsigned long mix64(unsigned long x){
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdUL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53UL;
    x ^= x >> 33;
    return x;
}

void qos_stack_canary_init(void){
    if (g_canary_seeded){
        return;
    }

    unsigned long cntpct = 0;
    unsigned long cntfrq = 0;
    unsigned long sp = 0;
    asm volatile("mrs %0, cntpct_el0" : "=r"(cntpct));
    asm volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
    asm volatile("mov %0, sp" : "=r"(sp));

    unsigned long seed = cntpct ^ (cntfrq << 7) ^ (sp << 13) ^ 0xD1B54A32D192ED03UL;
    seed = mix64(seed);
    if (seed == 0u){
        seed = 0xA5A5A5A5F00DF00DUL;
    }
    __stack_chk_guard = seed;
    g_canary_seeded = 1;
}

__attribute__((noreturn, no_stack_protector))
void qos_panic(const char* reason, const char* file, unsigned int line){
    asm volatile("msr daifset, #2" : : : "memory");

    uart_puts("\nPANIC\n");
    uart_puts("CORE=");
    uart_putdec((unsigned long)cpu_get_id());
    uart_puts(" REASON=");
    if (reason){
        uart_puts(reason);
    } else{
        uart_puts("(null)");
    }
    uart_puts("\nAT ");
    if (file){
        uart_puts(file);
    } else{
        uart_puts("(unknown)");
    }
    uart_puts(":");
    uart_putdec((unsigned long)line);
    uart_puts("\nHALTING\n");

    while (1){
        asm volatile("wfi");
    }
}

__attribute__((noreturn, no_stack_protector))
void __stack_chk_fail(void){
    qos_panic("stack canary mismatch", __FILE__, __LINE__);
}

__attribute__((noreturn, no_stack_protector))
void __stack_chk_fail_local(void){
    __stack_chk_fail();
}
