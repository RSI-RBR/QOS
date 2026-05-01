#include "nic.h"
#include "usb_host.h"
#include "uart.h"

#define SMSC95XX_VID 0x0424u
#define SMSC95XX_PID 0xEC00u

#define SMSC95XX_REQ_WRITE_REG 0xA0u
#define SMSC95XX_REQ_READ_REG  0xA1u

#define SMSC95XX_REG_ID_REV 0x0000u

static nic_rx_handler_t g_rx_handler = 0;
static unsigned char g_dev_addr = 0;
static int g_ready = 0;
static unsigned int g_id_rev = 0;

static int smsc95xx_try_set_configuration(unsigned char dev_addr, unsigned char cfg_value){
    usb_setup_packet_t req;
    req.bmRequestType = 0x00; // OUT | standard | device
    req.bRequest = 0x09;      // SET_CONFIGURATION
    req.wValue = (unsigned short)cfg_value;
    req.wIndex = 0;
    req.wLength = 0;
    return usb_host_control_transfer(dev_addr, &req, 0, 0, 0);
}

static int smsc95xx_read_reg(unsigned short reg, unsigned int* out){
    if (!out || g_dev_addr == 0){
        return -1;
    }

    usb_setup_packet_t req;
    unsigned char data[4];
    req.bmRequestType = 0xC0; // IN | vendor | device
    req.bRequest = SMSC95XX_REQ_READ_REG;
    req.wValue = 0;
    req.wIndex = reg;
    req.wLength = 4;

    if (usb_host_control_transfer(g_dev_addr, &req, data, sizeof(data), 1) != 0){
        return -1;
    }

    *out = (unsigned int)data[0]
         | ((unsigned int)data[1] << 8)
         | ((unsigned int)data[2] << 16)
         | ((unsigned int)data[3] << 24);
    return 0;
}

static int smsc95xx_set_rx_handler(nic_rx_handler_t handler){
    g_rx_handler = handler;
    return 0;
}

static int smsc95xx_init(void){
    usb_root_device_info_t info;
    int child_cfg_ok = 0;
    unsigned char cfg_value = 0;
    g_ready = 0;
    g_dev_addr = 0;
    g_id_rev = 0;

    if (usb_host_get_root_device_info(&info) != 0){
        uart_puts("SMSC95XX: root device info unavailable\n");
        return -1;
    }
    if (!info.child_present){
        uart_puts("SMSC95XX: child missing\n");
        return -1;
    }
    if (info.child_vid != SMSC95XX_VID || info.child_pid != SMSC95XX_PID){
        uart_puts("SMSC95XX: child VID/PID mismatch\n");
        uart_puts("SMSC95XX: child vid=");
        uart_puthex(info.child_vid);
        uart_puts(" pid=");
        uart_puthex(info.child_pid);
        uart_puts("\n");
        return -1;
    }

    cfg_value = info.child_config_value ? info.child_config_value : 1u;
    child_cfg_ok = info.child_configured ? 1 : 0;
    if (!child_cfg_ok){
        uart_puts("SMSC95XX: child not configured, trying SET_CONFIGURATION...\n");
        if (smsc95xx_try_set_configuration(info.child_address, cfg_value) == 0){
            child_cfg_ok = 1;
            uart_puts("SMSC95XX: child SET_CONFIGURATION ok\n");
        } else{
            uart_puts("SMSC95XX: child SET_CONFIGURATION failed, probing anyway\n");
        }
    }

    g_dev_addr = info.child_address;
    uart_puts("SMSC95XX: probing dev addr=");
    uart_puthex(g_dev_addr);
    uart_puts(" cfg=");
    uart_puthex(cfg_value);
    uart_puts(child_cfg_ok ? " (ok)\n" : " (best-effort)\n");
    if (!child_cfg_ok){
        // Many devices reject vendor register access while unconfigured.
        return -1;
    }
    if (smsc95xx_read_reg(SMSC95XX_REG_ID_REV, &g_id_rev) != 0){
        uart_puts("SMSC95XX: ID_REV read failed\n");
        return -1;
    }

    uart_puts("SMSC95XX: ID_REV=");
    uart_puthex(g_id_rev);
    uart_puts("\n");
    g_ready = 1;
    return 0;
}

static int smsc95xx_poll(void){
    (void)g_rx_handler;
    return 0;
}

static int smsc95xx_send(const unsigned char* frame, unsigned int len){
    (void)frame;
    (void)len;
    // Bulk TX endpoint path not implemented yet.
    return -1;
}

static int smsc95xx_link_up(void){
    return g_ready ? 1 : 0;
}

static const nic_driver_t g_smsc95xx_driver = {
    .name = "smsc95xx-ctrl",
    .init = smsc95xx_init,
    .poll = smsc95xx_poll,
    .send = smsc95xx_send,
    .set_rx_handler = smsc95xx_set_rx_handler,
    .link_up = smsc95xx_link_up
};

const nic_driver_t* nic_probe_smsc95xx(void){
    usb_root_device_info_t info;
    if (usb_host_get_root_device_info(&info) != 0){
        return 0;
    }
    if (info.child_present &&
        info.child_vid == SMSC95XX_VID &&
        info.child_pid == SMSC95XX_PID){
        return &g_smsc95xx_driver;
    }
    return 0;
}
