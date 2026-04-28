#ifndef UART_H
#define UART_H

void uart_init(void);
void uart_send(char c);
void uart_puts(const char* str);
char uart_getc(void);
void uart_puthex(unsigned int val);
int uart_try_getc(char *c);


#endif
