#include "dma.h"
#include "cache.h"
#include "spinlock.h"

#define DMA_BASE              0x3F007000UL
#define DMA_ENABLE_REG        (*(volatile unsigned int*)(DMA_BASE + 0xFF0))

// Keep one full DMA channel for kernel-owned memory copies. USB uses DWC2
// internal DMA, so this is separate from the USB host controller path.
#define DMA_CHANNEL           5u
#define DMA_CH_BASE           (DMA_BASE + (DMA_CHANNEL * 0x100UL))

#define DMA_CS                (*(volatile unsigned int*)(DMA_CH_BASE + 0x00))
#define DMA_CONBLK_AD         (*(volatile unsigned int*)(DMA_CH_BASE + 0x04))
#define DMA_DEBUG             (*(volatile unsigned int*)(DMA_CH_BASE + 0x20))

#define DMA_CS_ACTIVE         (1u << 0)
#define DMA_CS_END            (1u << 1)
#define DMA_CS_INT            (1u << 2)
#define DMA_CS_ERROR          (1u << 8)
#define DMA_CS_PANIC_PRIORITY_SHIFT 20
#define DMA_CS_PRIORITY_SHIFT 16
#define DMA_CS_DISDEBUG       (1u << 29)
#define DMA_CS_ABORT          (1u << 30)
#define DMA_CS_RESET          (1u << 31)

#define DMA_TI_TDMODE         (1u << 1)
#define DMA_TI_DEST_INC       (1u << 4)
#define DMA_TI_SRC_INC        (1u << 8)
#define DMA_TI_NO_WIDE_BURSTS (1u << 26)

#define DMA_BUS_UNCACHED_BASE 0xC0000000UL
#define DMA_TIMEOUT_LOOPS     2000000u

typedef struct {
    unsigned int ti;
    unsigned int source_ad;
    unsigned int dest_ad;
    unsigned int txfr_len;
    unsigned int stride;
    unsigned int nextconbk;
    unsigned int reserved0;
    unsigned int reserved1;
} dma_cb_t;

static dma_cb_t g_dma_cb __attribute__((aligned(32)));
static spinlock_t g_dma_lock;
static int g_dma_ready = 0;
static int g_dma_enabled = 0;
static int g_dma_disabled = 0;
static unsigned int g_dma_failures = 0;
static unsigned int g_dma_last_cs = 0;
static unsigned int g_dma_last_debug = 0;

static void dma_barrier(void){
    asm volatile("dsb sy" : : : "memory");
}

static unsigned int dma_bus_address(const void* p){
    unsigned long addr = (unsigned long)p;
    return (unsigned int)((addr & 0x3FFFFFFFUL) | DMA_BUS_UNCACHED_BASE);
}

static void dma_reset_channel(void){
    DMA_CS = DMA_CS_ABORT;
    dma_barrier();
    for (unsigned int i = 0; i < 10000u; i++){
        if ((DMA_CS & DMA_CS_ACTIVE) == 0u){
            break;
        }
    }

    DMA_CS = DMA_CS_RESET;
    dma_barrier();
    for (unsigned int i = 0; i < 10000u; i++){
        if ((DMA_CS & DMA_CS_RESET) == 0u){
            break;
        }
    }

    DMA_DEBUG = 0x7u;
    DMA_CS = DMA_CS_END | DMA_CS_INT | DMA_CS_ERROR;
    DMA_CONBLK_AD = 0;
    dma_barrier();
}

void dma_init(void){
    if (g_dma_ready){
        return;
    }

    spinlock_init(&g_dma_lock);
    DMA_ENABLE_REG |= (1u << DMA_CHANNEL);
    dma_barrier();
    dma_reset_channel();
    g_dma_ready = 1;
}

void dma_set_enabled(int enabled){
    dma_init();
    unsigned long irq = spin_lock_irqsave(&g_dma_lock);
    if (enabled){
        dma_reset_channel();
        g_dma_disabled = 0;
        g_dma_enabled = 1;
        g_dma_last_cs = 0;
        g_dma_last_debug = 0;
    } else{
        g_dma_enabled = 0;
        dma_reset_channel();
    }
    spin_unlock_irqrestore(&g_dma_lock, irq);
}

int dma_is_enabled(void){
    return g_dma_enabled && !g_dma_disabled;
}

unsigned int dma_failure_count(void){
    return g_dma_failures;
}

unsigned int dma_last_cs(void){
    return g_dma_last_cs;
}

unsigned int dma_last_debug(void){
    return g_dma_last_debug;
}

static int dma_start_memcopy(unsigned int src_bus,
                             unsigned int dst_bus,
                             unsigned int txfr_len,
                             unsigned int stride,
                             unsigned int ti_extra){
    unsigned long irq = spin_lock_irqsave(&g_dma_lock);
    dma_reset_channel();

    g_dma_cb.ti = DMA_TI_DEST_INC |
                  DMA_TI_SRC_INC |
                  DMA_TI_NO_WIDE_BURSTS |
                  ti_extra;
    g_dma_cb.source_ad = src_bus;
    g_dma_cb.dest_ad = dst_bus;
    g_dma_cb.txfr_len = txfr_len;
    g_dma_cb.stride = stride;
    g_dma_cb.nextconbk = 0;
    g_dma_cb.reserved0 = 0;
    g_dma_cb.reserved1 = 0;

    clean_invalidate_data_cache_range((unsigned long)&g_dma_cb, sizeof(g_dma_cb));

    dma_barrier();
    DMA_CONBLK_AD = dma_bus_address(&g_dma_cb);
    DMA_CS = DMA_CS_END | DMA_CS_INT | DMA_CS_ERROR;
    DMA_DEBUG = 0x7u;
    dma_barrier();
    DMA_CS = DMA_CS_ACTIVE |
             DMA_CS_DISDEBUG |
             (8u << DMA_CS_PRIORITY_SHIFT) |
             (8u << DMA_CS_PANIC_PRIORITY_SHIFT);

    int rc = -1;
    for (unsigned int i = 0; i < DMA_TIMEOUT_LOOPS; i++){
        unsigned int cs = DMA_CS;
        g_dma_last_cs = cs;
        if (cs & DMA_CS_ERROR){
            break;
        }
        if ((cs & DMA_CS_ACTIVE) == 0u && (cs & DMA_CS_END)){
            rc = 0;
            break;
        }
    }

    if (rc != 0){
        g_dma_last_cs = DMA_CS;
        g_dma_last_debug = DMA_DEBUG;
        g_dma_failures++;
        g_dma_disabled = 1;
        dma_reset_channel();
    } else{
        DMA_CS = DMA_CS_END | DMA_CS_INT;
        dma_barrier();
    }

    spin_unlock_irqrestore(&g_dma_lock, irq);
    return rc;
}

int dma_memcpy_to_bus(unsigned int dst_bus, const void* src, unsigned int bytes){
    if (!src || dst_bus == 0u || bytes == 0u){
        return -1;
    }
    if (!g_dma_enabled || g_dma_disabled){
        return -1;
    }
    if (bytes >= (1u << 30)){
        return -1;
    }

    dma_init();
    clean_invalidate_data_cache_range((unsigned long)src, bytes);
    return dma_start_memcopy(dma_bus_address(src), dst_bus, bytes, 0u, 0u);
}

int dma_memcpy_2d_to_bus(unsigned int dst_bus,
                         unsigned int dst_stride,
                         const void* src,
                         unsigned int src_stride,
                         unsigned int row_bytes,
                         unsigned int rows){
    if (!src || dst_bus == 0u || row_bytes == 0u || rows == 0u){
        return -1;
    }
    if (!g_dma_enabled || g_dma_disabled){
        return -1;
    }
    if (row_bytes > 0xFFFFu || rows > 0x3FFFu){
        return -1;
    }
    if (dst_stride > 0x7FFFu || src_stride > 0x7FFFu){
        return -1;
    }

    dma_init();

    unsigned long src_total = ((unsigned long)(row_bytes + src_stride) * (unsigned long)(rows - 1u)) +
                              (unsigned long)row_bytes;
    clean_invalidate_data_cache_range((unsigned long)src, src_total);

    return dma_start_memcopy(dma_bus_address(src),
                             dst_bus,
                             (rows << 16) | row_bytes,
                             ((dst_stride & 0xFFFFu) << 16) | (src_stride & 0xFFFFu),
                             DMA_TI_TDMODE);
}

int dma_memcpy_2d(void* dst,
                  unsigned int dst_stride,
                  const void* src,
                  unsigned int src_stride,
                  unsigned int row_bytes,
                  unsigned int rows){
    if (!dst || !src || row_bytes == 0u || rows == 0u){
        return -1;
    }
    if (!g_dma_enabled || g_dma_disabled){
        return -1;
    }
    if (row_bytes > 0xFFFFu || rows > 0x3FFFu){
        return -1;
    }
    if (dst_stride > 0x7FFFu || src_stride > 0x7FFFu){
        return -1;
    }

    return dma_memcpy_2d_to_bus(dma_bus_address(dst),
                                dst_stride,
                                src,
                                src_stride,
                                row_bytes,
                                rows);
}
