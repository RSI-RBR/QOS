#include "net.h"
#include "nic.h"
#include "uart.h"
#include "ethernet.h"
#include "usb_host.h"
#include "net_proto.h"
#include "icmp.h"
#include "spinlock.h"
#include "crypto.h"

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
static volatile int g_net_poll_active = 0;
static spinlock_t g_net_state_lock;
static spinlock_t g_net_rxq_lock;
static spinlock_t g_net_io_lock;

static void net_rxq_reset(void);

static int net_switch_backend(const nic_driver_t* nic){
    if (!nic || !nic->init || !nic->poll || !nic->send || !nic->set_rx_handler){
        return -1;
    }
    if (nic->set_rx_handler(net_ingest_rx_from_driver) != 0){
        return -1;
    }
    if (nic->init() != 0){
        return -1;
    }
    {
        unsigned long irq = spin_lock_irqsave(&g_net_rxq_lock);
        net_rxq_reset();
        spin_unlock_irqrestore(&g_net_rxq_lock, irq);
    }
    {
        unsigned long irq = spin_lock_irqsave(&g_net_state_lock);
        g_nic = nic;
        spin_unlock_irqrestore(&g_net_state_lock, irq);
    }
    return 0;
}

static void net_rxq_reset(void){
    g_rxq.head = 0;
    g_rxq.tail = 0;
    g_rxq.count = 0;
}

static int net_rxq_push(const unsigned char* frame, unsigned int len){
    unsigned long irq = spin_lock_irqsave(&g_net_rxq_lock);
    if (!frame || len == 0 || len > NET_MAX_FRAME_SIZE){
        spin_unlock_irqrestore(&g_net_rxq_lock, irq);
        return -1;
    }
    if (g_rxq.count >= NET_RX_QUEUE_LEN){
        spin_unlock_irqrestore(&g_net_rxq_lock, irq);
        return -1;
    }

    net_frame_t* slot = &g_rxq.frames[g_rxq.tail];
    slot->len = len;
    for (unsigned int i = 0; i < len; i++){
        slot->data[i] = frame[i];
    }

    g_rxq.tail = (g_rxq.tail + 1) % NET_RX_QUEUE_LEN;
    asm volatile("dmb ishst" : : : "memory");
    g_rxq.count++;
    spin_unlock_irqrestore(&g_net_rxq_lock, irq);
    return 0;
}

static int net_rxq_pop(net_frame_t* out){
    unsigned long irq = spin_lock_irqsave(&g_net_rxq_lock);
    if (!out || g_rxq.count == 0){
        spin_unlock_irqrestore(&g_net_rxq_lock, irq);
        return -1;
    }

    net_frame_t* src = &g_rxq.frames[g_rxq.head];
    asm volatile("dmb ish" : : : "memory");
    out->len = src->len;
    for (unsigned int i = 0; i < src->len && i < NET_MAX_FRAME_SIZE; i++){
        out->data[i] = src->data[i];
    }
    g_rxq.head = (g_rxq.head + 1) % NET_RX_QUEUE_LEN;
    g_rxq.count--;
    spin_unlock_irqrestore(&g_net_rxq_lock, irq);
    return 0;
}

void net_ingest_rx_from_driver(const unsigned char* frame, unsigned int len){
    if (frame && len){
        unsigned int take = (len < 32u) ? len : 32u;
        crypto_add_entropy(frame, take);
    }
    if (net_rxq_push(frame, len) == 0){
        unsigned long irq = spin_lock_irqsave(&g_net_state_lock);
        g_stats.rx_ok++;
        spin_unlock_irqrestore(&g_net_state_lock, irq);
    } else{
        unsigned long irq = spin_lock_irqsave(&g_net_state_lock);
        g_stats.rx_drop++;
        spin_unlock_irqrestore(&g_net_state_lock, irq);
    }
}

void net_set_rx_callback(net_rx_callback_t cb){
    unsigned long irq = spin_lock_irqsave(&g_net_state_lock);
    g_rx_cb = cb;
    spin_unlock_irqrestore(&g_net_state_lock, irq);
}

int net_init(void){
    spinlock_init(&g_net_state_lock);
    spinlock_init(&g_net_rxq_lock);
    spinlock_init(&g_net_io_lock);
    g_nic = nic_probe_default();
    {
        unsigned long irq = spin_lock_irqsave(&g_net_rxq_lock);
        net_rxq_reset();
        spin_unlock_irqrestore(&g_net_rxq_lock, irq);
    }
    {
        unsigned long irq = spin_lock_irqsave(&g_net_state_lock);
        g_stats.rx_ok = 0;
        g_stats.rx_drop = 0;
        g_stats.tx_ok = 0;
        g_stats.tx_fail = 0;
        g_net_ready = 0;
        g_net_poll_active = 0;
        g_rx_cb = 0;
        spin_unlock_irqrestore(&g_net_state_lock, irq);
    }

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
        uart_puts("NET: NIC init failed for ");
        uart_puts(g_nic->name ? g_nic->name : "unknown");
        uart_puts("; falling back to stub-loopback\n");

        g_nic = nic_probe_stub();
        if (!g_nic || !g_nic->init || !g_nic->poll || !g_nic->send || !g_nic->set_rx_handler){
            uart_puts("NET: stub backend unavailable\n");
            return -1;
        }
        if (g_nic->set_rx_handler(net_ingest_rx_from_driver) != 0 || g_nic->init() != 0){
            uart_puts("NET: stub init failed\n");
            return -1;
        }
    }

    {
        unsigned long irq = spin_lock_irqsave(&g_net_state_lock);
        g_net_ready = 1;
        spin_unlock_irqrestore(&g_net_state_lock, irq);
    }
    net_proto_init();
    net_proto_configure_defaults();
    return 0;
}

int net_ready(void){
    unsigned long irq = spin_lock_irqsave(&g_net_state_lock);
    int ready = g_net_ready;
    spin_unlock_irqrestore(&g_net_state_lock, irq);
    return ready;
}

const char* net_driver_name(void){
    unsigned long irq = spin_lock_irqsave(&g_net_state_lock);
    const char* name = "none";
    if (!g_nic || !g_nic->name){
        spin_unlock_irqrestore(&g_net_state_lock, irq);
        return "none";
    }
    name = g_nic->name;
    spin_unlock_irqrestore(&g_net_state_lock, irq);
    return name;
}

int net_link_up(void){
    const nic_driver_t* nic = 0;
    unsigned long irq = spin_lock_irqsave(&g_net_state_lock);
    if (!g_net_ready || !g_nic || !g_nic->link_up){
        spin_unlock_irqrestore(&g_net_state_lock, irq);
        return 0;
    }
    nic = g_nic;
    spin_unlock_irqrestore(&g_net_state_lock, irq);
    return nic->link_up();
}

int net_send_raw(const unsigned char* frame, unsigned int len){
    const nic_driver_t* nic = 0;
    unsigned long irq = spin_lock_irqsave(&g_net_state_lock);
    if (!g_net_ready || !g_nic){
        spin_unlock_irqrestore(&g_net_state_lock, irq);
        return -1;
    }
    nic = g_nic;
    spin_unlock_irqrestore(&g_net_state_lock, irq);
    if (!frame || len == 0 || len > NET_MAX_FRAME_SIZE){
        return -1;
    }

    unsigned long io_irq = spin_lock_irqsave(&g_net_io_lock);
    int send_rc = nic->send(frame, len);
    spin_unlock_irqrestore(&g_net_io_lock, io_irq);

    if (send_rc == 0){
        irq = spin_lock_irqsave(&g_net_state_lock);
        g_stats.tx_ok++;
        spin_unlock_irqrestore(&g_net_state_lock, irq);
        return 0;
    }
    irq = spin_lock_irqsave(&g_net_state_lock);
    g_stats.tx_fail++;
    spin_unlock_irqrestore(&g_net_state_lock, irq);
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
    const nic_driver_t* nic = 0;
    net_rx_callback_t cb = 0;
    unsigned long irq = spin_lock_irqsave(&g_net_state_lock);
    if (!g_net_ready || !g_nic){
        spin_unlock_irqrestore(&g_net_state_lock, irq);
        return 0;
    }
    // Avoid nested NIC polling when called both from timer IRQ and foreground
    // wait loops (e.g. ping syscall path).
    if (g_net_poll_active){
        spin_unlock_irqrestore(&g_net_state_lock, irq);
        return 0;
    }
    g_net_poll_active = 1;
    nic = g_nic;
    cb = g_rx_cb;
    spin_unlock_irqrestore(&g_net_state_lock, irq);

    unsigned long io_irq = spin_lock_irqsave(&g_net_io_lock);
    if (nic->poll){
        nic->poll();
    }
    spin_unlock_irqrestore(&g_net_io_lock, io_irq);

    int delivered = 0;
    net_frame_t frame;
    while (net_rxq_pop(&frame) == 0){
        // Kernel protocol stack entry point for every received raw frame.
        net_proto_handle_frame(frame.data, frame.len);
        if (cb){
            cb(frame.data, frame.len);
        }
        delivered++;
    }
    irq = spin_lock_irqsave(&g_net_state_lock);
    g_net_poll_active = 0;
    spin_unlock_irqrestore(&g_net_state_lock, irq);
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

int net_ping_gateway(unsigned int timeout_ms){
    return icmp_ping_gateway(timeout_ms);
}

int net_try_select_wifi_backend(void){
    const nic_driver_t* wifi = nic_probe_cyw43();
    if (!wifi){
        return -1;
    }
    return net_switch_backend(wifi);
}

int net_try_select_default_backend(void){
    const nic_driver_t* d = nic_probe_default();
    if (!d){
        return -1;
    }
    if (net_switch_backend(d) == 0){
        return 0;
    }
    d = nic_probe_stub();
    if (!d){
        return -1;
    }
    return net_switch_backend(d);
}

void net_dump_stats(void){
    unsigned long rx_ok, rx_drop, tx_ok, tx_fail;
    unsigned int rxq_count;
    const char* driver;
    int link;

    unsigned long irq = spin_lock_irqsave(&g_net_state_lock);
    rx_ok = g_stats.rx_ok;
    rx_drop = g_stats.rx_drop;
    tx_ok = g_stats.tx_ok;
    tx_fail = g_stats.tx_fail;
    driver = (!g_nic || !g_nic->name) ? "none" : g_nic->name;
    spin_unlock_irqrestore(&g_net_state_lock, irq);

    irq = spin_lock_irqsave(&g_net_rxq_lock);
    rxq_count = g_rxq.count;
    spin_unlock_irqrestore(&g_net_rxq_lock, irq);

    link = net_link_up();

    uart_puts("NET driver=");
    uart_puts(driver);
    uart_puts(" link=");
    uart_puts(link ? "up" : "down");
    uart_puts("\n");
    uart_puts("NET rx_ok=");
    uart_putdec(rx_ok);
    uart_puts(" rx_drop=");
    uart_putdec(rx_drop);
    uart_puts(" tx_ok=");
    uart_putdec(tx_ok);
    uart_puts(" tx_fail=");
    uart_putdec(tx_fail);
    uart_puts(" rxq=");
    uart_putdec(rxq_count);
    uart_puts("\n");
    net_proto_dump_stats();
}
