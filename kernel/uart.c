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


static void delay(int count){
    while (count--) { asm volatile("nop"); }
}

void uart_init(void){
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
    while (*(volatile unsigned int*)UART0_FR & (1 << 5)){}
    *(volatile unsigned int*)UART0_DR = c;
}

char uart_getc(void){
    while (*(volatile unsigned int*)UART0_FR & (1 << 4)){}
    return (char)(*(volatile unsigned int*)UART0_DR);
}

void uart_puts(const char* str){
    while (*str){
        if (*str == '\n') uart_send('\r');
        uart_send(*str++);
    }
}

void uart_puthex(unsigned int val){
    char hex[] = "0123456789ABCDEF";

    for (int i = 28; i >= 0; i -= 4){
        uart_send(hex[(val >> i) & 0xF]);
    }
}
