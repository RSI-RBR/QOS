#include "interrupt.h"
#include "uart.h"
#include "timer.h"
#include "process.h"
#include "syscall.h"
#include "net.h"
#include "arp.h"
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
static volatile unsigned int g_kernel_preempt_depth = 0;
static volatile unsigned long g_kernel_preempt_saved_daif = 0;

#define MAX_BANK2_IRQ_HANDLERS 8
static unsigned int g_bank2_irq_bits[MAX_BANK2_IRQ_HANDLERS];
static void (*g_bank2_irq_handlers[MAX_BANK2_IRQ_HANDLERS])(void);

// Trap-frame index from vectors.S layout:
// x0..x30 => 0..30, ELR => 31, SPSR => 32, SP_EL0 => 33
#define IRQ_FRAME_SPSR_IDX 32
#define SPSR_MODE_MASK 0xFUL
#define SPSR_MODE_EL0T 0x0UL

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
    asm volatile("mrs %0, daif" : "=r"(daif_prev));
    asm volatile("msr daifset, #2");
    if (g_kernel_preempt_depth == 0){
        g_kernel_preempt_saved_daif = daif_prev;
    }
    g_kernel_preempt_depth++;
    // Sync exceptions (SVC) typically enter EL1 with IRQ masked. Unmask IRQ
    // inside opted-in long kernel paths so timer IRQ can drive scheduling.
    asm volatile("msr daifclr, #2" : : : "memory");
}

void kernel_preempt_exit(void){
    unsigned long restore_daif = 0;
    asm volatile("msr daifset, #2");
    if (g_kernel_preempt_depth > 0){
        g_kernel_preempt_depth--;
        if (g_kernel_preempt_depth == 0){
            restore_daif = g_kernel_preempt_saved_daif;
        }
    }
    if (g_kernel_preempt_depth == 0){
        asm volatile("msr daif, %0" : : "r"(restore_daif) : "memory");
    } else{
        // Keep IRQ enabled for outer opted-in region.
        asm volatile("msr daifclr, #2" : : : "memory");
    }
}

int kernel_preempt_enabled(void){
    return g_kernel_preempt_depth > 0 ? 1 : 0;
}

void* irq_handler(void* irq_frame_sp){
    unsigned int core = cpu_get_id();
    unsigned int local_src = CORE_IRQ_SOURCE(core);
    if (local_src & CORE_CNTPNSIRQ_PENDING){
        timer_clear_interrupt();
        if (core == 0){
            timer_handler();
            arp_periodic_tick(system_ticks);
            // Keep timer IRQ short and non-blocking; NIC polling can stall
            // and starve scheduling when done in interrupt context.
            // Networking paths invoke net_poll() from syscall/foreground flow.
        } else{
            scheduler_tick();
        }
        // By default, preempt only EL0 user-mode. For selected long-running
        // kernel paths, syscall code can opt in to cooperative EL1 preemption.
        unsigned long* frame = (unsigned long*)irq_frame_sp;
        unsigned long spsr = frame ? frame[IRQ_FRAME_SPSR_IDX] : 0;
        int need_resched = scheduler_consume_need_resched();
        int in_el0 = ((spsr & SPSR_MODE_MASK) == SPSR_MODE_EL0T);
        process_t* cur = get_current_process();
        int sleeping_syscall = (cur && cur->state == PROC_SLEEPING);
        int idle_kernel = (cur == 0);
        if (need_resched && (in_el0 || kernel_preempt_enabled() || idle_kernel || sleeping_syscall)){
            return scheduler_on_irq(irq_frame_sp);
        }
        return irq_frame_sp;
    }

    if (local_src & CORE_MAILBOX0_PENDING){
        smp_clear_ipi_for_core(core);
        unsigned long* frame = (unsigned long*)irq_frame_sp;
        unsigned long spsr = frame ? frame[IRQ_FRAME_SPSR_IDX] : 0;
        int need_resched = scheduler_consume_need_resched();
        int in_el0 = ((spsr & SPSR_MODE_MASK) == SPSR_MODE_EL0T);
        process_t* cur = get_current_process();
        int sleeping_syscall = (cur && cur->state == PROC_SLEEPING);
        int idle_kernel = (cur == 0);
        if (need_resched && (in_el0 || kernel_preempt_enabled() || idle_kernel || sleeping_syscall)){
            return scheduler_on_irq(irq_frame_sp);
        }
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
                return irq_frame_sp;
            }
        }
        // Backward compatibility fallback if legacy SDHOST registration is used.
        if ((bank2_pending & IRQ_SDHOST_PENDING_BIT) && g_sdhost_irq_handler){
            g_sdhost_irq_handler();
            return irq_frame_sp;
        }
    }

    return irq_frame_sp;
}

void* sync_exception_handler(void* frame_sp, unsigned long esr, unsigned long elr, unsigned long spsr){
    unsigned long ec = (esr >> 26) & 0x3FUL;
    unsigned long far = 0;
    asm volatile("mrs %0, far_el1" : "=r"(far));

    if (ec == 0x15UL){
        return syscall_handle(frame_sp, esr);
    }

    uart_puts("\nSYNC EXCEPTION\n");
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
    if (ec == 0x00UL){
        volatile unsigned int* ip = (volatile unsigned int*)(elr & ~0x3UL);
        uart_puts("INSN@ELR=");
        uart_puthex(*ip);
        uart_puts("\n");
    }

    if (get_current_process()){
        uart_puts("Fault in process; terminating current PID.\n");
        process_fault_current();
        void* next_sp = scheduler_on_irq(frame_sp);
        if (next_sp != frame_sp){
            return next_sp;
        }
        uart_puts("No alternate runnable frame after fault.\n");
        process_dump();
    }

    uart_puts("HALTING\n");
    while (1){}
}
