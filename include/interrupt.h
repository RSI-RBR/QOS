#ifndef INTERRUPT_H
#define INTERRUPT_H

void interrupt_init(void);
void enable_interrupts(void);
void disable_interrupts(void);
void kernel_preempt_enter(void);
void kernel_preempt_exit(void);
int kernel_preempt_enabled(void);
void* sync_exception_handler(void* frame_sp, unsigned long esr, unsigned long elr, unsigned long spsr);
void interrupt_prepare_return(void* frame_sp);
void interrupt_register_sdhost_irq(void (*handler)(void));
int interrupt_register_bank2_irq(unsigned int pending_bit_mask, void (*handler)(void));

#endif
