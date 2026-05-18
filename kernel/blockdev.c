#include "blockdev.h"
#include "emmc.h"
#include "sdhost.h"
#include "gpio.h"
#include "uart.h"
#include "cyw43.h"
#include "spinlock.h"

enum {
    BACKEND_NONE = 0,
    BACKEND_EMMC = 1,
    BACKEND_SDHOST = 2
};

static int g_backend = BACKEND_NONE;
static int g_emmc_reserved_for_wifi = 0;
static int g_emmc_multiblock_disabled = 0;
static spinlock_t g_blockdev_lock;

static unsigned long blockdev_lock(void){
    /*
     * EMMC/SD operations poll system_ticks for timeouts, so this lock must not
     * mask local IRQs. Callers that need scheduler exclusion already use the
     * kernel preemption guard around long storage syscalls.
     */
    spin_lock(&g_blockdev_lock);
    return 0;
}

static void blockdev_unlock(unsigned long irq){
    (void)irq;
    spin_unlock(&g_blockdev_lock);
}

static const char* blockdev_name_unlocked(void){
    if (g_backend == BACKEND_EMMC) return "emmc";
    if (g_backend == BACKEND_SDHOST) return "sdhost";
    return "none";
}

const char* blockdev_name(void){
    unsigned long irq = blockdev_lock();
    const char* name = blockdev_name_unlocked();
    blockdev_unlock(irq);
    return name;
}

static int blockdev_init_unlocked(void){
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

int blockdev_init(void){
    unsigned long irq = blockdev_lock();
    int rc = blockdev_init_unlocked();
    blockdev_unlock(irq);
    return rc;
}

static int blockdev_reinit_emmc_unlocked(void){
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

int blockdev_reinit(void){
    unsigned long irq = blockdev_lock();
    int reserved = g_emmc_reserved_for_wifi;
    blockdev_unlock(irq);

    /*
     * Releasing WiFi may call back into blockdev_reserve_emmc_for_wifi().
     * Keep it outside the blockdev lock, then serialize the actual storage
     * reinitialization below.
     */
    if (reserved){
        (void)cyw43_release_emmc_for_storage();
    }

    irq = blockdev_lock();
    if (g_emmc_reserved_for_wifi){
        // Prefer reclaiming EMMC for storage when possible.
        // On Pi 3 this temporarily takes the shared host path away from WiFi.
        uart_puts("Blockdev: EMMC takeover from WiFi...\n");
        blockdev_unlock(irq);
        (void)cyw43_release_emmc_for_storage();
        irq = blockdev_lock();
        if (blockdev_reinit_emmc_unlocked() == 0){
            uart_puts("Blockdev: EMMC takeover OK (WiFi disabled)\n");
            blockdev_unlock(irq);
            return 0;
        }

        uart_puts("Blockdev: EMMC takeover failed, trying SDHOST...\n");
        gpio_init_sd();
        sdhost_reset();
        if (sdhost_init_card() == 0){
            g_backend = BACKEND_SDHOST;
            uart_puts("Blockdev: SDHOST reinit OK\n");
            blockdev_unlock(irq);
            return 0;
        }
        g_backend = BACKEND_NONE;
        uart_puts("Blockdev: SDHOST reinit failed\n");
        blockdev_unlock(irq);
        return -1;
    }

    if (g_backend == BACKEND_EMMC){
        gpio_init_emmc();
        if (emmc_init() == 0){
            g_emmc_multiblock_disabled = 0;
            blockdev_unlock(irq);
            return 0;
        }
        blockdev_unlock(irq);
        return -1;
    }
    if (g_backend == BACKEND_SDHOST){
        gpio_init_sd();
        sdhost_reset();
        int rc = sdhost_init_card();
        blockdev_unlock(irq);
        return rc;
    }
    int rc = blockdev_init_unlocked();
    blockdev_unlock(irq);
    return rc;
}

int blockdev_reinit_emmc(void){
    unsigned long irq = blockdev_lock();
    int rc = blockdev_reinit_emmc_unlocked();
    blockdev_unlock(irq);
    return rc;
}

void blockdev_reserve_emmc_for_wifi(int reserved){
    unsigned long irq = blockdev_lock();
    g_emmc_reserved_for_wifi = reserved ? 1 : 0;
    if (g_emmc_reserved_for_wifi && g_backend == BACKEND_EMMC){
        g_backend = BACKEND_NONE;
    }
    blockdev_unlock(irq);
}

static int blockdev_read_block_unlocked(unsigned int lba, unsigned char *buffer){
    if (g_backend == BACKEND_EMMC){
        return emmc_read_block(lba, buffer);
    }
    if (g_backend == BACKEND_SDHOST){
        return sdhost_read_block(lba, buffer);
    }
    return -1;
}

static int blockdev_write_block_unlocked(unsigned int lba, const unsigned char *buffer){
    if (g_backend == BACKEND_EMMC){
        return emmc_write_block(lba, buffer);
    }

    /*
     * SDHOST write support is intentionally not wired yet. The EMMC path is
     * the stable storage path on the boards we are actively using.
     */
    return -1;
}

int blockdev_read_block(unsigned int lba, unsigned char *buffer){
    unsigned long irq = blockdev_lock();
    int rc = blockdev_read_block_unlocked(lba, buffer);
    blockdev_unlock(irq);
    return rc;
}

int blockdev_read_blocks(unsigned int lba, unsigned int count, unsigned char *buffer){
    unsigned long irq;
    int rc = 0;

    if (!buffer || count == 0u){
        return -1;
    }

    irq = blockdev_lock();
    if (count == 1u){
        rc = blockdev_read_block_unlocked(lba, buffer);
        blockdev_unlock(irq);
        return rc;
    }

    if (g_backend == BACKEND_EMMC){
        if (!g_emmc_multiblock_disabled){
            if (emmc_read_blocks(lba, count, buffer) == 0){
                blockdev_unlock(irq);
                return 0;
            }
            g_emmc_multiblock_disabled = 1;
        }
        // Hardware multi-block is an optimization; preserve the known-good
        // single-block path if a card/controller rejects CMD18.
    }

    for (unsigned int i = 0u; i < count; i++){
        if (blockdev_read_block_unlocked(lba + i, buffer + (i * 512u)) != 0){
            rc = -1;
            break;
        }
    }
    blockdev_unlock(irq);
    return rc;
}

int blockdev_write_block(unsigned int lba, const unsigned char *buffer){
    unsigned long irq = blockdev_lock();
    int rc = blockdev_write_block_unlocked(lba, buffer);
    blockdev_unlock(irq);
    return rc;
}

int blockdev_write_blocks(unsigned int lba, unsigned int count, const unsigned char *buffer){
    unsigned long irq;
    int rc = 0;

    if (!buffer || count == 0u){
        return -1;
    }

    irq = blockdev_lock();
    for (unsigned int i = 0u; i < count; i++){
        if (blockdev_write_block_unlocked(lba + i, buffer + (i * 512u)) != 0){
            rc = -1;
            break;
        }
    }
    blockdev_unlock(irq);
    return rc;
}
