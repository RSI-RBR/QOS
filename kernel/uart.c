#define UART0_BASE 0x3F201000
#define MMIO_BASE 0x3F000000

#define UART0_DR (UART0_BASE + 0x00)
#define UART0_RSR_ECR (UART0_BASE + 0x04)
#define UART0_FR (UART0_BASE + 0x18)
#define UART0_IBRD (UART0_BASE + 0x24)
#define UART0_FBRD (UART0_BASE + 0x28)
#define UART0_LCRH (UART0_BASE + 0x2C)
#define UART0_CR (UART0_BASE + 0x30)

#define GPFSEL1 (MMIO_BASE + 0x200004)
#define GPPUD (MMIO_BASE + 0x200094)
#define GPPUDCLK0 (MMIO_BASE + 0x200098)

#define GPIO14_TXD0 (1u << 14)
#define GPIO15_RXD0 (1u << 15)
#define GPPUD_OFF 0u
#define GPPUD_PULLUP 2u
#define UART_DR_ERROR_MASK 0xF00u

static void delay(int count){
    while (count--) { asm volatile("nop"); }
}

static void gpio_set_pull(unsigned int pins, unsigned int pull){
    *(volatile unsigned int*)GPPUD = pull;
    delay(150);
    *(volatile unsigned int*)GPPUDCLK0 = pins;
    delay(150);
    *(volatile unsigned int*)GPPUDCLK0 = 0;
    *(volatile unsigned int*)GPPUD = GPPUD_OFF;
}

static void uart_send_raw(char c){
    while (*(volatile unsigned int*)UART0_FR & (1 << 5)){}
    *(volatile unsigned int*)UART0_DR = c;
}

void uart_init(void){
    //disable uart
    *(volatile unsigned int*)UART0_CR = 0;

    unsigned int r = *(volatile unsigned int*)GPFSEL1;
    r &= ~((7 << 12) | (7 << 15));
    r |= (4 << 12) | (4 << 15);

    *(volatile unsigned int*)GPFSEL1 = r;

    /*
     * Keep RXD0 at the UART idle level when the USB serial adapter is
     * disconnected or unpowered. Without this, a floating RX pin can inject
     * garbage input into the shell while TX still works normally.
     */
    gpio_set_pull(GPIO14_TXD0, GPPUD_OFF);
    gpio_set_pull(GPIO15_RXD0, GPPUD_PULLUP);

    // baud rate setup
    *(volatile unsigned int*)UART0_IBRD = 26;
    *(volatile unsigned int*)UART0_FBRD = 3;

    *(volatile unsigned int*)UART0_LCRH = (1 << 4) | (3 << 5);

    // enable uart
    *(volatile unsigned int*)UART0_CR = (1 << 0) | (1 << 8) | (1 << 9);

}

void uart_send(char c){
    uart_send_raw(c);
}

char uart_getc(void){
    for (;;){
        while (*(volatile unsigned int*)UART0_FR & (1 << 4)){}
        unsigned int dr = *(volatile unsigned int*)UART0_DR;
        if ((dr & UART_DR_ERROR_MASK) == 0u){
            return (char)(dr & 0xFFu);
        }
        *(volatile unsigned int*)UART0_RSR_ECR = 0u;
    }
}

int uart_try_getc(char *c){
    if (*(volatile unsigned int*)UART0_FR & (1 << 4)){
        return 0;
    }

    unsigned int dr = *(volatile unsigned int*)UART0_DR;
    if (dr & UART_DR_ERROR_MASK){
        *(volatile unsigned int*)UART0_RSR_ECR = 0u;
        return 0;
    }

    *c = (char)(dr & 0xFFu);
    return 1;
}

void uart_puts(const char* str){
    while (*str){
        if (*str == '\n'){
            uart_send_raw('\r');
        }
        uart_send_raw(*str++);
    }
}

void uart_puthex(unsigned int val){
//    char hex[] = "0123456789ABCDEF";
//
//    for (int i = 28; i >= 0; i -= 4){
//        uart_send(hex[(val >> i) & 0xF]);
//    }
    for (int i = 28; i >= 0; i -= 4){
        unsigned int digit = (val >> i) & 0xF;

        if (digit < 10){
            uart_send_raw((char)('0' + digit));
        } else{
            uart_send_raw((char)('A' + digit - 10));
        }
    }
}

void uart_putdec(unsigned long val){
    char buf[21];
    int i = 0;

    if (val == 0){
        uart_send_raw('0');
        return;
    }

    while (val > 0 && i < (int)sizeof(buf)){
        buf[i++] = (char)('0' + (val % 10UL));
        val /= 10UL;
    }

    while (i > 0){
        uart_send_raw(buf[--i]);
    }
}

//void uart_puthex64(unsigned long value

