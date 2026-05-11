#include "clock.h"
#include "platform/mmio.h"
#include "uart.h"

#define CM_BASE QOS_CLOCK_BASE

#define CM_EMMCCTL (*(volatile unsigned int*)(CM_BASE + 0x1C))
#define CM_EMMCDIV (*(volatile unsigned int*)(CM_BASE + 0x20))
#define CM_GP2CTL  (*(volatile unsigned int*)(CM_BASE + 0x80))
#define CM_GP2DIV  (*(volatile unsigned int*)(CM_BASE + 0x84))

#define CM_PASSWORD 0x5A000000

#define CM_CTL_ENAB (1 << 4)
#define CM_CTL_KILL (1 << 5)
#define CM_CTL_BUSY (1 << 7)
#define CM_CTL_MASH_1 (1 << 9)
#define CM_SRC_OSC 1

void clock_debug_write(void)
{
    volatile unsigned int *test = (volatile unsigned int*)QOS_CLOCK_BASE;

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

void clock_init_wifi_lpo(void){
    uart_puts("CLOCK: init WiFi LPO\n");

    // GPIO43/GPCLK2 feeds the CYW4343x EXT_SLEEP_CLK on WiFi Pis.
    // 19.2 MHz oscillator / 585.9375 = 32768 Hz.
    CM_GP2CTL = CM_PASSWORD | CM_CTL_KILL;
    mmio_barrier();
    delay(5000);

    int timeout = 1000000;
    while ((CM_GP2CTL & CM_CTL_BUSY) && timeout--) {}
    if (timeout <= 0){
        uart_puts("CLOCK: GP2 stop timeout\n");
        return;
    }

    CM_GP2DIV = CM_PASSWORD | (585u << 12) | 3840u;
    mmio_barrier();
    CM_GP2CTL = CM_PASSWORD | CM_CTL_MASH_1 | CM_SRC_OSC;
    mmio_barrier();
    delay(5000);
    CM_GP2CTL = CM_PASSWORD | CM_CTL_MASH_1 | CM_CTL_ENAB | CM_SRC_OSC;
    mmio_barrier();
    delay(5000);

    timeout = 1000000;
    while (!(CM_GP2CTL & CM_CTL_BUSY) && timeout--) {}
    if (timeout <= 0){
        uart_puts("CLOCK: GP2 start timeout\n");
        return;
    }

    uart_puts("CLOCK: WiFi LPO running\n");
}
