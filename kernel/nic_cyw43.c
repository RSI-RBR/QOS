#include "nic.h"
#include "cyw43.h"

static nic_rx_handler_t g_rx_handler = 0;

static void cyw43_nic_rx_bridge(const unsigned char* frame, unsigned int len){
    if (g_rx_handler){
        g_rx_handler(frame, len);
    }
}

static int cyw43_nic_set_rx_handler(nic_rx_handler_t handler){
    g_rx_handler = handler;
    return cyw43_net_set_rx_handler(cyw43_nic_rx_bridge);
}

static int cyw43_nic_init(void){
    return cyw43_net_ready() ? 0 : -1;
}

static int cyw43_nic_poll(void){
    return cyw43_net_poll();
}

static int cyw43_nic_send(const unsigned char* frame, unsigned int len){
    return cyw43_net_send_ethernet(frame, len);
}

static int cyw43_nic_link_up(void){
    return cyw43_net_link_up();
}

static const nic_driver_t g_cyw43_driver = {
    .name = "cyw43-sdio",
    .init = cyw43_nic_init,
    .poll = cyw43_nic_poll,
    .send = cyw43_nic_send,
    .set_rx_handler = cyw43_nic_set_rx_handler,
    .link_up = cyw43_nic_link_up
};

const nic_driver_t* nic_probe_cyw43(void){
    return &g_cyw43_driver;
}

