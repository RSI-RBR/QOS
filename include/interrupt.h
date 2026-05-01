#ifndef INTERRUPT_H
#define INTERRUPT_H

void interrupt_init(void);
void enable_interrupts(void);
void disable_interrupts(void);
void* sync_exception_handler(void* frame_sp, unsigned long esr, unsigned long elr, unsigned long spsr);
void interrupt_register_sdhost_irq(void (*handler)(void));
int interrupt_register_bank2_irq(unsigned int pending_bit_mask, void (*handler)(void));

#endif
