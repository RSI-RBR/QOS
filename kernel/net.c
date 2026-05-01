#include "net.h"
#include "nic.h"
#include "uart.h"
#include "ethernet.h"

#define NET_RX_QUEUE_LEN 32

typedef struct {
    unsigned int len;
    unsigned char data[NET_MAX_FRAME_SIZE];
} net_frame_t;

typedef struct {
    net_frame_t frames[NET_RX_QUEUE_LEN];
    unsigned int head;
    unsigned int tail;
    unsigned int count;
} net_rx_queue_t;

typedef struct {
    unsigned long rx_ok;
    unsigned long rx_drop;
    unsigned long tx_ok;
    unsigned long tx_fail;
} net_stats_t;

static const nic_driver_t* g_nic = 0;
static net_rx_callback_t g_rx_cb = 0;
static net_rx_queue_t g_rxq;
static net_stats_t g_stats;
static int g_net_ready = 0;

static void net_rxq_reset(void){
    g_rxq.head = 0;
    g_rxq.tail = 0;
    g_rxq.count = 0;
}

static int net_rxq_push(const unsigned char* frame, unsigned int len){
    if (!frame || len == 0 || len > NET_MAX_FRAME_SIZE){
        return -1;
    }
    if (g_rxq.count >= NET_RX_QUEUE_LEN){
        return -1;
    }

    net_frame_t* slot = &g_rxq.frames[g_rxq.tail];
    slot->len = len;
    for (unsigned int i = 0; i < len; i++){
        slot->data[i] = frame[i];
    }

    g_rxq.tail = (g_rxq.tail + 1) % NET_RX_QUEUE_LEN;
    g_rxq.count++;
    return 0;
}

static int net_rxq_pop(net_frame_t* out){
    if (!out || g_rxq.count == 0){
        return -1;
    }

    *out = g_rxq.frames[g_rxq.head];
    g_rxq.head = (g_rxq.head + 1) % NET_RX_QUEUE_LEN;
    g_rxq.count--;
    return 0;
}

void net_ingest_rx_from_driver(const unsigned char* frame, unsigned int len){
    if (net_rxq_push(frame, len) == 0){
        g_stats.rx_ok++;
    } else{
        g_stats.rx_drop++;
    }
}

void net_set_rx_callback(net_rx_callback_t cb){
    g_rx_cb = cb;
}

int net_init(void){
    g_nic = nic_probe_default();
    net_rxq_reset();
    g_stats.rx_ok = 0;
    g_stats.rx_drop = 0;
    g_stats.tx_ok = 0;
    g_stats.tx_fail = 0;
    g_net_ready = 0;

    if (!g_nic){
        uart_puts("NET: no NIC backend\n");
        return -1;
    }
    if (!g_nic->init || !g_nic->poll || !g_nic->send || !g_nic->set_rx_handler){
        uart_puts("NET: NIC backend incomplete\n");
        return -1;
    }
    if (g_nic->set_rx_handler(net_ingest_rx_from_driver) != 0){
        uart_puts("NET: NIC RX hook failed\n");
        return -1;
    }
    if (g_nic->init() != 0){
        uart_puts("NET: NIC init failed\n");
        return -1;
    }

    g_net_ready = 1;
    uart_puts("NET: initialized with driver ");
    uart_puts(g_nic->name ? g_nic->name : "unknown");
    uart_puts("\n");
    return 0;
}

int net_ready(void){
    return g_net_ready;
}

const char* net_driver_name(void){
    if (!g_nic || !g_nic->name){
        return "none";
    }
    return g_nic->name;
}

int net_link_up(void){
    if (!g_net_ready || !g_nic || !g_nic->link_up){
        return 0;
    }
    return g_nic->link_up();
}

int net_send_raw(const unsigned char* frame, unsigned int len){
    if (!g_net_ready || !g_nic){
        return -1;
    }
    if (!frame || len == 0 || len > NET_MAX_FRAME_SIZE){
        return -1;
    }

    if (g_nic->send(frame, len) == 0){
        g_stats.tx_ok++;
        return 0;
    }
    g_stats.tx_fail++;
    return -1;
}

int net_recv_raw(unsigned char* out, unsigned int out_cap){
    net_frame_t frame;
    if (!out || out_cap == 0){
        return -1;
    }
    if (net_rxq_pop(&frame) != 0){
        return 0;
    }

    unsigned int n = frame.len;
    if (n > out_cap){
        n = out_cap;
    }
    for (unsigned int i = 0; i < n; i++){
        out[i] = frame.data[i];
    }
    return (int)n;
}

int net_poll(void){
    if (!g_net_ready || !g_nic){
        return 0;
    }

    if (g_nic->poll){
        g_nic->poll();
    }

    if (!g_rx_cb){
        return 0;
    }

    int delivered = 0;
    net_frame_t frame;
    while (net_rxq_pop(&frame) == 0){
        g_rx_cb(frame.data, frame.len);
        delivered++;
    }
    return delivered;
}

int net_send_test_frame(void){
    unsigned char frame[64];
    for (unsigned int i = 0; i < sizeof(frame); i++){
        frame[i] = 0;
    }

    // Locally administered unicast MAC addresses.
    frame[0] = 0x02; frame[1] = 0x00; frame[2] = 0x00; frame[3] = 0x00; frame[4] = 0x00; frame[5] = 0x01;
    frame[6] = 0x02; frame[7] = 0x00; frame[8] = 0x00; frame[9] = 0x00; frame[10] = 0x00; frame[11] = 0x02;
    frame[12] = (unsigned char)(ETH_TYPE_ARP >> 8);
    frame[13] = (unsigned char)(ETH_TYPE_ARP & 0xFF);
    frame[14] = 'Q';
    frame[15] = 'O';
    frame[16] = 'S';

    return net_send_raw(frame, sizeof(frame));
}

void net_dump_stats(void){
    uart_puts("NET driver=");
    uart_puts(net_driver_name());
    uart_puts(" link=");
    uart_puts(net_link_up() ? "up" : "down");
    uart_puts("\n");
    uart_puts("NET rx_ok=");
    uart_puthex((unsigned int)g_stats.rx_ok);
    uart_puts(" rx_drop=");
    uart_puthex((unsigned int)g_stats.rx_drop);
    uart_puts(" tx_ok=");
    uart_puthex((unsigned int)g_stats.tx_ok);
    uart_puts(" tx_fail=");
    uart_puthex((unsigned int)g_stats.tx_fail);
    uart_puts(" rxq=");
    uart_puthex(g_rxq.count);
    uart_puts("\n");
}
