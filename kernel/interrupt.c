#include "interrupt.h"
#include "uart.h"
#include "timer.h"
#include "process.h"
#include "syscall.h"
#include "net.h"
#include "arp.h"
#include "mmu.h"
#include "cpu.h"
#include "smp.h"

extern void vectors(void);

#define LOCAL_BASE 0x40000000UL
#define CORE_IRQ_SOURCE(core) (*(volatile unsigned int*)(LOCAL_BASE + 0x60 + ((core) * 4u)))
#define CORE_CNTPNSIRQ_PENDING (1u << 1)
#define CORE_MAILBOX0_PENDING (1u << 4)

#define IRQ_BASE 0x3F00B000UL
#define IRQ_PENDING_2 (*(volatile unsigned int*)(IRQ_BASE + 0x204))
#define IRQ_SDHOST_PENDING_BIT (1u << 30) // IRQ 62 -> bank2 bit 30

static void (*g_sdhost_irq_handler)(void) = 0;
static volatile unsigned int g_kernel_preempt_depth[MAX_CPU_CORES];
static volatile unsigned long g_kernel_preempt_saved_daif[MAX_CPU_CORES];

#define MAX_BANK2_IRQ_HANDLERS 8
static unsigned int g_bank2_irq_bits[MAX_BANK2_IRQ_HANDLERS];
static void (*g_bank2_irq_handlers[MAX_BANK2_IRQ_HANDLERS])(void);

// Trap-frame index from vectors.S layout:
// x0..x30 => 0..30, ELR => 31, SPSR => 32, SP_EL0 => 33
#define IRQ_FRAME_SPSR_IDX 32
#define SPSR_MODE_MASK 0xFUL
#define SPSR_MODE_EL0T 0x0UL

static const char* data_fault_name(unsigned long dfsc){
    switch (dfsc & 0x3FUL){
        case 0x04UL: return "translation fault L0";
        case 0x05UL: return "translation fault L1";
        case 0x06UL: return "translation fault L2";
        case 0x07UL: return "translation fault L3";
        case 0x09UL: return "access flag fault L1";
        case 0x0AUL: return "access flag fault L2";
        case 0x0BUL: return "access flag fault L3";
        case 0x0DUL: return "permission fault L1";
        case 0x0EUL: return "permission fault L2";
        case 0x0FUL: return "permission fault L3";
        case 0x10UL: return "synchronous external abort";
        case 0x21UL: return "alignment fault";
        default: return "unknown data fault";
    }
}

static const char* instr_fault_name(unsigned long ifsc){
    switch (ifsc & 0x3FUL){
        case 0x04UL: return "translation fault L0";
        case 0x05UL: return "translation fault L1";
        case 0x06UL: return "translation fault L2";
        case 0x07UL: return "translation fault L3";
        case 0x09UL: return "access flag fault L1";
        case 0x0AUL: return "access flag fault L2";
        case 0x0BUL: return "access flag fault L3";
        case 0x0DUL: return "permission fault L1";
        case 0x0EUL: return "permission fault L2";
        case 0x0FUL: return "permission fault L3";
        case 0x10UL: return "synchronous external abort";
        default: return "unknown instruction fault";
    }
}

static void dump_user_fault_layout(unsigned long elr, unsigned long far){
    process_t* p = get_current_process();
    if (!p || !p->program_memory){
        return;
    }

    unsigned long base = (unsigned long)p->program_memory;
    unsigned long size = p->program_size;
    unsigned long rw_start = base + p->user_rw_offset;
    unsigned long rw_end = rw_start + p->user_rw_size;

    uart_puts("PROG_BASE=");
    uart_puthex((unsigned int)base);
    uart_puts(" PROG_SIZE=");
    uart_puthex((unsigned int)size);
    uart_puts("\n");

    uart_puts("ELR_OFF=");
    if (elr >= base && elr < base + size){
        uart_puthex((unsigned int)(elr - base));
    } else{
        uart_puts("OUTSIDE");
    }
    uart_puts(" FAR_OFF=");
    if (far >= base && far < base + size){
        uart_puthex((unsigned int)(far - base));
    } else{
        uart_puts("OUTSIDE");
    }
    uart_puts("\n");

    uart_puts("USER_RX=[");
    uart_puthex((unsigned int)base);
    uart_puts(",");
    uart_puthex((unsigned int)rw_start);
    uart_puts(") USER_RW=[");
    uart_puthex((unsigned int)rw_start);
    uart_puts(",");
    uart_puthex((unsigned int)rw_end);
    uart_puts(")\n");
}

static unsigned int preempt_core_id(void){
    unsigned int core = cpu_get_id();
    if (core >= MAX_CPU_CORES){
        return 0u;
    }
    return core;
}

static void ensure_return_ttbr_for_frame(void* frame_sp){
    unsigned long* frame = (unsigned long*)frame_sp;
    if (!frame){
        return;
    }
    if ((frame[IRQ_FRAME_SPSR_IDX] & SPSR_MODE_MASK) != SPSR_MODE_EL0T){
        return;
    }

    process_t* cur = get_current_process();
    int pid = process_current_pid();
    mmu_prepare_return_to_pid((cur && cur->user_mode && pid >= 0) ? pid : -1);
}

void interrupt_init(void){
    asm volatile("msr VBAR_EL1, %0" : : "r"(vectors));
    asm volatile("isb");

    timer_init();
    smp_init_ipi_for_core(cpu_get_id());

    if (cpu_get_id() == 0){
        uart_puts("Interrupts initialized\n");
    }
}

void interrupt_register_sdhost_irq(void (*handler)(void)){
    g_sdhost_irq_handler = handler;
    interrupt_register_bank2_irq(IRQ_SDHOST_PENDING_BIT, handler);
}

int interrupt_register_bank2_irq(unsigned int pending_bit_mask, void (*handler)(void)){
    if (!pending_bit_mask || !handler){
        return -1;
    }

    for (int i = 0; i < MAX_BANK2_IRQ_HANDLERS; i++){
        if (g_bank2_irq_handlers[i] == handler && g_bank2_irq_bits[i] == pending_bit_mask){
            return 0;
        }
    }

    for (int i = 0; i < MAX_BANK2_IRQ_HANDLERS; i++){
        if (!g_bank2_irq_handlers[i]){
            g_bank2_irq_bits[i] = pending_bit_mask;
            g_bank2_irq_handlers[i] = handler;
            return 0;
        }
    }
    return -1;
}

void enable_interrupts(void){
    asm volatile("msr daifclr, #2");
}

void disable_interrupts(void){
    asm volatile("msr daifset, #2");
}

void kernel_preempt_enter(void){
    unsigned long daif_prev;
    unsigned int core = preempt_core_id();
    asm volatile("mrs %0, daif" : "=r"(daif_prev));
    asm volatile("msr daifset, #2");
    if (g_kernel_preempt_depth[core] == 0u){
        g_kernel_preempt_saved_daif[core] = daif_prev;
    }
    g_kernel_preempt_depth[core]++;
    // Sync exceptions (SVC) typically enter EL1 with IRQ masked. Unmask IRQ
    // inside opted-in long kernel paths so timer IRQ can drive scheduling.
    asm volatile("msr daifclr, #2" : : : "memory");
}

void kernel_preempt_exit(void){
    unsigned long restore_daif = 0;
    unsigned int core = preempt_core_id();
    asm volatile("msr daifset, #2");
    if (g_kernel_preempt_depth[core] > 0u){
        g_kernel_preempt_depth[core]--;
        if (g_kernel_preempt_depth[core] == 0u){
            restore_daif = g_kernel_preempt_saved_daif[core];
        }
    }
    if (g_kernel_preempt_depth[core] == 0u){
        asm volatile("msr daif, %0" : : "r"(restore_daif) : "memory");
    } else{
        // Keep IRQ enabled for outer opted-in region.
        asm volatile("msr daifclr, #2" : : : "memory");
    }
}

int kernel_preempt_enabled(void){
    return g_kernel_preempt_depth[preempt_core_id()] > 0u ? 1 : 0;
}

void* irq_handler(void* irq_frame_sp){
    unsigned int core = cpu_get_id();
    mmu_sync_local_tlb();
    unsigned int local_src = CORE_IRQ_SOURCE(core);
    if (local_src & CORE_CNTPNSIRQ_PENDING){
        timer_clear_interrupt();
        if (core == 0){
            timer_handler();
            // Keep timer IRQ short and non-blocking; NIC polling can stall
            // and starve scheduling when done in interrupt context.
            // Networking paths invoke net_poll() from syscall/foreground flow,
            // including periodic ARP maintenance.
        } else{
            scheduler_tick();
        }
        // Context-switch only from EL0 frames or from the kernel idle loop.
        // Sleep syscalls yield explicitly at the SVC frame boundary, which
        // avoids saving nested EL1 timer frames as process resume points.
        unsigned long* frame = (unsigned long*)irq_frame_sp;
        unsigned long spsr = frame ? frame[IRQ_FRAME_SPSR_IDX] : 0;
        int need_resched = scheduler_consume_need_resched();
        int in_el0 = ((spsr & SPSR_MODE_MASK) == SPSR_MODE_EL0T);
        process_t* cur = get_current_process();
        int idle_kernel = (cur == 0);
        if (need_resched && (in_el0 || idle_kernel)){
            void* out = scheduler_on_irq(irq_frame_sp);
            ensure_return_ttbr_for_frame(out);
            return out;
        }
        ensure_return_ttbr_for_frame(irq_frame_sp);
        return irq_frame_sp;
    }

    if (local_src & CORE_MAILBOX0_PENDING){
        smp_clear_ipi_for_core(core);
        mmu_handle_ipi();
        unsigned long* frame = (unsigned long*)irq_frame_sp;
        unsigned long spsr = frame ? frame[IRQ_FRAME_SPSR_IDX] : 0;
        int need_resched = scheduler_consume_need_resched();
        int in_el0 = ((spsr & SPSR_MODE_MASK) == SPSR_MODE_EL0T);
        process_t* cur = get_current_process();
        int idle_kernel = (cur == 0);
        if (need_resched && (in_el0 || idle_kernel)){
            void* out = scheduler_on_irq(irq_frame_sp);
            ensure_return_ttbr_for_frame(out);
            return out;
        }
        ensure_return_ttbr_for_frame(irq_frame_sp);
        return irq_frame_sp;
    }

    unsigned int bank2_pending = IRQ_PENDING_2;
    if (bank2_pending){
        for (int i = 0; i < MAX_BANK2_IRQ_HANDLERS; i++){
            if (!g_bank2_irq_handlers[i]){
                continue;
            }
            if (bank2_pending & g_bank2_irq_bits[i]){
                g_bank2_irq_handlers[i]();
                ensure_return_ttbr_for_frame(irq_frame_sp);
                return irq_frame_sp;
            }
        }
        // Backward compatibility fallback if legacy SDHOST registration is used.
        if ((bank2_pending & IRQ_SDHOST_PENDING_BIT) && g_sdhost_irq_handler){
            g_sdhost_irq_handler();
            ensure_return_ttbr_for_frame(irq_frame_sp);
            return irq_frame_sp;
        }
    }

    ensure_return_ttbr_for_frame(irq_frame_sp);
    return irq_frame_sp;
}

void* sync_exception_handler(void* frame_sp, unsigned long esr, unsigned long elr, unsigned long spsr){
    unsigned long ec = (esr >> 26) & 0x3FUL;
    unsigned long far = 0;
    asm volatile("mrs %0, far_el1" : "=r"(far));

    if (ec == 0x15UL){
        void* out = syscall_handle(frame_sp, esr);
        ensure_return_ttbr_for_frame(out);
        return out;
    }

    uart_puts("\nSYNC EXCEPTION\n");
    uart_puts("CORE=");
    uart_puthex(cpu_get_id());
    uart_puts(" PID=");
    {
        int pid = process_current_pid();
        if (pid < 0){
            uart_puts("FFFFFFFF");
        } else{
            uart_puthex((unsigned int)pid);
        }
    }
    uart_puts("\n");
    uart_puts("EC=");
    uart_puthex((unsigned int)ec);
    uart_puts("\n");
    uart_puts("ESR_EL1=");
    uart_puthex((unsigned int)esr);
    uart_puts("\nELR_EL1=");
    uart_puthex((unsigned int)elr);
    uart_puts("\nSPSR_EL1=");
    uart_puthex((unsigned int)spsr);
    uart_puts("\nFAR_EL1=");
    uart_puthex((unsigned int)far);
    uart_puts("\n");
    if (ec == 0x24UL || ec == 0x25UL){
        unsigned long dfsc = esr & 0x3FUL;
        uart_puts("DFSC=");
        uart_puthex((unsigned int)dfsc);
        uart_puts(" WnR=");
        uart_puthex((unsigned int)((esr >> 6) & 1UL));
        uart_puts(" (");
        uart_puts(data_fault_name(dfsc));
        uart_puts(")");
        uart_puts("\n");
    }
    if (ec == 0x20UL || ec == 0x21UL){
        unsigned long ifsc = esr & 0x3FUL;
        uart_puts("IFSC=");
        uart_puthex((unsigned int)ifsc);
        uart_puts(" (");
        uart_puts(instr_fault_name(ifsc));
        uart_puts(")");
        uart_puts("\n");
    }
    dump_user_fault_layout(elr, far);
    if (ec == 0x00UL){
        uart_puts("INSN@ELR=");
        unsigned long insn_addr = (elr & ~0x3UL);
        if (process_user_range_readable((const void*)insn_addr, 4u)){
            volatile unsigned int* ip = (volatile unsigned int*)insn_addr;
            uart_puthex(*ip);
        } else{
            uart_puts("UNREADABLE");
        }
        uart_puts("\n");
    }

    if (get_current_process()){
        uart_puts("Fault in process; terminating current PID.\n");
        process_fault_current();
        void* next_sp = scheduler_on_irq(frame_sp);
        if (next_sp != frame_sp){
            return next_sp;
        }
        uart_puts("No alternate runnable frame after fault; entering recoverable core-idle.\n");
        process_enter_idle_loop();
    }

    uart_puts("Kernel-context exception; isolating this core into recoverable idle.\n");
    process_enter_idle_loop();
}
