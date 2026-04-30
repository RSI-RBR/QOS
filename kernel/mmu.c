#include "mmu.h"

#define L1_ENTRIES 512
#define L2_ENTRIES 512

#define DESC_VALID          (1UL << 0)
#define DESC_TABLE          (1UL << 1)
#define DESC_BLOCK          (0UL << 1)

#define ATTRIDX_SHIFT       2
#define SH_SHIFT            8
#define AF_BIT              (1UL << 10)
#define PXN_BIT             (1UL << 53)
#define UXN_BIT             (1UL << 54)

#define SH_OUTER            (2UL << SH_SHIFT)
#define SH_INNER            (3UL << SH_SHIFT)

#define ATTRIDX_NORMAL      0UL
#define ATTRIDX_DEVICE      1UL

#define DEVICE_BASE         0x3F000000UL
#define DEVICE_END          0x40200000UL

static unsigned long l1_table[L1_ENTRIES] __attribute__((aligned(4096)));
static unsigned long l2_table[L2_ENTRIES] __attribute__((aligned(4096)));
static unsigned long l2_table_1[L2_ENTRIES] __attribute__((aligned(4096)));

static void zero_tables(void){
    for (int i = 0; i < L1_ENTRIES; i++){
        l1_table[i] = 0;
    }
    for (int i = 0; i < L2_ENTRIES; i++){
        l2_table[i] = 0;
        l2_table_1[i] = 0;
    }
}

static unsigned long block_desc(unsigned long pa, int is_device){
    unsigned long desc = (pa & 0xFFFFFFFFFFE00000UL) | DESC_VALID | DESC_BLOCK | AF_BIT;
    if (is_device){
        desc |= (ATTRIDX_DEVICE << ATTRIDX_SHIFT) | SH_OUTER | PXN_BIT | UXN_BIT;
    } else{
        desc |= (ATTRIDX_NORMAL << ATTRIDX_SHIFT) | SH_INNER;
    }
    return desc;
}

static void set_block_attr(unsigned long pa, int is_device){
    unsigned long l1_index = pa >> 30;            // 1GB region
    unsigned long l2_index = (pa >> 21) & 0x1FF; // 2MB block
    unsigned long* table = 0;

    if (l1_index == 0){
        table = l2_table;
    } else if (l1_index == 1){
        table = l2_table_1;
    } else{
        return;
    }

    table[l2_index] = block_desc(pa, is_device);
}

void mmu_init(void){
    zero_tables();

    // 2GB identity map using two L2 tables (1GB each via 2MB blocks).
    l1_table[0] = ((unsigned long)l2_table & ~0xFFFUL) | DESC_VALID | DESC_TABLE;
    l1_table[1] = ((unsigned long)l2_table_1 & ~0xFFFUL) | DESC_VALID | DESC_TABLE;

    for (unsigned long i = 0; i < L2_ENTRIES; i++){
        unsigned long pa = i << 21; // 2MB blocks
        int is_device = (pa >= DEVICE_BASE && pa < DEVICE_END);
        l2_table[i] = block_desc(pa, is_device);

        unsigned long pa1 = (1UL << 30) + (i << 21); // 1GB..2GB
        int is_device1 = (pa1 >= DEVICE_BASE && pa1 < DEVICE_END);
        l2_table_1[i] = block_desc(pa1, is_device1);
    }

    // MAIR index0: normal WBWA cacheable, index1: device nGnRnE.
    unsigned long mair =
        (0xFFUL << 0) |   // AttrIdx 0
        (0x00UL << 8);    // AttrIdx 1

    // TCR: TTBR0, 4KB granule, inner-shareable WBWA, 4GB VA space (T0SZ=32).
    unsigned long tcr =
        (32UL << 0)  |    // T0SZ
        (0UL << 6)   |    // TG0 = 4KB
        (3UL << 8)   |    // SH0 = Inner shareable
        (1UL << 10)  |    // ORGN0 = WBWA
        (1UL << 12);      // IRGN0 = WBWA

    asm volatile("dsb ishst");
    asm volatile("tlbi vmalle1");
    asm volatile("dsb ish");
    asm volatile("isb");

    asm volatile("msr mair_el1, %0" : : "r"(mair));
    asm volatile("msr tcr_el1, %0" : : "r"(tcr));
    asm volatile("msr ttbr0_el1, %0" : : "r"(l1_table));
    asm volatile("isb");

    unsigned long sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0);  // M
    sctlr |= (1UL << 2);  // C
    sctlr |= (1UL << 12); // I
    asm volatile("msr sctlr_el1, %0" : : "r"(sctlr));
    asm volatile("isb");
}

void mmu_map_device_region(unsigned long pa_start, unsigned long size){
    if (size == 0){
        return;
    }

    unsigned long start = pa_start & ~((1UL << 21) - 1); // 2MB aligned
    unsigned long end = (pa_start + size + ((1UL << 21) - 1)) & ~((1UL << 21) - 1);

    for (unsigned long pa = start; pa < end; pa += (1UL << 21)){
        set_block_attr(pa, 1);
    }

    asm volatile("dsb ishst");
    asm volatile("tlbi vmalle1");
    asm volatile("dsb ish");
    asm volatile("isb");
}
