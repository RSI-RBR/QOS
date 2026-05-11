#include "nic.h"
#include "platform/board_config.h"

#define STUB_Q_LEN 16
#define STUB_MAX_FRAME 1536

typedef struct {
    unsigned int len;
    unsigned char data[STUB_MAX_FRAME];
} stub_frame_t;

static stub_frame_t g_txq[STUB_Q_LEN];
static unsigned int g_head = 0;
static unsigned int g_tail = 0;
static unsigned int g_count = 0;
static nic_rx_handler_t g_rx = 0;

static int stub_init(void){
    g_head = 0;
    g_tail = 0;
    g_count = 0;
    return 0;
}

static int stub_set_rx_handler(nic_rx_handler_t handler){
    g_rx = handler;
    return 0;
}

static int stub_send(const unsigned char* frame, unsigned int len){
    if (!frame || len == 0 || len > STUB_MAX_FRAME){
        return -1;
    }
    if (g_count >= STUB_Q_LEN){
        return -1;
    }

    stub_frame_t* slot = &g_txq[g_tail];
    slot->len = len;
    for (unsigned int i = 0; i < len; i++){
        slot->data[i] = frame[i];
    }

    g_tail = (g_tail + 1) % STUB_Q_LEN;
    g_count++;
    return 0;
}

static int stub_poll(void){
    int delivered = 0;
    while (g_count > 0){
        stub_frame_t* slot = &g_txq[g_head];
        if (g_rx){
            g_rx(slot->data, slot->len);
            delivered++;
        }
        g_head = (g_head + 1) % STUB_Q_LEN;
        g_count--;
    }
    return delivered;
}

static int stub_link_up(void){
    return 1;
}

static const nic_driver_t g_stub_driver = {
    .name = "stub-loopback",
    .init = stub_init,
    .poll = stub_poll,
    .send = stub_send,
    .set_rx_handler = stub_set_rx_handler,
    .link_up = stub_link_up
};

const nic_driver_t* nic_probe_stub(void){
    return &g_stub_driver;
}

const nic_driver_t* nic_probe_default(void){
#if QOS_BOARD_HAS_USB_ETHERNET
    const nic_driver_t* smsc = nic_probe_smsc95xx();
    if (smsc){
        return smsc;
    }
#endif
#if QOS_BOARD_HAS_ONBOARD_WIFI
    const nic_driver_t* wifi = nic_probe_cyw43();
    if (wifi){
        return wifi;
    }
#endif
    return nic_probe_stub();
}
