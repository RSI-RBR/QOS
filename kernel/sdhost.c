#include "uart.h"

#define SDHOST_BASE 0x3F202000

#define SDCMD   (*(volatile unsigned int*)(SDHOST_BASE + 0x00))
#define SDARG   (*(volatile unsigned int*)(SDHOST_BASE + 0x04))
#define SDTOUT  (*(volatile unsigned int*)(SDHOST_BASE + 0x08))
#define SDCDIV  (*(volatile unsigned int*)(SDHOST_BASE + 0x0C))
#define SDRSP0  (*(volatile unsigned int*)(SDHOST_BASE + 0x10))
#define SDHSTS  (*(volatile unsigned int*)(SDHOST_BASE + 0x20))
#define SDVDD   (*(volatile unsigned int*)(SDHOST_BASE + 0x30))

#define SDCMD_NEW_FLAG 0x8000
#define SDCMD_FAIL_FLAG 0x4000

static void delay(int count) {
    while (count--) asm volatile("nop");
}
