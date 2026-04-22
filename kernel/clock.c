#include "clock.h"
#include "uart.h"

#define CM_BASE 0x3f101000

#define CM_EMMCCTL (*(volatile unsigned int*)(CM_BASE + 0x1C))
#define CM_EMMCDIV (*(volatile unsigned int*)(CM_BASE + 0x20))

#define CM_PASSWORD 0x5A000000

#define CM_CTL_ENAB (1 << 4)
#define CM_CTL_KILL (1 << 5)
#define CM_CTL_BUSY (1 << 7)

void clock_debug_write(void)
{
    volatile unsigned int *test = (volatile unsigned int*)0x3F101000;

    uart_puts("BEFORE: ");
    uart_puthex(*test);
    uart_puts("\n");

    *test = 0xDEADBEEF;

    uart_puts("AFTER: ");
    uart_puthex(*test);
    uart_puts("\n");
}

static void delay(int count) {
    while (count--) asm volatile("nop");
}

static void mmio_barrier() {
    asm volatile("dsb sy");
}

void clock_init_emmc(void) {
    uart_puts("CLOCK: init EMMC\n");

    // --- STEP 1: Disable clock cleanly ---
    unsigned int ctl = CM_EMMCCTL;

    // Kill + disable (preserve nothing)
    CM_EMMCCTL = CM_PASSWORD | CM_CTL_KILL;
    mmio_barrier();

    delay(5000);

    // Wait for BUSY to clear
    int timeout = 1000000;
    while ((CM_EMMCCTL & CM_CTL_BUSY) && timeout--) {}

    if (timeout <= 0) {
        uart_puts("CLOCK: failed to stop\n");
        return;
    }

    uart_puts("CLOCK: stopped\n");

    // --- STEP 2: Set divisor ---
    unsigned int divisor = 650;

    CM_EMMCDIV = CM_PASSWORD | (divisor << 12);
    mmio_barrier();

    uart_puts("CLOCK: divisor set\n");

    // --- STEP 3: Set clock source (PLLD = 6), no enable ---
    CM_EMMCCTL = CM_PASSWORD | 6;
    mmio_barrier();

    delay(5000);

    // --- STEP 4: Enable clock ---
    CM_EMMCCTL = CM_PASSWORD | 6 | CM_CTL_ENAB;
    mmio_barrier();

    delay(5000);

    // --- STEP 5: Wait for clock to run ---
    timeout = 1000000;
    while (!(CM_EMMCCTL & CM_CTL_BUSY) && timeout--) {}

    if (timeout <= 0) {
        uart_puts("CLOCK: FAILED TO START\n");

        uart_puts("CTL=");
        uart_puthex(CM_EMMCCTL);
        uart_puts("\n");

        return;
    }

    uart_puts("CLOCK: running\n");
}
