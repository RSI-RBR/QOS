#include "mmu.h"
#include "cpu.h"
#include "smp.h"
#include "spinlock.h"

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

#define AP_SHIFT            6
#define AP_EL1_RW_EL0_NONE  (0UL << AP_SHIFT)
#define AP_EL1_RW_EL0_RW    (1UL << AP_SHIFT)
#define MMU_MAX_CORES       4U

typedef struct {
    unsigned long attridx;
    unsigned long sh;
    unsigned long ap;
    unsigned long xn;
} mmu_block_attrs_t;

static unsigned long l1_table[L1_ENTRIES] __attribute__((aligned(4096)));
static unsigned long l2_table[L2_ENTRIES] __attribute__((aligned(4096)));
static unsigned long l2_table_1[L2_ENTRIES] __attribute__((aligned(4096)));
static spinlock_t g_mmu_lock;
static volatile unsigned int g_tlb_epoch = 1;
static volatile unsigned int g_tlb_ack_epoch[MMU_MAX_CORES];

static unsigned int mmu_local_core_id(void){
    unsigned int core = cpu_get_id();
    if (core >= MMU_MAX_CORES){
        return 0;
    }
    return core;
}

static void mmu_local_tlbi_all(void){
    // Translation-table update ordering for SMP:
    // 1) table stores visible 2) invalidate TLBs in shareable domain
    // 3) wait completion 4) synchronize instruction stream.
    asm volatile("dsb ishst");
    asm volatile("tlbi vmalle1is");
    asm volatile("dsb ish");
    asm volatile("isb");
}

static void mmu_tlb_shootdown_all_locked(void){
    unsigned int core = mmu_local_core_id();
    unsigned int online = smp_online_mask();
    if ((online & (1u << core)) == 0u){
        online |= (1u << core);
    }

    unsigned int epoch = g_tlb_epoch + 1u;
    if (epoch == 0u){
        epoch = 1u;
    }
    g_tlb_epoch = epoch;
    g_tlb_ack_epoch[core] = epoch;
    asm volatile("dmb ishst" : : : "memory");

    // Hardware broadcast invalidation plus explicit IPI poke so remote cores
    // observe epoch changes promptly even while idle in WFI.
    mmu_local_tlbi_all();
    for (unsigned int c = 0; c < MMU_MAX_CORES; c++){
        if (c == core){
            continue;
        }
        if ((online & (1u << c)) != 0u){
            smp_send_ipi(c);
        }
    }

    // Wait for every online core to acknowledge this epoch.
    for (unsigned int c = 0; c < MMU_MAX_CORES; c++){
        if ((online & (1u << c)) == 0u){
            continue;
        }
        while (g_tlb_ack_epoch[c] != epoch){
            asm volatile("nop");
        }
    }
    asm volatile("dmb ish" : : : "memory");
}

static unsigned long mmu_build_mair(void){
    // MAIR index0: normal WBWA cacheable, index1: device nGnRnE.
    return (0xFFUL << 0) | (0x00UL << 8);
}

static unsigned long mmu_build_tcr(void){
    // TCR: TTBR0, 4KB granule, inner-shareable WBWA, 4GB VA space (T0SZ=32).
    return (32UL << 0) | (0UL << 6) | (3UL << 8) | (1UL << 10) | (1UL << 12);
}

static void mmu_program_core_registers(void){
    unsigned long mair = mmu_build_mair();
    unsigned long tcr = mmu_build_tcr();

    asm volatile("msr mair_el1, %0" : : "r"(mair));
    asm volatile("msr tcr_el1, %0" : : "r"(tcr));
    asm volatile("msr ttbr0_el1, %0" : : "r"(l1_table));
    asm volatile("isb");
}

static void mmu_enable_current_core(void){
    unsigned long sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0);  // M
    sctlr |= (1UL << 2);  // C
    sctlr |= (1UL << 12); // I
    asm volatile("msr sctlr_el1, %0" : : "r"(sctlr));
    asm volatile("isb");
}

static void zero_tables(void){
    for (int i = 0; i < L1_ENTRIES; i++){
        l1_table[i] = 0;
    }
    for (int i = 0; i < L2_ENTRIES; i++){
        l2_table[i] = 0;
        l2_table_1[i] = 0;
    }
}

static unsigned long block_desc(unsigned long pa, const mmu_block_attrs_t* attrs){
    unsigned long desc = (pa & 0xFFFFFFFFFFE00000UL) | DESC_VALID | DESC_BLOCK | AF_BIT;
    desc |= (attrs->attridx << ATTRIDX_SHIFT) | attrs->sh | attrs->ap | attrs->xn;
    return desc;
}

static void set_block_attr(unsigned long pa, const mmu_block_attrs_t* attrs){
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

    table[l2_index] = block_desc(pa, attrs);
}

static void apply_region_attrs(unsigned long pa_start, unsigned long size, const mmu_block_attrs_t* attrs){
    if (size == 0){
        return;
    }

    unsigned long start = pa_start & ~((1UL << 21) - 1);
    unsigned long end = (pa_start + size + ((1UL << 21) - 1)) & ~((1UL << 21) - 1);
    for (unsigned long pa = start; pa < end; pa += (1UL << 21)){
        set_block_attr(pa, attrs);
    }
    mmu_tlb_shootdown_all_locked();
}

void mmu_init(void){
    spinlock_init(&g_mmu_lock);
    zero_tables();
    for (unsigned int i = 0; i < MMU_MAX_CORES; i++){
        g_tlb_ack_epoch[i] = g_tlb_epoch;
    }

    // 2GB identity map using two L2 tables (1GB each via 2MB blocks).
    l1_table[0] = ((unsigned long)l2_table & ~0xFFFUL) | DESC_VALID | DESC_TABLE;
    l1_table[1] = ((unsigned long)l2_table_1 & ~0xFFFUL) | DESC_VALID | DESC_TABLE;

    static const mmu_block_attrs_t kernel_normal = {
        .attridx = ATTRIDX_NORMAL,
        .sh = SH_INNER,
        .ap = AP_EL1_RW_EL0_NONE,
        .xn = 0
    };
    static const mmu_block_attrs_t kernel_device = {
        .attridx = ATTRIDX_DEVICE,
        .sh = SH_OUTER,
        .ap = AP_EL1_RW_EL0_NONE,
        .xn = PXN_BIT | UXN_BIT
    };

    for (unsigned long i = 0; i < L2_ENTRIES; i++){
        unsigned long pa = i << 21; // 2MB blocks
        int is_device = (pa >= DEVICE_BASE && pa < DEVICE_END);
        l2_table[i] = block_desc(pa, is_device ? &kernel_device : &kernel_normal);

        unsigned long pa1 = (1UL << 30) + (i << 21); // 1GB..2GB
        int is_device1 = (pa1 >= DEVICE_BASE && pa1 < DEVICE_END);
        l2_table_1[i] = block_desc(pa1, is_device1 ? &kernel_device : &kernel_normal);
    }

    mmu_local_tlbi_all();

    mmu_program_core_registers();
    mmu_enable_current_core();
}

void mmu_enable_secondary(void){
    // Reuse primary-built tables; each core must still program its own EL1 MMU regs.
    mmu_local_tlbi_all();
    mmu_program_core_registers();
    mmu_enable_current_core();
    g_tlb_ack_epoch[mmu_local_core_id()] = g_tlb_epoch;
}

void mmu_map_device_region(unsigned long pa_start, unsigned long size){
    static const mmu_block_attrs_t kernel_device = {
        .attridx = ATTRIDX_DEVICE,
        .sh = SH_OUTER,
        .ap = AP_EL1_RW_EL0_NONE,
        .xn = PXN_BIT | UXN_BIT
    };
    unsigned long irq = spin_lock_irqsave(&g_mmu_lock);
    apply_region_attrs(pa_start, size, &kernel_device);
    spin_unlock_irqrestore(&g_mmu_lock, irq);
}

void mmu_map_user_code_region(unsigned long pa_start, unsigned long size){
    static const mmu_block_attrs_t user_code = {
        .attridx = ATTRIDX_NORMAL,
        .sh = SH_INNER,
        .ap = AP_EL1_RW_EL0_RW,
        .xn = PXN_BIT
    };
    unsigned long irq = spin_lock_irqsave(&g_mmu_lock);
    apply_region_attrs(pa_start, size, &user_code);
    spin_unlock_irqrestore(&g_mmu_lock, irq);
}

void mmu_map_user_data_region(unsigned long pa_start, unsigned long size){
    static const mmu_block_attrs_t user_data = {
        .attridx = ATTRIDX_NORMAL,
        .sh = SH_INNER,
        .ap = AP_EL1_RW_EL0_RW,
        .xn = PXN_BIT | UXN_BIT
    };
    unsigned long irq = spin_lock_irqsave(&g_mmu_lock);
    apply_region_attrs(pa_start, size, &user_data);
    spin_unlock_irqrestore(&g_mmu_lock, irq);
}

void mmu_map_kernel_private_region(unsigned long pa_start, unsigned long size){
    static const mmu_block_attrs_t kernel_private = {
        .attridx = ATTRIDX_NORMAL,
        .sh = SH_INNER,
        .ap = AP_EL1_RW_EL0_NONE,
        .xn = PXN_BIT | UXN_BIT
    };
    unsigned long irq = spin_lock_irqsave(&g_mmu_lock);
    apply_region_attrs(pa_start, size, &kernel_private);
    spin_unlock_irqrestore(&g_mmu_lock, irq);
}

void mmu_tlb_shootdown_all(void){
    unsigned long irq = spin_lock_irqsave(&g_mmu_lock);
    mmu_tlb_shootdown_all_locked();
    spin_unlock_irqrestore(&g_mmu_lock, irq);
}

void mmu_handle_ipi(void){
    unsigned int core = mmu_local_core_id();
    unsigned int epoch = g_tlb_epoch;
    if (g_tlb_ack_epoch[core] == epoch){
        return;
    }

    // Respond to remote shootdown request.
    mmu_local_tlbi_all();
    g_tlb_ack_epoch[core] = epoch;
    asm volatile("dmb ishst" : : : "memory");
}
