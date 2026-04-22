#include "clock.h"
#include "uart.h"

#define CM_BASE 0x3F101000

#define CM_EMMCCTL (*(volatile unsigned int*)(CM_BASE + 0x1C))
#define CM_EMMCDIV (*(volatile unsigned int*)(CM_BASE + 0x20))

#define CM_PASSWORD 0x5A000000

static void delay(int count) {
    while (count--) asm volatile("nop");
}

void clock_init_emmc(void) {
    uart_puts("CLOCK: init EMMC\n");

    // 1. Stop clock (safe disable)
    CM_EMMCCTL = CM_PASSWORD | (1 << 5); // kill

    delay(2000);

    // Wait until not busy
    while (CM_EMMCCTL & (1 << 7));

    uart_puts("CLOCK: stopped\n");

    // 2. Set clock divisor
    // Base clock ~250 MHz → divide to ~400kHz for init
    // divisor = 250MHz / 400kHz ≈ 625 → use 650 for safety
    unsigned int divisor = 650;

    CM_EMMCDIV = CM_PASSWORD | (divisor << 12);

    uart_puts("CLOCK: divisor set\n");

    // 3. Enable clock (PLLD = source 6)
    CM_EMMCCTL = CM_PASSWORD | (6) | (1 << 4); // enable

    delay(2000);

    // 4. Wait for clock to be running
    while (!(CM_EMMCCTL & (1 << 7)));

    uart_puts("CLOCK: running\n");
}
