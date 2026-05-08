#ifndef KLOG_H
#define KLOG_H

void klog_set_terminal_ready(int ready);
void klog_send(char c);
void klog_puts(const char* s);
void klog_puthex(unsigned int val);
void klog_putdec(unsigned long val);

#endif
