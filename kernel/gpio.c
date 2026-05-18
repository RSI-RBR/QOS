#include "gpio.h"
#include "platform/mmio.h"
#include "uart.h"
#include "mailbox.h"

#define GPIO_BASE QOS_GPIO_BASE

#define GPFSEL4 ((volatile unsigned int*)(GPIO_BASE + 0x10))
#define GPFSEL5 ((volatile unsigned int*)(GPIO_BASE + 0x14))

#define GPSET0 ((volatile unsigned int*)(GPIO_BASE + 0x1C))
#define GPSET1 ((volatile unsigned int*)(GPIO_BASE + 0x20))
#define GPCLR0 ((volatile unsigned int*)(GPIO_BASE + 0x28))
#define GPCLR1 ((volatile unsigned int*)(GPIO_BASE + 0x2C))
#define GPLEV0 ((volatile unsigned int*)(GPIO_BASE + 0x34))
#define GPLEV1 ((volatile unsigned int*)(GPIO_BASE + 0x38))

#define GPPUD     ((volatile unsigned int*)(GPIO_BASE + 0x94))
#define GPPUDCLK0 ((volatile unsigned int*)(GPIO_BASE + 0x98))
#define GPPUDCLK1 ((volatile unsigned int*)(GPIO_BASE + 0x9C))

static void delay(int count) {
    while (count--) asm volatile("nop");
}

static volatile unsigned int* gpio_fsel_reg(unsigned int pin){
    if (pin < 10u){
        return (volatile unsigned int*)(GPIO_BASE + 0x00);
    }
    if (pin < 20u){
        return (volatile unsigned int*)(GPIO_BASE + 0x04);
    }
    if (pin < 30u){
        return (volatile unsigned int*)(GPIO_BASE + 0x08);
    }
    if (pin < 40u){
        return (volatile unsigned int*)(GPIO_BASE + 0x0C);
    }
    if (pin < 50u){
        return (volatile unsigned int*)(GPIO_BASE + 0x10);
    }
    if (pin < 60u){
        return (volatile unsigned int*)(GPIO_BASE + 0x14);
    }
    return 0;
}

static void gpio_set_func(unsigned int pin, unsigned int func) {
    volatile unsigned int *fsel;
    unsigned int shift;

    fsel = gpio_fsel_reg(pin);
    if (!fsel){
        return;
    }

    shift = (pin % 10) * 3;

    unsigned int val = *fsel;
    val &= ~(7u << shift);
    val |= ((func & 7u) << shift);
    *fsel = val;
}

static void gpio_set_alt(unsigned int pin, unsigned int alt) {
    gpio_set_func(pin, alt & 7u);
}

void gpio_set_input(unsigned int pin){
    gpio_set_func(pin, 0u);
}

void gpio_set_output(unsigned int pin){
    gpio_set_func(pin, 1u);
}

void gpio_set_pull(unsigned int pin, unsigned int pull){
    volatile unsigned int* clk = 0;
    unsigned int bit = 0;

    if (pin >= 54u){
        return;
    }

    *GPPUD = (pull & 0x3u);
    delay(150);

    if (pin < 32u){
        clk = GPPUDCLK0;
        bit = pin;
    } else{
        clk = GPPUDCLK1;
        bit = pin - 32u;
    }

    *clk = (1u << bit);
    delay(150);
    *clk = 0u;
}

int gpio_read(unsigned int pin){
    volatile unsigned int* lev = 0;
    unsigned int bit = 0;

    if (pin >= 54u){
        return 0;
    }
    if (pin < 32u){
        lev = GPLEV0;
        bit = pin;
    } else{
        lev = GPLEV1;
        bit = pin - 32u;
    }
    return ((*lev & (1u << bit)) != 0u) ? 1 : 0;
}

void gpio_write(unsigned int pin, int value){
    volatile unsigned int* reg = 0;
    unsigned int bit = 0;

    if (pin >= 54u){
        return;
    }
    if (pin < 32u){
        reg = value ? GPSET0 : GPCLR0;
        bit = pin;
    } else{
        reg = value ? GPSET1 : GPCLR1;
        bit = pin - 32u;
    }
    *reg = (1u << bit);
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
    gpio_set_pull(48u, GPIO_PULL_NONE);
    gpio_set_pull(49u, GPIO_PULL_UP);
    gpio_set_pull(50u, GPIO_PULL_UP);
    gpio_set_pull(51u, GPIO_PULL_UP);
    gpio_set_pull(52u, GPIO_PULL_UP);
    gpio_set_pull(53u, GPIO_PULL_UP);

    

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
    gpio_set_pull(49u, GPIO_PULL_UP);
    gpio_set_pull(50u, GPIO_PULL_UP);
    gpio_set_pull(51u, GPIO_PULL_UP);
    gpio_set_pull(52u, GPIO_PULL_UP);
    gpio_set_pull(53u, GPIO_PULL_UP);

    uart_puts("GPIO: EMMC pins configured\n");
}

void gpio_disconnect_wifi_sdio(void){
    uart_puts("GPIO: disconnecting WiFi SDIO pins\n");

    for (unsigned int pin = 34; pin <= 39; pin++){
        gpio_set_alt(pin, 0); // input
    }

    gpio_set_pull(34u, GPIO_PULL_NONE);
    gpio_set_pull(35u, GPIO_PULL_NONE);
    gpio_set_pull(36u, GPIO_PULL_NONE);
    gpio_set_pull(37u, GPIO_PULL_NONE);
    gpio_set_pull(38u, GPIO_PULL_NONE);
    gpio_set_pull(39u, GPIO_PULL_NONE);
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
    gpio_set_pull(34u, GPIO_PULL_NONE);
    gpio_set_pull(35u, GPIO_PULL_UP);
    gpio_set_pull(36u, GPIO_PULL_UP);
    gpio_set_pull(37u, GPIO_PULL_UP);
    gpio_set_pull(38u, GPIO_PULL_UP);
    gpio_set_pull(39u, GPIO_PULL_UP);

    uart_puts("GPIO: WiFi SDIO pins configured\n");
}

void gpio_wifi_wl_on_set(int on){
    static const unsigned int wl_on_candidates[] = {129u, 1u};

    for (unsigned int i = 0; i < (sizeof(wl_on_candidates) / sizeof(wl_on_candidates[0])); i++){
        unsigned int pin = wl_on_candidates[i];
        uart_puts("GPIO: WL_ON mailbox pin ");
        uart_putdec(pin);
        uart_puts(on ? " high\n" : " low\n");
        if (mailbox_set_gpio_state(pin, on ? 1 : 0) != 0){
            uart_puts("GPIO: WL_ON set failed\n");
        }
    }
}

void gpio_wifi_wl_on_pulse(void){
    uart_puts("GPIO: WiFi WL_ON pulse\n");
    gpio_wifi_wl_on_set(0);
    delay(5000000);
    gpio_wifi_wl_on_set(1);
    delay(50000000);
}

void gpio_wifi_wl_on_hard_reset(void){
    uart_puts("GPIO: WiFi WL_ON hard reset\n");
    gpio_wifi_wl_on_set(0);
    /*
     * Second firmware-profile loads are more fragile than cold boot because
     * the chip can still be internally alive. Hold WL_ON low long enough for a
     * full CYW43 core reset, then give the PMU/SDIO core time to reappear.
     */
    delay(50000000);
    gpio_wifi_wl_on_set(1);
    delay(120000000);
}
