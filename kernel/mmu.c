#include "mmu.h"
#include "cpu.h"
#include "smp.h"
#include "spinlock.h"

#define L1_ENTRIES 512
#define L2_ENTRIES 512
#define L3_ENTRIES 512

#define DESC_VALID          (1UL << 0)
#define DESC_TABLE          (1UL << 1)
#define DESC_BLOCK          (0UL << 1)
#define DESC_PAGE           DESC_TABLE
#define DESC_KIND_MASK      (DESC_VALID | DESC_TABLE)

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
#define AP_EL1_RO_EL0_RO    (3UL << AP_SHIFT)
#define AP_MASK             (3UL << AP_SHIFT)
#define MMU_MAX_CORES       4U
#define MMU_MAX_PROCESS_SPACES 8U
#define MMU_PAGE_SIZE       4096UL
#define MMU_SLOT_SIZE       (L3_ENTRIES * MMU_PAGE_SIZE)
#define MMU_BLOCK_SIZE      (1UL << 21)
#define MMU_USER_POOL_START 0x08000000UL
#define MMU_USER_POOL_SIZE  (16UL * 1024UL * 1024UL)
#define MMU_USER_POOL_END   (MMU_USER_POOL_START + MMU_USER_POOL_SIZE)
#define MMU_ASID_BITS       8U
#define MMU_ASID_MAX        ((1U << MMU_ASID_BITS) - 1U)
#define MMU_ASID_KERNEL     0U
#define NG_BIT              (1UL << 11)
#define TTBR0_BADDR_MASK    0x0000FFFFFFFFF000UL
#define TTBR0_ASID_SHIFT    48U
#define TTBR0_ASID_MASK     (0xFFUL << TTBR0_ASID_SHIFT)
#define MMU_TLB_WAIT_RETRY_INTERVAL 1024U
#define MMU_TLB_WAIT_MAX_SPINS 20000000U

typedef struct {
    unsigned long attridx;
    unsigned long sh;
    unsigned long ap;
    unsigned long xn;
    unsigned long ng;
} mmu_block_attrs_t;

static unsigned long l1_table[L1_ENTRIES] __attribute__((aligned(4096)));
static unsigned long l2_table[L2_ENTRIES] __attribute__((aligned(4096)));
static unsigned long l2_table_1[L2_ENTRIES] __attribute__((aligned(4096)));
static unsigned long proc_l1_table[MMU_MAX_PROCESS_SPACES][L1_ENTRIES] __attribute__((aligned(4096)));
static unsigned long proc_l2_table[MMU_MAX_PROCESS_SPACES][L2_ENTRIES] __attribute__((aligned(4096)));
static unsigned long proc_l2_table_1[MMU_MAX_PROCESS_SPACES][L2_ENTRIES] __attribute__((aligned(4096)));
static unsigned long proc_l3_user_slot[MMU_MAX_PROCESS_SPACES][L3_ENTRIES] __attribute__((aligned(4096)));
static unsigned char proc_space_active[MMU_MAX_PROCESS_SPACES];
static int core_active_pid[MMU_MAX_CORES];
static spinlock_t g_mmu_lock;
static volatile unsigned int g_tlb_epoch = 1;
static volatile unsigned int g_tlb_ack_epoch[MMU_MAX_CORES];
static unsigned short proc_asid[MMU_MAX_PROCESS_SPACES];
static unsigned short core_active_asid[MMU_MAX_CORES];

static unsigned int mmu_local_core_id(void){
    unsigned int core = cpu_get_id();
    if (core >= MMU_MAX_CORES){
        return 0;
    }
    return core;
}

static int mmu_range_within_user_pool(unsigned long start, unsigned long size){
    if (size == 0UL){
        return 0;
    }
    if (start < MMU_USER_POOL_START || start >= MMU_USER_POOL_END){
        return 0;
    }
    if (size > (MMU_USER_POOL_END - start)){
        return 0;
    }
    return 1;
}

static unsigned short mmu_default_asid_for_pid(int pid){
    if (pid < 0 || (unsigned int)pid >= MMU_MAX_PROCESS_SPACES){
        return (unsigned short)MMU_ASID_KERNEL;
    }
    // Reserve ASID 0 for the kernel TTBR0 context.
    unsigned int asid = (unsigned int)pid + 1u;
    if (asid > MMU_ASID_MAX){
        asid = MMU_ASID_MAX;
    }
    return (unsigned short)asid;
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

static void mmu_set_ttbr0(unsigned long table_base, unsigned short asid){
    unsigned long ttbr = (table_base & TTBR0_BADDR_MASK) |
                         ((((unsigned long)asid) & 0xFFUL) << TTBR0_ASID_SHIFT);
    // Ensure we never accidentally leak stale ASID high bits into TTBR0.
    ttbr &= (TTBR0_BADDR_MASK | TTBR0_ASID_MASK);
    asm volatile("msr ttbr0_el1, %0" : : "r"(ttbr));
    asm volatile("isb");
}

static unsigned long mmu_make_ttbr0_value(unsigned long table_base, unsigned short asid){
    unsigned long ttbr = (table_base & TTBR0_BADDR_MASK) |
                         ((((unsigned long)asid) & 0xFFUL) << TTBR0_ASID_SHIFT);
    return ttbr & (TTBR0_BADDR_MASK | TTBR0_ASID_MASK);
}

static unsigned long mmu_read_ttbr0(void){
    unsigned long ttbr;
    asm volatile("mrs %0, ttbr0_el1" : "=r"(ttbr));
    return ttbr & (TTBR0_BADDR_MASK | TTBR0_ASID_MASK);
}

static int mmu_epoch_acked_by_online(unsigned int online, unsigned int epoch){
    asm volatile("dmb ish" : : : "memory");
    for (unsigned int c = 0; c < MMU_MAX_CORES; c++){
        if ((online & (1u << c)) == 0u){
            continue;
        }
        if (g_tlb_ack_epoch[c] != epoch){
            return 0;
        }
    }
    return 1;
}

static void mmu_poke_unacked_cores(unsigned int online, unsigned int epoch, unsigned int local_core){
    for (unsigned int c = 0; c < MMU_MAX_CORES; c++){
        if (c == local_core){
            continue;
        }
        if ((online & (1u << c)) == 0u){
            continue;
        }
        if (g_tlb_ack_epoch[c] != epoch){
            smp_send_ipi(c);
        }
    }
    // Kick cores waiting in WFE loops so delayed ack is observed quickly.
    asm volatile("sev" : : : "memory");
}

static unsigned int mmu_quarantine_unacked_cores(unsigned int online, unsigned int epoch, unsigned int local_core){
    unsigned int dropped = 0u;
    for (unsigned int c = 0; c < MMU_MAX_CORES; c++){
        if (c == local_core){
            continue;
        }
        if ((online & (1u << c)) == 0u){
            continue;
        }
        if (g_tlb_ack_epoch[c] == epoch){
            continue;
        }
        smp_mark_core_offline(c);
        g_tlb_ack_epoch[c] = epoch;
        dropped++;
    }
    if (dropped){
        asm volatile("dmb ishst" : : : "memory");
    }
    return dropped;
}

void mmu_sync_local_tlb(void){
    unsigned int core = mmu_local_core_id();
    unsigned int epoch = g_tlb_epoch;
    asm volatile("dmb ish" : : : "memory");
    if (g_tlb_ack_epoch[core] == epoch){
        return;
    }

    mmu_local_tlbi_all();
    g_tlb_ack_epoch[core] = epoch;
    asm volatile("dmb ishst" : : : "memory");
    asm volatile("sev" : : : "memory");
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
    asm volatile("dmb ishst" : : : "memory");

    // Hardware broadcast invalidation plus explicit IPI poke so remote cores
    // observe epoch changes promptly even while idle in WFI.
    mmu_local_tlbi_all();
    g_tlb_ack_epoch[core] = epoch;
    asm volatile("dmb ishst" : : : "memory");
    mmu_poke_unacked_cores(online, epoch, core);

    // Wait for every online core to acknowledge this epoch. Re-poke stragglers
    // so shootdown completes even if a mailbox edge was missed.
    unsigned int spins = 0u;
    unsigned int timeout_recoveries = 0u;
    while (!mmu_epoch_acked_by_online(online, epoch)){
        if ((spins & (MMU_TLB_WAIT_RETRY_INTERVAL - 1u)) == 0u){
            mmu_poke_unacked_cores(online, epoch, core);
        }
        if (spins++ >= MMU_TLB_WAIT_MAX_SPINS){
            unsigned int dropped = mmu_quarantine_unacked_cores(online, epoch, core);
            online = smp_online_mask();
            if ((online & (1u << core)) == 0u){
                online |= (1u << core);
            }
            if (!dropped || timeout_recoveries++ >= 1u){
                // Keep the system live even if a core is unhealthy.
                break;
            }
            spins = 0u;
            mmu_poke_unacked_cores(online, epoch, core);
        }
        // Do not sleep with WFE here: if a secondary core missed the IPI/SEV,
        // the timeout counter would stop advancing and process creation could
        // hang forever. Keep this as a bounded poll so unhealthy cores are
        // quarantined and boot/shell startup remains recoverable.
        asm volatile("nop" : : : "memory");
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
    mmu_set_ttbr0((unsigned long)l1_table, (unsigned short)MMU_ASID_KERNEL);
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
    for (unsigned int p = 0; p < MMU_MAX_PROCESS_SPACES; p++){
        proc_space_active[p] = 0;
        proc_asid[p] = mmu_default_asid_for_pid((int)p);
    }
    for (unsigned int c = 0; c < MMU_MAX_CORES; c++){
        core_active_pid[c] = -1;
        core_active_asid[c] = (unsigned short)MMU_ASID_KERNEL;
    }
    for (int i = 0; i < L1_ENTRIES; i++){
        l1_table[i] = 0;
        for (unsigned int p = 0; p < MMU_MAX_PROCESS_SPACES; p++){
            proc_l1_table[p][i] = 0;
        }
    }
    for (int i = 0; i < L2_ENTRIES; i++){
        l2_table[i] = 0;
        l2_table_1[i] = 0;
        for (unsigned int p = 0; p < MMU_MAX_PROCESS_SPACES; p++){
            proc_l2_table[p][i] = 0;
            proc_l2_table_1[p][i] = 0;
        }
    }
    for (unsigned int p = 0; p < MMU_MAX_PROCESS_SPACES; p++){
        for (unsigned int i = 0; i < L3_ENTRIES; i++){
            proc_l3_user_slot[p][i] = 0;
        }
    }
}

static unsigned long block_desc(unsigned long pa, const mmu_block_attrs_t* attrs){
    unsigned long desc = (pa & 0xFFFFFFFFFFE00000UL) | DESC_VALID | DESC_BLOCK | AF_BIT;
    desc |= (attrs->attridx << ATTRIDX_SHIFT) | attrs->sh | attrs->ap | attrs->xn | attrs->ng;
    return desc;
}

static unsigned long page_desc(unsigned long pa, const mmu_block_attrs_t* attrs){
    unsigned long desc = (pa & 0xFFFFFFFFFFFFF000UL) | DESC_VALID | DESC_PAGE | AF_BIT;
    desc |= (attrs->attridx << ATTRIDX_SHIFT) | attrs->sh | attrs->ap | attrs->xn | attrs->ng;
    return desc;
}

static void set_block_attr_for_tables(unsigned long* table0, unsigned long* table1,
                                      unsigned long pa, const mmu_block_attrs_t* attrs){
    unsigned long l1_index = pa >> 30;            // 1GB region
    unsigned long l2_index = (pa >> 21) & 0x1FF; // 2MB block
    unsigned long* table = 0;

    if (l1_index == 0){
        table = table0;
    } else if (l1_index == 1){
        table = table1;
    } else{
        return;
    }

    // Never clobber an L2 table-descriptor (used for per-process L3 user slot
    // mappings) with a block descriptor. This preserves per-process slot tables
    // when global/device attribute updates run on active address spaces.
    if ((table[l2_index] & DESC_KIND_MASK) == (DESC_VALID | DESC_TABLE)){
        return;
    }
    table[l2_index] = block_desc(pa, attrs);
}

static void apply_region_attrs_for_tables(unsigned long* table0, unsigned long* table1,
                                          unsigned long pa_start, unsigned long size,
                                          const mmu_block_attrs_t* attrs){
    if (size == 0){
        return;
    }

    unsigned long start = pa_start & ~((1UL << 21) - 1UL);
    unsigned long end = (pa_start + size + ((1UL << 21) - 1UL)) & ~((1UL << 21) - 1UL);
    for (unsigned long pa = start; pa < end; pa += (1UL << 21)){
        set_block_attr_for_tables(table0, table1, pa, attrs);
    }
}

static void apply_region_attrs_all_spaces(unsigned long pa_start, unsigned long size,
                                          const mmu_block_attrs_t* attrs){
    apply_region_attrs_for_tables(l2_table, l2_table_1, pa_start, size, attrs);
    for (unsigned int pid = 0; pid < MMU_MAX_PROCESS_SPACES; pid++){
        if (!proc_space_active[pid]){
            continue;
        }
        apply_region_attrs_for_tables(proc_l2_table[pid], proc_l2_table_1[pid], pa_start, size, attrs);
    }
    mmu_tlb_shootdown_all_locked();
}

static void l3_fill_kernel_private(unsigned long* l3, unsigned long slot_base){
    static const mmu_block_attrs_t kernel_private = {
        .attridx = ATTRIDX_NORMAL,
        .sh = SH_INNER,
        .ap = AP_EL1_RW_EL0_NONE,
        .xn = PXN_BIT | UXN_BIT,
        .ng = 0
    };
    for (unsigned int i = 0; i < L3_ENTRIES; i++){
        l3[i] = page_desc(slot_base + ((unsigned long)i * MMU_PAGE_SIZE), &kernel_private);
    }
}

static void l3_map_range(unsigned long* l3,
                         unsigned long slot_base,
                         unsigned long off,
                         unsigned long size,
                         const mmu_block_attrs_t* attrs){
    if (size == 0u){
        return;
    }
    if (off >= MMU_SLOT_SIZE){
        return;
    }
    if (size > (MMU_SLOT_SIZE - off)){
        size = MMU_SLOT_SIZE - off;
    }

    unsigned long start = off & ~(MMU_PAGE_SIZE - 1UL);
    unsigned long end = (off + size + (MMU_PAGE_SIZE - 1UL)) & ~(MMU_PAGE_SIZE - 1UL);
    for (unsigned long p = start; p < end; p += MMU_PAGE_SIZE){
        unsigned int idx = (unsigned int)(p / MMU_PAGE_SIZE);
        if (idx >= L3_ENTRIES){
            break;
        }
        l3[idx] = page_desc(slot_base + p, attrs);
    }
}

static void l3_unmap_range(unsigned long* l3,
                           unsigned long off,
                           unsigned long size){
    if (size == 0u){
        return;
    }
    if (off >= MMU_SLOT_SIZE){
        return;
    }
    if (size > (MMU_SLOT_SIZE - off)){
        size = MMU_SLOT_SIZE - off;
    }

    unsigned long start = off & ~(MMU_PAGE_SIZE - 1UL);
    unsigned long end = (off + size + (MMU_PAGE_SIZE - 1UL)) & ~(MMU_PAGE_SIZE - 1UL);
    for (unsigned long p = start; p < end; p += MMU_PAGE_SIZE){
        unsigned int idx = (unsigned int)(p / MMU_PAGE_SIZE);
        if (idx >= L3_ENTRIES){
            break;
        }
        l3[idx] = 0UL;
    }
}

static int l3_slot_is_wx_safe(const unsigned long* l3){
    if (!l3){
        return 0;
    }
    for (unsigned int i = 0; i < L3_ENTRIES; i++){
        unsigned long desc = l3[i];
        if ((desc & DESC_VALID) == 0UL){
            continue;
        }
        if ((desc & DESC_KIND_MASK) != (DESC_VALID | DESC_PAGE)){
            continue;
        }

        unsigned long ap = (desc & AP_MASK) >> AP_SHIFT;
        int writable = (ap == 0UL || ap == 1UL) ? 1 : 0;
        int executable = ((desc & (PXN_BIT | UXN_BIT)) != (PXN_BIT | UXN_BIT)) ? 1 : 0;
        if (writable && executable){
            return 0;
        }
    }
    return 1;
}

static int l2_is_el0_none_except_user_slot(const unsigned long* l2, unsigned long user_slot_index){
    if (!l2){
        return 0;
    }
    if (user_slot_index > L2_ENTRIES){
        return 0;
    }

    for (unsigned int i = 0; i < L2_ENTRIES; i++){
        unsigned long desc = l2[i];
        if ((desc & DESC_VALID) == 0UL){
            continue;
        }

        // Only the designated user slot may point to an L3 table.
        if ((desc & DESC_KIND_MASK) == (DESC_VALID | DESC_TABLE)){
            if (user_slot_index >= L2_ENTRIES || (unsigned long)i != user_slot_index){
                return 0;
            }
            continue;
        }

        // For all block mappings, EL0 access must be disabled.
        unsigned long ap = (desc & AP_MASK) >> AP_SHIFT;
        int el0_access = (ap == 1UL || ap == 3UL) ? 1 : 0;
        if (el0_access){
            return 0;
        }
    }
    return 1;
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
        .xn = 0,
        .ng = 0
    };
    static const mmu_block_attrs_t kernel_device = {
        .attridx = ATTRIDX_DEVICE,
        .sh = SH_OUTER,
        .ap = AP_EL1_RW_EL0_NONE,
        .xn = PXN_BIT | UXN_BIT,
        .ng = 0
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
    asm volatile("dmb ishst" : : : "memory");
}

void mmu_process_spaces_reset(void){
    unsigned long irq = spin_lock_irqsave(&g_mmu_lock);
    for (unsigned int pid = 0; pid < MMU_MAX_PROCESS_SPACES; pid++){
        proc_space_active[pid] = 0;
    }
    for (unsigned int core = 0; core < MMU_MAX_CORES; core++){
        core_active_pid[core] = -1;
        core_active_asid[core] = (unsigned short)MMU_ASID_KERNEL;
    }
    mmu_set_ttbr0((unsigned long)l1_table, (unsigned short)MMU_ASID_KERNEL);
    mmu_tlb_shootdown_all_locked();
    spin_unlock_irqrestore(&g_mmu_lock, irq);
}

int mmu_process_space_create(int pid,
                             unsigned long user_pa_start,
                             unsigned long user_size,
                             unsigned long user_rw_offset,
                             unsigned long user_rw_size){
    if (pid < 0 || (unsigned int)pid >= MMU_MAX_PROCESS_SPACES || user_size == 0u){
        return -1;
    }

    static const mmu_block_attrs_t user_code_rx = {
        .attridx = ATTRIDX_NORMAL,
        .sh = SH_INNER,
        .ap = AP_EL1_RO_EL0_RO,
        .xn = PXN_BIT,
        .ng = NG_BIT
    };
    static const mmu_block_attrs_t user_data_rw_nx = {
        .attridx = ATTRIDX_NORMAL,
        .sh = SH_INNER,
        .ap = AP_EL1_RW_EL0_RW,
        .xn = PXN_BIT | UXN_BIT,
        .ng = NG_BIT
    };
    const unsigned long guard_bytes = QOS_USER_GUARD_PAGE_BYTES;
    const unsigned long stack_bytes = QOS_USER_STACK_BYTES;

    unsigned long irq = spin_lock_irqsave(&g_mmu_lock);

    unsigned long slot_base = user_pa_start & ~(MMU_SLOT_SIZE - 1UL);
    if (user_pa_start != slot_base || user_size > MMU_SLOT_SIZE){
        spin_unlock_irqrestore(&g_mmu_lock, irq);
        return -1;
    }
    if (!mmu_range_within_user_pool(slot_base, MMU_SLOT_SIZE)){
        spin_unlock_irqrestore(&g_mmu_lock, irq);
        return -1;
    }
    if (user_rw_offset >= MMU_SLOT_SIZE || user_rw_size == 0u){
        spin_unlock_irqrestore(&g_mmu_lock, irq);
        return -1;
    }
    if ((user_rw_offset & (MMU_PAGE_SIZE - 1UL)) != 0UL){
        spin_unlock_irqrestore(&g_mmu_lock, irq);
        return -1;
    }
    if (user_rw_offset < MMU_PAGE_SIZE){
        spin_unlock_irqrestore(&g_mmu_lock, irq);
        return -1;
    }
    if (user_rw_size > (MMU_SLOT_SIZE - user_rw_offset)){
        spin_unlock_irqrestore(&g_mmu_lock, irq);
        return -1;
    }
    if ((guard_bytes & (MMU_PAGE_SIZE - 1UL)) != 0UL ||
        (stack_bytes & (MMU_PAGE_SIZE - 1UL)) != 0UL){
        spin_unlock_irqrestore(&g_mmu_lock, irq);
        return -1;
    }
    if (user_rw_size <= (stack_bytes + guard_bytes + MMU_PAGE_SIZE)){
        spin_unlock_irqrestore(&g_mmu_lock, irq);
        return -1;
    }
    unsigned long rw_end = user_rw_offset + user_rw_size;
    unsigned long stack_off = rw_end - stack_bytes;
    unsigned long guard_off = stack_off - guard_bytes;
    if (guard_off < user_rw_offset){
        spin_unlock_irqrestore(&g_mmu_lock, irq);
        return -1;
    }
    unsigned long heap_size = guard_off - user_rw_offset;
    if (heap_size < MMU_PAGE_SIZE){
        spin_unlock_irqrestore(&g_mmu_lock, irq);
        return -1;
    }
    for (unsigned int i = 0; i < L1_ENTRIES; i++){
        proc_l1_table[pid][i] = l1_table[i];
    }
    for (unsigned int i = 0; i < L2_ENTRIES; i++){
        proc_l2_table[pid][i] = l2_table[i];
        proc_l2_table_1[pid][i] = l2_table_1[i];
    }

    proc_l1_table[pid][0] = ((unsigned long)proc_l2_table[pid] & ~0xFFFUL) | DESC_VALID | DESC_TABLE;
    proc_l1_table[pid][1] = ((unsigned long)proc_l2_table_1[pid] & ~0xFFFUL) | DESC_VALID | DESC_TABLE;

    unsigned long l1_index = slot_base >> 30;
    unsigned long l2_index = (slot_base >> 21) & 0x1FFUL;
    unsigned long* l2 = 0;
    if (l1_index == 0UL){
        l2 = proc_l2_table[pid];
    } else if (l1_index == 1UL){
        l2 = proc_l2_table_1[pid];
    } else{
        spin_unlock_irqrestore(&g_mmu_lock, irq);
        return -1;
    }

    l2[l2_index] = ((unsigned long)proc_l3_user_slot[pid] & ~0xFFFUL) | DESC_VALID | DESC_TABLE;
    if (!l2_is_el0_none_except_user_slot(proc_l2_table[pid], (l1_index == 0UL) ? l2_index : L2_ENTRIES) ||
        !l2_is_el0_none_except_user_slot(proc_l2_table_1[pid], (l1_index == 1UL) ? l2_index : L2_ENTRIES)){
        spin_unlock_irqrestore(&g_mmu_lock, irq);
        return -1;
    }

    l3_fill_kernel_private(proc_l3_user_slot[pid], slot_base);
    l3_map_range(proc_l3_user_slot[pid], slot_base, 0u, user_rw_offset, &user_code_rx);
    // Heap/data are RW+NX up to the guard page below the stack.
    l3_map_range(proc_l3_user_slot[pid], slot_base, user_rw_offset, heap_size, &user_data_rw_nx);
    // Leave one unmapped guard page between heap and stack.
    l3_unmap_range(proc_l3_user_slot[pid], guard_off, guard_bytes);
    l3_map_range(proc_l3_user_slot[pid], slot_base, stack_off, stack_bytes, &user_data_rw_nx);
    if (!l3_slot_is_wx_safe(proc_l3_user_slot[pid])){
        spin_unlock_irqrestore(&g_mmu_lock, irq);
        return -1;
    }
    proc_asid[pid] = mmu_default_asid_for_pid(pid);
    proc_space_active[pid] = 1;

    mmu_tlb_shootdown_all_locked();
    spin_unlock_irqrestore(&g_mmu_lock, irq);
    return 0;
}

void mmu_process_space_destroy(int pid){
    if (pid < 0 || (unsigned int)pid >= MMU_MAX_PROCESS_SPACES){
        return;
    }
    unsigned long irq = spin_lock_irqsave(&g_mmu_lock);
    proc_space_active[pid] = 0;
    for (unsigned int core = 0; core < MMU_MAX_CORES; core++){
        if (core_active_pid[core] == pid){
            core_active_pid[core] = -1;
        }
    }
    mmu_tlb_shootdown_all_locked();
    spin_unlock_irqrestore(&g_mmu_lock, irq);
}

void mmu_switch_to_pid(int pid){
    // Defensive catch-up for cases where a core had IRQs masked during a
    // remote shootdown request.
    mmu_sync_local_tlb();

    unsigned int core = mmu_local_core_id();
    unsigned long irq = spin_lock_irqsave(&g_mmu_lock);
    unsigned long* table = l1_table;
    int effective_pid = -1;
    unsigned short effective_asid = (unsigned short)MMU_ASID_KERNEL;
    if (pid >= 0 &&
        (unsigned int)pid < MMU_MAX_PROCESS_SPACES &&
        proc_space_active[pid]){
        table = proc_l1_table[pid];
        effective_pid = pid;
        effective_asid = proc_asid[pid];
    }

    // Always rewrite TTBR0 on an explicit switch. The cached pid/asid fields
    // are useful diagnostics, but TTBR0 is the real security boundary and can
    // be disturbed by low-level recovery/init paths.
    mmu_set_ttbr0((unsigned long)table, effective_asid);
    core_active_pid[core] = effective_pid;
    core_active_asid[core] = effective_asid;
    spin_unlock_irqrestore(&g_mmu_lock, irq);
}

void mmu_prepare_return_to_pid(int pid){
    unsigned int core = mmu_local_core_id();
    unsigned long* table = l1_table;
    int effective_pid = -1;
    unsigned short effective_asid = (unsigned short)MMU_ASID_KERNEL;

    if (pid >= 0 &&
        (unsigned int)pid < MMU_MAX_PROCESS_SPACES &&
        proc_space_active[pid]){
        table = proc_l1_table[pid];
        effective_pid = pid;
        effective_asid = proc_asid[pid];
    }

    unsigned long expected = mmu_make_ttbr0_value((unsigned long)table, effective_asid);
    if (core_active_pid[core] == effective_pid &&
        core_active_asid[core] == effective_asid &&
        mmu_read_ttbr0() == expected){
        return;
    }

    // Slow path only when a caller is about to return with a stale/wrong
    // address space. This preserves the final eret safety net without paying
    // the full TLB/lock cost on every timer tick.
    mmu_switch_to_pid(pid);
}

void mmu_map_device_region(unsigned long pa_start, unsigned long size){
    static const mmu_block_attrs_t kernel_device = {
        .attridx = ATTRIDX_DEVICE,
        .sh = SH_OUTER,
        .ap = AP_EL1_RW_EL0_NONE,
        .xn = PXN_BIT | UXN_BIT,
        .ng = 0
    };
    unsigned long irq = spin_lock_irqsave(&g_mmu_lock);
    apply_region_attrs_all_spaces(pa_start, size, &kernel_device);
    spin_unlock_irqrestore(&g_mmu_lock, irq);
}

void mmu_map_user_code_region(unsigned long pa_start, unsigned long size){
    static const mmu_block_attrs_t user_code = {
        .attridx = ATTRIDX_NORMAL,
        .sh = SH_INNER,
        .ap = AP_EL1_RO_EL0_RO,
        .xn = PXN_BIT,
        .ng = NG_BIT
    };
    if ((pa_start & (MMU_BLOCK_SIZE - 1UL)) != 0UL ||
        (size & (MMU_BLOCK_SIZE - 1UL)) != 0UL ||
        !mmu_range_within_user_pool(pa_start, size)){
        return;
    }
    unsigned long irq = spin_lock_irqsave(&g_mmu_lock);
    apply_region_attrs_for_tables(l2_table, l2_table_1, pa_start, size, &user_code);
    mmu_tlb_shootdown_all_locked();
    spin_unlock_irqrestore(&g_mmu_lock, irq);
}

void mmu_map_user_data_region(unsigned long pa_start, unsigned long size){
    static const mmu_block_attrs_t user_data = {
        .attridx = ATTRIDX_NORMAL,
        .sh = SH_INNER,
        .ap = AP_EL1_RW_EL0_RW,
        .xn = PXN_BIT | UXN_BIT,
        .ng = NG_BIT
    };
    if ((pa_start & (MMU_BLOCK_SIZE - 1UL)) != 0UL ||
        (size & (MMU_BLOCK_SIZE - 1UL)) != 0UL ||
        !mmu_range_within_user_pool(pa_start, size)){
        return;
    }
    unsigned long irq = spin_lock_irqsave(&g_mmu_lock);
    apply_region_attrs_for_tables(l2_table, l2_table_1, pa_start, size, &user_data);
    mmu_tlb_shootdown_all_locked();
    spin_unlock_irqrestore(&g_mmu_lock, irq);
}

void mmu_map_kernel_private_region(unsigned long pa_start, unsigned long size){
    static const mmu_block_attrs_t kernel_private = {
        .attridx = ATTRIDX_NORMAL,
        .sh = SH_INNER,
        .ap = AP_EL1_RW_EL0_NONE,
        .xn = PXN_BIT | UXN_BIT,
        .ng = 0
    };
    unsigned long irq = spin_lock_irqsave(&g_mmu_lock);
    apply_region_attrs_all_spaces(pa_start, size, &kernel_private);
    spin_unlock_irqrestore(&g_mmu_lock, irq);
}

void mmu_tlb_shootdown_all(void){
    unsigned long irq = spin_lock_irqsave(&g_mmu_lock);
    mmu_tlb_shootdown_all_locked();
    spin_unlock_irqrestore(&g_mmu_lock, irq);
}

void mmu_handle_ipi(void){
    mmu_sync_local_tlb();
}
