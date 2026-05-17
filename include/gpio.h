#ifndef GPIO_H
#define GPIO_H

#define GPIO_PULL_NONE 0u
#define GPIO_PULL_DOWN 1u
#define GPIO_PULL_UP   2u

void gpio_init_sd(void);
void gpio_init_emmc(void);
void gpio_init_wifi_sdio(void);
void gpio_disconnect_wifi_sdio(void);
void gpio_wifi_wl_on_pulse(void);

void gpio_set_input(unsigned int pin);
void gpio_set_output(unsigned int pin);
void gpio_set_pull(unsigned int pin, unsigned int pull);
int gpio_read(unsigned int pin);
void gpio_write(unsigned int pin, int value);

#endif
