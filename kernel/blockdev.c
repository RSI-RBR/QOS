#include "blockdev.h"
#include "emmc.h"
#include "sdhost.h"
#include "gpio.h"
#include "uart.h"

enum {
    BACKEND_NONE = 0,
    BACKEND_EMMC = 1,
    BACKEND_SDHOST = 2
};

static int g_backend = BACKEND_NONE;

const char* blockdev_name(void){
    if (g_backend == BACKEND_EMMC) return "emmc";
    if (g_backend == BACKEND_SDHOST) return "sdhost";
    return "none";
}

int blockdev_init(void){
    gpio_init_emmc();
    if (emmc_init() == 0){
        g_backend = BACKEND_EMMC;
        uart_puts("Blockdev: EMMC active\n");
        return 0;
    }

    gpio_init_sd();
    sdhost_reset();
    if (sdhost_init_card() == 0){
        g_backend = BACKEND_SDHOST;
        uart_puts("Blockdev: SDHOST active\n");
        return 0;
    }

    g_backend = BACKEND_NONE;
    uart_puts("Blockdev: init failed\n");
    return -1;
}

int blockdev_reinit(void){
    if (g_backend == BACKEND_EMMC){
        gpio_init_emmc();
        return emmc_init();
    }
    if (g_backend == BACKEND_SDHOST){
        gpio_init_sd();
        sdhost_reset();
        return sdhost_init_card();
    }
    return blockdev_init();
}

int blockdev_read_block(unsigned int lba, unsigned char *buffer){
    if (g_backend == BACKEND_EMMC){
        return emmc_read_block(lba, buffer);
    }
    if (g_backend == BACKEND_SDHOST){
        return sdhost_read_block(lba, buffer);
    }
    return -1;
}
