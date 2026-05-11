#include "gpio.h"
#include "platform/mmio.h"
#include "uart.h"
#include "mailbox.h"

#define GPIO_BASE QOS_GPIO_BASE

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

void gpio_init_emmc(void) {
    uart_puts("GPIO: configuring EMMC pins\n");

    gpio_disconnect_wifi_sdio();

    // EMMC/SDHCI uses ALT3 on GPIO48..53 (shared pins with SDHOST ALT0).
    gpio_set_alt(48, 7); // CLK
    gpio_set_alt(49, 7); // CMD
    gpio_set_alt(50, 7); // DAT0
    gpio_set_alt(51, 7); // DAT1
    gpio_set_alt(52, 7); // DAT2
    gpio_set_alt(53, 7); // DAT3

    // Pull up CMD/DAT lines; keep CLK without pull.
    *GPPUD = 2;
    delay(150);
    *GPPUDCLK1 = (1 << (49 - 32)) |
                 (1 << (50 - 32)) |
                 (1 << (51 - 32)) |
                 (1 << (52 - 32)) |
                 (1 << (53 - 32));
    delay(150);
    *GPPUDCLK1 = 0;

    uart_puts("GPIO: EMMC pins configured\n");
}

void gpio_disconnect_wifi_sdio(void){
    uart_puts("GPIO: disconnecting WiFi SDIO pins\n");

    for (unsigned int pin = 34; pin <= 39; pin++){
        gpio_set_alt(pin, 0); // input
    }

    *GPPUD = 0;
    delay(150);
    *GPPUDCLK1 = (1u << (34 - 32)) |
                 (1u << (35 - 32)) |
                 (1u << (36 - 32)) |
                 (1u << (37 - 32)) |
                 (1u << (38 - 32)) |
                 (1u << (39 - 32));
    delay(150);
    *GPPUDCLK1 = 0;
}

void gpio_init_wifi_sdio(void){
    uart_puts("GPIO: configuring WiFi SDIO pins\n");

    // BCM4343x on Pi 3/Zero 2W uses SD1 on GPIO 34..39 (ALT3).
    gpio_set_alt(34, 7); // SD1_CLK
    gpio_set_alt(35, 7); // SD1_CMD
    gpio_set_alt(36, 7); // SD1_DAT0
    gpio_set_alt(37, 7); // SD1_DAT1
    gpio_set_alt(38, 7); // SD1_DAT2
    gpio_set_alt(39, 7); // SD1_DAT3
    gpio_set_alt(43, 4); // GPCLK2 for WiFi low-power clock (Linux DT uses this)

    // Pull scheme used by Raspberry Pi Linux DT overlays:
    // CLK no pull, CMD/DAT pull-up.
    *GPPUD = 0;
    delay(150);
    *GPPUDCLK1 = (1u << (34 - 32));
    delay(150);
    *GPPUDCLK1 = 0;

    *GPPUD = 2;
    delay(150);
    *GPPUDCLK1 = (1u << (35 - 32)) |
                 (1u << (36 - 32)) |
                 (1u << (37 - 32)) |
                 (1u << (38 - 32)) |
                 (1u << (39 - 32));
    delay(150);
    *GPPUDCLK1 = 0;

    uart_puts("GPIO: WiFi SDIO pins configured\n");
}

void gpio_wifi_wl_on_pulse(void){
    static const unsigned int wl_on_candidates[] = {129u, 1u};

    uart_puts("GPIO: WiFi WL_ON pulse\n");
    for (unsigned int i = 0; i < (sizeof(wl_on_candidates) / sizeof(wl_on_candidates[0])); i++){
        unsigned int pin = wl_on_candidates[i];
        uart_puts("GPIO: WL_ON mailbox pin ");
        uart_putdec(pin);
        uart_puts("\n");
        if (mailbox_set_gpio_state(pin, 0) != 0){
            uart_puts("GPIO: WL_ON low failed\n");
        }
        delay(5000000);
        if (mailbox_set_gpio_state(pin, 1) != 0){
            uart_puts("GPIO: WL_ON high failed\n");
        }
        delay(50000000);
    }
}
