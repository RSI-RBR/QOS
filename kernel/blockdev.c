#include "blockdev.h"
#include "emmc.h"
#include "sdhost.h"
#include "gpio.h"
#include "uart.h"
#include "cyw43.h"

enum {
    BACKEND_NONE = 0,
    BACKEND_EMMC = 1,
    BACKEND_SDHOST = 2
};

static int g_backend = BACKEND_NONE;
static int g_emmc_reserved_for_wifi = 0;
static int g_emmc_multiblock_disabled = 0;

const char* blockdev_name(void){
    if (g_backend == BACKEND_EMMC) return "emmc";
    if (g_backend == BACKEND_SDHOST) return "sdhost";
    return "none";
}

int blockdev_init(void){
    if (!g_emmc_reserved_for_wifi){
        gpio_init_emmc();
        if (emmc_init() == 0){
            g_backend = BACKEND_EMMC;
            g_emmc_multiblock_disabled = 0;
            uart_puts("Blockdev: EMMC active\n");
            return 0;
        }
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
    if (g_emmc_reserved_for_wifi){
        // Prefer reclaiming EMMC for storage when possible.
        // On Pi 3 this temporarily takes the shared host path away from WiFi.
        uart_puts("Blockdev: EMMC takeover from WiFi...\n");
        (void)cyw43_release_emmc_for_storage();
        if (blockdev_reinit_emmc() == 0){
            uart_puts("Blockdev: EMMC takeover OK (WiFi disabled)\n");
            return 0;
        }

        uart_puts("Blockdev: EMMC takeover failed, trying SDHOST...\n");
        gpio_init_sd();
        sdhost_reset();
        if (sdhost_init_card() == 0){
            g_backend = BACKEND_SDHOST;
            uart_puts("Blockdev: SDHOST reinit OK\n");
            return 0;
        }
        g_backend = BACKEND_NONE;
        uart_puts("Blockdev: SDHOST reinit failed\n");
        return -1;
    }

    if (g_backend == BACKEND_EMMC){
        gpio_init_emmc();
        if (emmc_init() == 0){
            g_emmc_multiblock_disabled = 0;
            return 0;
        }
        return -1;
    }
    if (g_backend == BACKEND_SDHOST){
        gpio_init_sd();
        sdhost_reset();
        return sdhost_init_card();
    }
    return blockdev_init();
}

int blockdev_reinit_emmc(void){
    g_emmc_reserved_for_wifi = 0;
    gpio_init_emmc();
    if (emmc_init() == 0){
        g_backend = BACKEND_EMMC;
        g_emmc_multiblock_disabled = 0;
        return 0;
    }
    g_backend = BACKEND_NONE;
    return -1;
}

void blockdev_reserve_emmc_for_wifi(int reserved){
    g_emmc_reserved_for_wifi = reserved ? 1 : 0;
    if (g_emmc_reserved_for_wifi && g_backend == BACKEND_EMMC){
        g_backend = BACKEND_NONE;
    }
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

int blockdev_read_blocks(unsigned int lba, unsigned int count, unsigned char *buffer){
    if (!buffer || count == 0u){
        return -1;
    }
    if (count == 1u){
        return blockdev_read_block(lba, buffer);
    }

    if (g_backend == BACKEND_EMMC){
        if (!g_emmc_multiblock_disabled){
            if (emmc_read_blocks(lba, count, buffer) == 0){
                return 0;
            }
            g_emmc_multiblock_disabled = 1;
        }
        // Hardware multi-block is an optimization; preserve the known-good
        // single-block path if a card/controller rejects CMD18.
    }

    for (unsigned int i = 0u; i < count; i++){
        if (blockdev_read_block(lba + i, buffer + (i * 512u)) != 0){
            return -1;
        }
    }
    return 0;
}
