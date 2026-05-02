#include "nic.h"
#include "usb_host.h"
#include "uart.h"
#include "ethernet.h"

#define SMSC95XX_VID 0x0424u
#define SMSC95XX_PID 0xEC00u

#define SMSC95XX_REQ_WRITE_REG 0xA0u
#define SMSC95XX_REQ_READ_REG  0xA1u

#define SMSC95XX_REG_ID_REV 0x0000u
#define SMSC95XX_REG_INT_STS 0x0008u
#define SMSC95XX_REG_RX_CFG 0x000Cu
#define SMSC95XX_REG_TX_CFG 0x0010u
#define SMSC95XX_REG_HW_CFG 0x0014u
#define SMSC95XX_REG_PM_CTRL 0x0020u
#define SMSC95XX_REG_AFC_CFG 0x002Cu
#define SMSC95XX_REG_BURST_CAP 0x0038u
#define SMSC95XX_REG_BULK_IN_DLY 0x006Cu
#define SMSC95XX_REG_MAC_CR 0x0100u
#define SMSC95XX_REG_ADDRH 0x0104u
#define SMSC95XX_REG_ADDRL 0x0108u
#define SMSC95XX_REG_FLOW 0x011Cu

#define SMSC95XX_INT_STS_CLEAR_ALL 0xFFFFFFFFu
#define SMSC95XX_RX_FIFO_FLUSH     0x00000001u
#define SMSC95XX_TX_CFG_ON         0x00000004u
#define SMSC95XX_TX_FIFO_FLUSH     0x00000001u
#define SMSC95XX_HW_CFG_BIR        0x00001000u
#define SMSC95XX_HW_CFG_RXDOFF     0x00000600u
#define SMSC95XX_HW_CFG_MEF        0x00000020u
#define SMSC95XX_HW_CFG_BCE        0x00000002u
#define SMSC95XX_HW_CFG_LRST       0x00000008u
#define SMSC95XX_PM_CTL_PHY_RST    0x00000010u
#define SMSC95XX_MAC_CR_PRMS       0x00040000u
#define SMSC95XX_MAC_CR_FDPX       0x00100000u
#define SMSC95XX_MAC_CR_TXEN       0x00000008u
#define SMSC95XX_MAC_CR_RXEN       0x00000004u
#define SMSC95XX_AFC_CFG_DEFAULT   0x00F830A1u
#define SMSC95XX_HS_BURST_CAP      37u

#define SMSC95XX_TX_CMD_A_FIRST_SEG 0x00002000u
#define SMSC95XX_TX_CMD_A_LAST_SEG  0x00001000u
#define SMSC95XX_TX_CMD_A_BUF_SIZE  0x000007FFu
#define SMSC95XX_TX_CMD_B_FRAME_LEN 0x000007FFu
#define SMSC95XX_RX_STS_FRAME_LEN   0x3FFF0000u
#define SMSC95XX_RX_STS_ERROR       0x00008000u
#define SMSC95XX_RX_STS_LEN_SHIFT   16u

#define SMSC95XX_RX_BUF_SIZE 2048u
#define SMSC95XX_TX_BUF_SIZE 1600u

static nic_rx_handler_t g_rx_handler = 0;
static unsigned char g_dev_addr = 0;
static unsigned char g_bulk_in_ep = 0;
static unsigned char g_bulk_out_ep = 0;
static unsigned short g_bulk_in_mps = 0;
static unsigned short g_bulk_out_mps = 0;
static int g_ready = 0;
static unsigned int g_id_rev = 0;
static unsigned char g_mac[ETH_ADDR_LEN] = {0x02, 0x51, 0x4F, 0x53, 0x00, 0x01};
static unsigned char g_rx_buf[SMSC95XX_RX_BUF_SIZE] __attribute__((aligned(64)));
static unsigned char g_tx_buf[SMSC95XX_TX_BUF_SIZE] __attribute__((aligned(64)));

static void smsc95xx_delay(unsigned int n){
    while (n--){
        asm volatile("nop");
    }
}

static int smsc95xx_try_set_configuration(unsigned char dev_addr, unsigned char cfg_value){
    usb_setup_packet_t req;
    req.bmRequestType = 0x00; // OUT | standard | device
    req.bRequest = 0x09;      // SET_CONFIGURATION
    req.wValue = (unsigned short)cfg_value;
    req.wIndex = 0;
    req.wLength = 0;
    return usb_host_control_transfer(dev_addr, &req, 0, 0, 0);
}

static int smsc95xx_get_configuration(unsigned char dev_addr, unsigned char* out_cfg){
    usb_setup_packet_t req;
    unsigned char cfg = 0;
    if (!out_cfg){
        return -1;
    }
    req.bmRequestType = 0x80; // IN | standard | device
    req.bRequest = 0x08;      // GET_CONFIGURATION
    req.wValue = 0;
    req.wIndex = 0;
    req.wLength = 1;
    if (usb_host_control_transfer(dev_addr, &req, &cfg, 1, 1) != 0){
        return -1;
    }
    *out_cfg = cfg;
    return 0;
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

    for (unsigned int attempt = 0; attempt < 3; attempt++){
        if (usb_host_control_transfer(g_dev_addr, &req, data, sizeof(data), 1) == 0){
            *out = (unsigned int)data[0]
                 | ((unsigned int)data[1] << 8)
                 | ((unsigned int)data[2] << 16)
                 | ((unsigned int)data[3] << 24);
            return 0;
        }
        smsc95xx_delay(200000);
    }
    return -1;
}

static int smsc95xx_write_reg(unsigned short reg, unsigned int value){
    if (g_dev_addr == 0){
        return -1;
    }

    usb_setup_packet_t req;
    unsigned char data[4];
    data[0] = (unsigned char)(value & 0xFFu);
    data[1] = (unsigned char)((value >> 8) & 0xFFu);
    data[2] = (unsigned char)((value >> 16) & 0xFFu);
    data[3] = (unsigned char)((value >> 24) & 0xFFu);

    req.bmRequestType = 0x40; // OUT | vendor | device
    req.bRequest = SMSC95XX_REQ_WRITE_REG;
    req.wValue = 0;
    req.wIndex = reg;
    req.wLength = 4;

    for (unsigned int attempt = 0; attempt < 3; attempt++){
        if (usb_host_control_transfer(g_dev_addr, &req, data, sizeof(data), 0) == 0){
            return 0;
        }
        smsc95xx_delay(200000);
    }
    return -1;
}

static int smsc95xx_wait_reg_clear(unsigned short reg, unsigned int mask){
    unsigned int value = 0;
    for (unsigned int i = 0; i < 100; i++){
        smsc95xx_delay(500000);
        if (smsc95xx_read_reg(reg, &value) != 0){
            return -1;
        }
        if ((value & mask) == 0){
            return 0;
        }
    }
    return -1;
}

static void smsc95xx_write_le32(unsigned char* p, unsigned int v){
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static unsigned int smsc95xx_read_le32(const unsigned char* p){
    return (unsigned int)p[0]
        | ((unsigned int)p[1] << 8)
        | ((unsigned int)p[2] << 16)
        | ((unsigned int)p[3] << 24);
}

static int smsc95xx_start_chip(void){
    unsigned int value = 0;

    // Linux/Circle-style bring-up: reset the lite block, set burst/RX behavior,
    // program a locally administered MAC, then enable MAC TX/RX and USB TX.
    if (smsc95xx_write_reg(SMSC95XX_REG_HW_CFG, SMSC95XX_HW_CFG_LRST) == 0){
        (void)smsc95xx_wait_reg_clear(SMSC95XX_REG_HW_CFG, SMSC95XX_HW_CFG_LRST);
    }
    if (smsc95xx_write_reg(SMSC95XX_REG_PM_CTRL, SMSC95XX_PM_CTL_PHY_RST) == 0){
        (void)smsc95xx_wait_reg_clear(SMSC95XX_REG_PM_CTRL, SMSC95XX_PM_CTL_PHY_RST);
    }

    if (smsc95xx_write_reg(SMSC95XX_REG_INT_STS, SMSC95XX_INT_STS_CLEAR_ALL) != 0){
        return -1;
    }
    (void)smsc95xx_write_reg(SMSC95XX_REG_RX_CFG, SMSC95XX_RX_FIFO_FLUSH);
    (void)smsc95xx_write_reg(SMSC95XX_REG_TX_CFG, SMSC95XX_TX_FIFO_FLUSH);
    smsc95xx_delay(1000000);

    if (smsc95xx_write_reg(SMSC95XX_REG_BURST_CAP, SMSC95XX_HS_BURST_CAP) != 0){
        return -1;
    }
    if (smsc95xx_write_reg(SMSC95XX_REG_BULK_IN_DLY, 0) != 0){
        return -1;
    }

    if (smsc95xx_read_reg(SMSC95XX_REG_HW_CFG, &value) != 0){
        return -1;
    }
    value |= SMSC95XX_HW_CFG_BIR | SMSC95XX_HW_CFG_MEF | SMSC95XX_HW_CFG_BCE;
    value &= ~SMSC95XX_HW_CFG_RXDOFF;
    value |= (2u << 9); // Align received IP payloads on a word boundary.
    if (smsc95xx_write_reg(SMSC95XX_REG_HW_CFG, value) != 0){
        return -1;
    }

    unsigned int addrl = (unsigned int)g_mac[0]
        | ((unsigned int)g_mac[1] << 8)
        | ((unsigned int)g_mac[2] << 16)
        | ((unsigned int)g_mac[3] << 24);
    unsigned int addrh = (unsigned int)g_mac[4] | ((unsigned int)g_mac[5] << 8);
    if (smsc95xx_write_reg(SMSC95XX_REG_ADDRL, addrl) != 0 ||
        smsc95xx_write_reg(SMSC95XX_REG_ADDRH, addrh) != 0){
        return -1;
    }

    (void)smsc95xx_write_reg(SMSC95XX_REG_FLOW, 0);
    (void)smsc95xx_write_reg(SMSC95XX_REG_AFC_CFG, SMSC95XX_AFC_CFG_DEFAULT);

    if (smsc95xx_read_reg(SMSC95XX_REG_MAC_CR, &value) != 0){
        return -1;
    }
    value |= SMSC95XX_MAC_CR_TXEN | SMSC95XX_MAC_CR_RXEN |
             SMSC95XX_MAC_CR_PRMS | SMSC95XX_MAC_CR_FDPX;
    if (smsc95xx_write_reg(SMSC95XX_REG_MAC_CR, value) != 0){
        return -1;
    }
    if (smsc95xx_write_reg(SMSC95XX_REG_TX_CFG, SMSC95XX_TX_CFG_ON) != 0){
        return -1;
    }

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
    unsigned char active_cfg = 0;
    int id_ok = 0;
    g_ready = 0;
    g_dev_addr = 0;
    g_bulk_in_ep = 0;
    g_bulk_out_ep = 0;
    g_bulk_in_mps = 0;
    g_bulk_out_mps = 0;
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
    g_dev_addr = info.child_address;
    g_bulk_in_ep = info.child_bulk_in_ep;
    g_bulk_out_ep = info.child_bulk_out_ep;
    g_bulk_in_mps = info.child_bulk_in_mps;
    g_bulk_out_mps = info.child_bulk_out_mps;
    uart_puts("SMSC95XX: probing dev addr=");
    uart_puthex(g_dev_addr);
    uart_puts(" cfg=");
    uart_puthex(cfg_value);
    uart_puts(child_cfg_ok ? " (cached)\n" : " (unknown)\n");
    uart_puts("SMSC95XX: bulk in=");
    uart_puthex(g_bulk_in_ep);
    uart_puts(" mps=");
    uart_puthex(g_bulk_in_mps);
    uart_puts(" out=");
    uart_puthex(g_bulk_out_ep);
    uart_puts(" mps=");
    uart_puthex(g_bulk_out_mps);
    uart_puts("\n");

    // Fast path: many LAN95xx children are already configured when reached via
    // the LAN9514 hub. Avoid extra EP0 traffic until needed.
    if (smsc95xx_read_reg(SMSC95XX_REG_ID_REV, &g_id_rev) == 0){
        id_ok = 1;
    } else{
        if (!child_cfg_ok){
            uart_puts("SMSC95XX: child not configured, trying SET_CONFIGURATION...\n");
            if (smsc95xx_try_set_configuration(info.child_address, cfg_value) == 0){
                child_cfg_ok = 1;
                uart_puts("SMSC95XX: child SET_CONFIGURATION ok\n");
            } else{
                uart_puts("SMSC95XX: child SET_CONFIGURATION failed, probing anyway\n");
            }
        }
        if (smsc95xx_get_configuration(info.child_address, &active_cfg) == 0){
            uart_puts("SMSC95XX: child active configuration=");
            uart_puthex(active_cfg);
            uart_puts("\n");
            if (active_cfg != 0){
                child_cfg_ok = 1;
            }
        } else{
            uart_puts("SMSC95XX: GET_CONFIGURATION failed\n");
        }

        if (smsc95xx_read_reg(SMSC95XX_REG_ID_REV, &g_id_rev) == 0){
            id_ok = 1;
        }
    }

    if (!id_ok){
        uart_puts("SMSC95XX: ID_REV read failed\n");
        return -1;
    }

    uart_puts("SMSC95XX: ID_REV=");
    uart_puthex(g_id_rev);
    uart_puts("\n");
    if (!g_bulk_in_ep || !g_bulk_out_ep || !g_bulk_in_mps || !g_bulk_out_mps){
        uart_puts("SMSC95XX: bulk endpoints missing\n");
        return -1;
    }
    if (smsc95xx_start_chip() != 0){
        uart_puts("SMSC95XX: chip start failed\n");
        return -1;
    }
    uart_puts("SMSC95XX: MAC=");
    for (unsigned int i = 0; i < ETH_ADDR_LEN; i++){
        uart_puthex(g_mac[i]);
        if (i + 1 < ETH_ADDR_LEN){
            uart_puts(":");
        }
    }
    uart_puts("\n");
    g_ready = 1;
    return 0;
}

static int smsc95xx_poll(void){
    if (!g_ready || !g_rx_handler){
        return 0;
    }

    int delivered = 0;
    for (unsigned int poll = 0; poll < 4; poll++){
        int n = usb_host_bulk_transfer(g_dev_addr, g_bulk_in_ep, g_bulk_in_mps,
                                       g_rx_buf, sizeof(g_rx_buf), 1);
        if (n <= 0){
            break;
        }

        unsigned int off = 0;
        while (off + 4u <= (unsigned int)n){
            unsigned int status = smsc95xx_read_le32(&g_rx_buf[off]);
            unsigned int rx_len = (status & SMSC95XX_RX_STS_FRAME_LEN) >> SMSC95XX_RX_STS_LEN_SHIFT;
            off += 4;
            if (rx_len < 4 || off + rx_len > (unsigned int)n){
                break;
            }
            if ((status & SMSC95XX_RX_STS_ERROR) == 0){
                unsigned int frame_len = rx_len - 4u; // Strip Ethernet FCS.
                if (frame_len > 0 && frame_len <= ETH_MAX_FRAME_LEN){
                    g_rx_handler(&g_rx_buf[off], frame_len);
                    delivered++;
                }
            }
            off += (rx_len + 3u) & ~3u;
        }
    }

    return delivered;
}

static int smsc95xx_send(const unsigned char* frame, unsigned int len){
    if (!g_ready || !frame || len == 0 || len > ETH_MAX_FRAME_LEN){
        return -1;
    }

    unsigned int frame_len = len;
    if (frame_len < ETH_MIN_FRAME_LEN){
        frame_len = ETH_MIN_FRAME_LEN;
    }
    unsigned int total = frame_len + 8u;
    if (total > sizeof(g_tx_buf)){
        return -1;
    }

    for (unsigned int i = 0; i < total; i++){
        g_tx_buf[i] = 0;
    }

    unsigned int tx_cmd_a = SMSC95XX_TX_CMD_A_FIRST_SEG |
        SMSC95XX_TX_CMD_A_LAST_SEG |
        (frame_len & SMSC95XX_TX_CMD_A_BUF_SIZE);
    unsigned int tx_cmd_b = frame_len & SMSC95XX_TX_CMD_B_FRAME_LEN;
    smsc95xx_write_le32(&g_tx_buf[0], tx_cmd_a);
    smsc95xx_write_le32(&g_tx_buf[4], tx_cmd_b);
    for (unsigned int i = 0; i < len; i++){
        g_tx_buf[8 + i] = frame[i];
    }

    int sent = usb_host_bulk_transfer(g_dev_addr, g_bulk_out_ep, g_bulk_out_mps,
                                      g_tx_buf, total, 0);
    return (sent == (int)total) ? 0 : -1;
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
