#include "usb_host.h"
#include "uart.h"

// Raspberry Pi USB OTG (DWC2) base on Pi 2/3 peripheral map.
#define USB_DWC2_BASE 0x3F980000UL

#define GOTGCTL   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x000))
#define GAHBCFG   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x008))
#define GUSBCFG   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x00C))
#define GRSTCTL   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x010))
#define GINTSTS   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x014))
#define GINTMSK   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x018))
#define GRXFSIZ   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x024))
#define GNPTXFSIZ (*(volatile unsigned int*)(USB_DWC2_BASE + 0x028))
#define GCCFG     (*(volatile unsigned int*)(USB_DWC2_BASE + 0x038))
#define GSNPSID   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x040))
#define HCFG      (*(volatile unsigned int*)(USB_DWC2_BASE + 0x400))
#define HFIR      (*(volatile unsigned int*)(USB_DWC2_BASE + 0x404))
#define HPRT0     (*(volatile unsigned int*)(USB_DWC2_BASE + 0x440))
#define PCGCTL    (*(volatile unsigned int*)(USB_DWC2_BASE + 0xE00))

// GRSTCTL bits
#define GRSTCTL_CSRST        (1u << 0)
#define GRSTCTL_AHB_IDLE     (1u << 31)

// GUSBCFG bits
#define GUSBCFG_FHMOD        (1u << 29)
#define GUSBCFG_FDMOD        (1u << 30)

// GAHBCFG bits
#define GAHBCFG_GLBL_INTR_EN (1u << 0)

// HPRT0 bits
#define HPRT0_PWR            (1u << 12)
#define HPRT0_CONN_STS       (1u << 0)
#define HPRT0_ENA            (1u << 2)
#define HPRT0_OVRCURR        (1u << 4)
#define HPRT0_RESET          (1u << 8)

static int g_usb_ready = 0;

static void spin_delay(unsigned int n){
    while (n--){
        asm volatile("nop");
    }
}

static int wait_mask_set(volatile unsigned int* reg, unsigned int mask, unsigned int loops){
    while (loops--){
        if ((*reg) & mask){
            return 0;
        }
    }
    return -1;
}

static int wait_mask_clear(volatile unsigned int* reg, unsigned int mask, unsigned int loops){
    while (loops--){
        if (((*reg) & mask) == 0){
            return 0;
        }
    }
    return -1;
}

int usb_host_init(void){
    g_usb_ready = 0;

    unsigned int id = GSNPSID;
    uart_puts("USB: GSNPSID=");
    uart_puthex(id);
    uart_puts("\n");

    if (id == 0 || id == 0xFFFFFFFFu){
        uart_puts("USB: DWC2 not responding.\n");
        return -1;
    }

    // Power/clock gate disable for the core.
    PCGCTL = 0;
    asm volatile("dsb sy");
    asm volatile("isb");

    if (wait_mask_set((volatile unsigned int*)&GRSTCTL, GRSTCTL_AHB_IDLE, 2000000) != 0){
        uart_puts("USB: AHB idle timeout.\n");
        return -1;
    }

    // Core soft reset.
    GRSTCTL |= GRSTCTL_CSRST;
    if (wait_mask_clear((volatile unsigned int*)&GRSTCTL, GRSTCTL_CSRST, 2000000) != 0){
        uart_puts("USB: core reset timeout.\n");
        return -1;
    }
    spin_delay(50000);

    // Force host mode.
    unsigned int gusbcfg = GUSBCFG;
    gusbcfg &= ~GUSBCFG_FDMOD;
    gusbcfg |= GUSBCFG_FHMOD;
    GUSBCFG = gusbcfg;
    asm volatile("dsb sy");
    asm volatile("isb");
    spin_delay(200000);

    // Basic FIFO defaults suitable for initial control transfer work.
    GRXFSIZ = 512;
    GNPTXFSIZ = (256u << 16) | 512u; // depth | start addr

    // Clear and mask interrupts for phase 1 polling path.
    GINTSTS = 0xFFFFFFFFu;
    GINTMSK = 0;
    GAHBCFG |= GAHBCFG_GLBL_INTR_EN;

    // Set full-speed PHY clock (safe default on many Pi bare-metal bring-ups).
    HCFG = 0x00000003u;
    (void)HFIR;

    // Enable port power, preserving write-1-to-clear bits.
    unsigned int hprt = HPRT0;
    hprt |= HPRT0_PWR;
    hprt &= ~HPRT0_RESET;
    HPRT0 = hprt;
    asm volatile("dsb sy");
    spin_delay(500000);

    g_usb_ready = 1;
    uart_puts("USB: host phase1 init OK.\n");
    usb_host_dump_state();
    return 0;
}

int usb_host_ready(void){
    return g_usb_ready;
}

void usb_host_dump_state(void){
    uart_puts("USB: HPRT0=");
    uart_puthex(HPRT0);
    uart_puts(" HCFG=");
    uart_puthex(HCFG);
    uart_puts(" GINTSTS=");
    uart_puthex(GINTSTS);
    uart_puts(" PCGCTL=");
    uart_puthex(PCGCTL);
    uart_puts("\n");
    if (HPRT0 & HPRT0_CONN_STS){
        uart_puts("USB: port device connected\n");
    } else{
        uart_puts("USB: no device on root port\n");
    }
    if (HPRT0 & HPRT0_ENA){
        uart_puts("USB: port enabled\n");
    }
    if (HPRT0 & HPRT0_OVRCURR){
        uart_puts("USB: overcurrent flagged\n");
    }
}

int usb_host_control_transfer(unsigned char dev_addr,
                              const usb_setup_packet_t* setup,
                              unsigned char* data,
                              unsigned int data_len,
                              int in_transfer){
    (void)dev_addr;
    (void)setup;
    (void)data;
    (void)data_len;
    (void)in_transfer;
    // Phase 2: implement channel/transfer scheduling for EP0 setup/data/status.
    return -1;
}
