#ifndef TIMER_H
#define TIMER_H

extern volatile unsigned long system_ticks;

void timer_init(void);
void timer_clear_interrupt(void);

void timer_handler(void);

#endif
