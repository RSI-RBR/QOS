#define UART0_BASE 0x3F201000
#define MMIO_BASE 0x3F000000

#define UART0_DR (UART0_BASE + 0x00)
#define UART0_FR (UART0_BASE + 0x18)
#define UART0_IBRD (UART0_BASE + 0x24)
#define UART0_FBRD (UART0_BASE + 0x28)
#define UART0_LCRH (UART0_BASE + 0x2C)
#define UART0_CR (UART0_BASE + 0x30)

#define GPFSEL1 (MMIO_BASE + 0x200004)
#define GPPUD (MMIO_BASE + 0x200094)
#define GPPUDCLK0 (MMIO_BASE + 0x200098)

#include "spinlock.h"

static spinlock_t g_uart_lock;

static void delay(int count){
    while (count--) { asm volatile("nop"); }
}

static void uart_send_raw(char c){
    while (*(volatile unsigned int*)UART0_FR & (1 << 5)){}
    *(volatile unsigned int*)UART0_DR = c;
}

void uart_init(void){
    spinlock_init(&g_uart_lock);
    //disable uart
    *(volatile unsigned int*)UART0_CR = 0;

    unsigned int r = *(volatile unsigned int*)GPFSEL1;
    r &= ~((7 << 12) | (7 << 15));
    r |= (4 << 12) | (4 << 15);

    *(volatile unsigned int*)GPFSEL1 = r;

    *(volatile unsigned int*)GPPUD = 0;
    delay(150);
    *(volatile unsigned int*)GPPUDCLK0 = (1 << 14) | (1 << 15);
    delay(150);
    *(volatile unsigned int*)GPPUDCLK0 = 0;


    // baud rate setup
    *(volatile unsigned int*)UART0_IBRD = 26;
    *(volatile unsigned int*)UART0_FBRD = 3;

    *(volatile unsigned int*)UART0_LCRH = (1 << 4) | (3 << 5);

    // enable uart
    *(volatile unsigned int*)UART0_CR = (1 << 0) | (1 << 8) | (1 << 9);

}

void uart_send(char c){
    unsigned long irq = spin_lock_irqsave(&g_uart_lock);
    uart_send_raw(c);
    spin_unlock_irqrestore(&g_uart_lock, irq);
}

char uart_getc(void){
    while (*(volatile unsigned int*)UART0_FR & (1 << 4)){}
    return (char)(*(volatile unsigned int*)UART0_DR);
}

int uart_try_getc(char *c){
    if (*(volatile unsigned int*)UART0_FR & (1 << 4)){
        return 0;
    }

    *c = (char)(*(volatile unsigned int*)UART0_DR);
    return 1;
}

void uart_puts(const char* str){
    unsigned long irq = spin_lock_irqsave(&g_uart_lock);
    while (*str){
        if (*str == '\n'){
            uart_send_raw('\r');
        }
        uart_send_raw(*str++);
    }
    spin_unlock_irqrestore(&g_uart_lock, irq);
}

void uart_puthex(unsigned int val){
//    char hex[] = "0123456789ABCDEF";
//
//    for (int i = 28; i >= 0; i -= 4){
//        uart_send(hex[(val >> i) & 0xF]);
//    }
    unsigned long irq = spin_lock_irqsave(&g_uart_lock);
    for (int i = 28; i >= 0; i -= 4){
        unsigned int digit = (val >> i) & 0xF;

        if (digit < 10){
            uart_send_raw((char)('0' + digit));
        } else{
            uart_send_raw((char)('A' + digit - 10));
        }
    }
    spin_unlock_irqrestore(&g_uart_lock, irq);
}

void uart_putdec(unsigned long val){
    char buf[21];
    int i = 0;

    if (val == 0){
        unsigned long irq0 = spin_lock_irqsave(&g_uart_lock);
        uart_send_raw('0');
        spin_unlock_irqrestore(&g_uart_lock, irq0);
        return;
    }

    while (val > 0 && i < (int)sizeof(buf)){
        buf[i++] = (char)('0' + (val % 10UL));
        val /= 10UL;
    }

    unsigned long irq = spin_lock_irqsave(&g_uart_lock);
    while (i > 0){
        uart_send_raw(buf[--i]);
    }
    spin_unlock_irqrestore(&g_uart_lock, irq);
}

//void uart_puthex64(unsigned long value

