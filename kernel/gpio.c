#include "gpio.h"
#include "uart.h"

#define GPIO_BASE 0x3F200000

#define GPFSEL4 ((volatile unsigned int*)(GPIO_BASE + 0x10))
#define GPFSEL5 ((volatile unsigned int*)(GPIO_BASE + 0x14))

#define GPPUD     ((volatile unsigned int*)(GPIO_BASE + 0x94))
#define GPPUDCLK1 ((volatile unsigned int*)(GPIO_BASE + 0x9C))

static void delay(int count) {
    while (count--) asm volatile("nop");
}

static void gpio_set_alt(unsigned int pin, unsigned int alt) {
    volatile unsigned int *fsel;
    unsigned int shift;

    if (pin < 10) {
        fsel = (volatile unsigned int*)(GPIO_BASE + 0x00);
    } else if (pin < 20) {
        fsel = (volatile unsigned int*)(GPIO_BASE + 0x04);
    } else if (pin < 30) {
        fsel = (volatile unsigned int*)(GPIO_BASE + 0x08);
    } else if (pin < 40) {
        fsel = (volatile unsigned int*)(GPIO_BASE + 0x0C);
    } else if (pin < 50) {
        fsel = (volatile unsigned int*)(GPIO_BASE + 0x10);
    } else {
        fsel = (volatile unsigned int*)(GPIO_BASE + 0x14);
    }

    shift = (pin % 10) * 3;

    unsigned int val = *fsel;
    val &= ~(7 << shift);
    val |= (alt << shift);
    *fsel = val;
}

void gpio_init_sd(void) {
    uart_puts("GPIO: configuring SD pins\n");

    // ALT0 = 100
    gpio_set_alt(48, 4); // SD_CLK
    gpio_set_alt(49, 4); // SD_CMD
    gpio_set_alt(50, 4); // SD_DAT0
    gpio_set_alt(51, 4); // SD_DAT1
    gpio_set_alt(52, 4); // SD_DAT2
    gpio_set_alt(53, 4); // SD_DAT3

    // Disable pull-up/down
    *GPPUD = 0;
    delay(150);

    *GPPUDCLK1 = (1 << (48 - 32)) |
                 (1 << (49 - 32)) |
                 (1 << (50 - 32)) |
                 (1 << (51 - 32)) |
                 (1 << (52 - 32)) |
                 (1 << (53 - 32));

    delay(150);
    *GPPUDCLK1 = 0;

    *GPPUD = 2;
    delay(150);

    *GPPUDCLK1 = (1 << (49 - 32)) |
                 (1 << (50 - 32)) |
                 (1 << (51 - 32)) |
                 (1 << (52 - 32)) |
                 (1 << (53 - 32));

    delay(150);

    *GPPUDCLK1 = 0;

    

    uart_puts("GPIO: SD pins configured\n");
}
