#include "udp.h"
#include "ipv4.h"
#include "net_proto.h"
#include "uart.h"
#include "spinlock.h"

typedef struct __attribute__((packed)) {
    unsigned short src_port_be;
    unsigned short dst_port_be;
    unsigned short len_be;
    unsigned short checksum_be;
} udp_header_t;

typedef struct {
    udp_meta_t meta;
    unsigned char data[UDP_MAX_PAYLOAD];
} udp_slot_t;

typedef struct {
    unsigned long rx_total;
    unsigned long rx_valid;
    unsigned long rx_short;
    unsigned long rx_bad_len;
    unsigned long rx_bad_checksum;
    unsigned long rx_drop;
    unsigned long tx_total;
    unsigned long tx_ok;
    unsigned long tx_fail;
} udp_stats_t;

#define UDP_RX_QUEUE_LEN 16u

static udp_stats_t g_udp_stats;
static udp_slot_t g_rxq[UDP_RX_QUEUE_LEN];
static unsigned int g_rx_head = 0;
static unsigned int g_rx_tail = 0;
static unsigned int g_rx_count = 0;
static spinlock_t g_udp_lock;

static unsigned short be16_read(const unsigned char* p){
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

static int ip4_eq(const unsigned char a[4], const unsigned char b[4]){
    return (a[0] == b[0]) && (a[1] == b[1]) && (a[2] == b[2]) && (a[3] == b[3]);
}

static void be16_write(unsigned char* p, unsigned short v){
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xFFu);
}

static unsigned long sum_add_bytes(unsigned long sum, const unsigned char* data, unsigned int len){
    unsigned int i = 0;
    while (i + 1u < len){
        sum += ((unsigned long)data[i] << 8) | data[i + 1u];
        i += 2u;
    }
    if (i < len){
        sum += ((unsigned long)data[i] << 8);
    }
    return sum;
}

static unsigned short sum_finalize(unsigned long sum){
    while (sum >> 16){
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (unsigned short)(~sum & 0xFFFFu);
}

static unsigned short udp_checksum(const unsigned char src_ip[4],
                                   const unsigned char dst_ip[4],
                                   const unsigned char* udp_bytes,
                                   unsigned int udp_len){
    unsigned long sum = 0;
    sum = sum_add_bytes(sum, src_ip, 4);
    sum = sum_add_bytes(sum, dst_ip, 4);
    sum += (unsigned long)IPV4_PROTO_UDP;
    sum += (unsigned long)udp_len;
    sum = sum_add_bytes(sum, udp_bytes, udp_len);
    return sum_finalize(sum);
}

void udp_init(void){
    spinlock_init(&g_udp_lock);
    unsigned long irq = spin_lock_irqsave(&g_udp_lock);
    g_udp_stats.rx_total = 0;
    g_udp_stats.rx_valid = 0;
    g_udp_stats.rx_short = 0;
    g_udp_stats.rx_bad_len = 0;
    g_udp_stats.rx_bad_checksum = 0;
    g_udp_stats.rx_drop = 0;
    g_udp_stats.tx_total = 0;
    g_udp_stats.tx_ok = 0;
    g_udp_stats.tx_fail = 0;
    g_rx_head = 0;
    g_rx_tail = 0;
    g_rx_count = 0;
    spin_unlock_irqrestore(&g_udp_lock, irq);
}

void udp_handle_ipv4_packet(const unsigned char src_ip[4],
                            const unsigned char dst_ip[4],
                            const unsigned char* payload,
                            unsigned int payload_len){
    unsigned long irq = spin_lock_irqsave(&g_udp_lock);
    g_udp_stats.rx_total++;
    if (!src_ip || !dst_ip || !payload || payload_len < sizeof(udp_header_t)){
        g_udp_stats.rx_short++;
        spin_unlock_irqrestore(&g_udp_lock, irq);
        return;
    }

    const udp_header_t* h = (const udp_header_t*)payload;
    unsigned int udp_len = be16_read((const unsigned char*)&h->len_be);
    if (udp_len < sizeof(udp_header_t) || udp_len > payload_len){
        g_udp_stats.rx_bad_len++;
        spin_unlock_irqrestore(&g_udp_lock, irq);
        return;
    }

    unsigned short recv_csum = be16_read((const unsigned char*)&h->checksum_be);
    if (recv_csum != 0){
        if (udp_checksum(src_ip, dst_ip, payload, udp_len) != 0){
            g_udp_stats.rx_bad_checksum++;
            spin_unlock_irqrestore(&g_udp_lock, irq);
            return;
        }
    }

    unsigned int data_len = udp_len - (unsigned int)sizeof(udp_header_t);
    if (data_len > UDP_MAX_PAYLOAD || g_rx_count >= UDP_RX_QUEUE_LEN){
        g_udp_stats.rx_drop++;
        spin_unlock_irqrestore(&g_udp_lock, irq);
        return;
    }

    udp_slot_t* slot = &g_rxq[g_rx_tail];
    for (unsigned int i = 0; i < 4; i++){
        slot->meta.src_ip[i] = src_ip[i];
    }
    slot->meta.src_port = be16_read((const unsigned char*)&h->src_port_be);
    slot->meta.dst_port = be16_read((const unsigned char*)&h->dst_port_be);
    slot->meta.len = (unsigned short)data_len;
    for (unsigned int i = 0; i < data_len; i++){
        slot->data[i] = payload[sizeof(udp_header_t) + i];
    }

    g_rx_tail = (g_rx_tail + 1u) % UDP_RX_QUEUE_LEN;
    asm volatile("dmb ishst" : : : "memory");
    g_rx_count++;
    g_udp_stats.rx_valid++;
    spin_unlock_irqrestore(&g_udp_lock, irq);
}

int udp_send(const unsigned char dst_ip[4],
             unsigned short src_port,
             unsigned short dst_port,
             const unsigned char* data,
             unsigned int len){
    unsigned char local_ip[4];
    unsigned char buf[8 + UDP_MAX_PAYLOAD];
    unsigned int udp_len = 8u + len;
    unsigned long irq = spin_lock_irqsave(&g_udp_lock);
    g_udp_stats.tx_total++;
    spin_unlock_irqrestore(&g_udp_lock, irq);

    if (!dst_ip || (!data && len > 0) || len > UDP_MAX_PAYLOAD){
        irq = spin_lock_irqsave(&g_udp_lock);
        g_udp_stats.tx_fail++;
        spin_unlock_irqrestore(&g_udp_lock, irq);
        return -1;
    }

    be16_write(&buf[0], src_port);
    be16_write(&buf[2], dst_port);
    be16_write(&buf[4], (unsigned short)udp_len);
    be16_write(&buf[6], 0);
    for (unsigned int i = 0; i < len; i++){
        buf[8 + i] = data[i];
    }

    net_proto_get_local_ip(local_ip);
    unsigned short csum = udp_checksum(local_ip, dst_ip, buf, udp_len);
    if (csum == 0){
        csum = 0xFFFFu;
    }
    be16_write(&buf[6], csum);

    if (ipv4_send_via_gateway(IPV4_PROTO_UDP, dst_ip, buf, udp_len) != 0){
        irq = spin_lock_irqsave(&g_udp_lock);
        g_udp_stats.tx_fail++;
        spin_unlock_irqrestore(&g_udp_lock, irq);
        return -1;
    }
    irq = spin_lock_irqsave(&g_udp_lock);
    g_udp_stats.tx_ok++;
    spin_unlock_irqrestore(&g_udp_lock, irq);
    return 0;
}

int udp_send_probe_gateway(void){
    static const unsigned char payload[] = "QOS-UDP-PROBE";
    unsigned char gw[4];
    net_proto_get_gateway_ip(gw);
    return udp_send(gw, 40000u, 40001u, payload, (unsigned int)(sizeof(payload) - 1u));
}

int udp_recv_next(unsigned char* out, unsigned int out_cap, udp_meta_t* meta){
    unsigned long irq = spin_lock_irqsave(&g_udp_lock);
    if (!out || out_cap == 0 || !meta){
        spin_unlock_irqrestore(&g_udp_lock, irq);
        return -1;
    }
    if (g_rx_count == 0){
        spin_unlock_irqrestore(&g_udp_lock, irq);
        return 0;
    }

    udp_slot_t* slot = &g_rxq[g_rx_head];
    unsigned int n = slot->meta.len;
    if (n > out_cap){
        n = out_cap;
    }
    *meta = slot->meta;
    for (unsigned int i = 0; i < n; i++){
        out[i] = slot->data[i];
    }

    g_rx_head = (g_rx_head + 1u) % UDP_RX_QUEUE_LEN;
    g_rx_count--;
    spin_unlock_irqrestore(&g_udp_lock, irq);
    return (int)n;
}

int udp_recv_filtered(unsigned short dst_port,
                      int require_src,
                      const unsigned char src_ip[4],
                      unsigned short src_port,
                      unsigned char* out,
                      unsigned int out_cap,
                      udp_meta_t* meta){
    unsigned long irq = spin_lock_irqsave(&g_udp_lock);
    if (!out || out_cap == 0 || !meta){
        spin_unlock_irqrestore(&g_udp_lock, irq);
        return -1;
    }
    if (g_rx_count == 0){
        spin_unlock_irqrestore(&g_udp_lock, irq);
        return 0;
    }

    int found = -1;
    for (unsigned int i = 0; i < g_rx_count; i++){
        unsigned int idx = (g_rx_head + i) % UDP_RX_QUEUE_LEN;
        udp_slot_t* slot = &g_rxq[idx];
        if (slot->meta.dst_port != dst_port){
            continue;
        }
        if (require_src){
            if (slot->meta.src_port != src_port){
                continue;
            }
            if (!src_ip || !ip4_eq(slot->meta.src_ip, src_ip)){
                continue;
            }
        }
        found = (int)i;
        break;
    }

    if (found < 0){
        spin_unlock_irqrestore(&g_udp_lock, irq);
        return 0;
    }

    unsigned int slot_idx = (g_rx_head + (unsigned int)found) % UDP_RX_QUEUE_LEN;
    udp_slot_t* slot = &g_rxq[slot_idx];
    unsigned int n = slot->meta.len;
    if (n > out_cap){
        n = out_cap;
    }
    *meta = slot->meta;
    for (unsigned int i = 0; i < n; i++){
        out[i] = slot->data[i];
    }

    for (unsigned int j = (unsigned int)found; (j + 1u) < g_rx_count; j++){
        unsigned int to = (g_rx_head + j) % UDP_RX_QUEUE_LEN;
        unsigned int from = (g_rx_head + j + 1u) % UDP_RX_QUEUE_LEN;
        g_rxq[to].meta.src_ip[0] = g_rxq[from].meta.src_ip[0];
        g_rxq[to].meta.src_ip[1] = g_rxq[from].meta.src_ip[1];
        g_rxq[to].meta.src_ip[2] = g_rxq[from].meta.src_ip[2];
        g_rxq[to].meta.src_ip[3] = g_rxq[from].meta.src_ip[3];
        g_rxq[to].meta.src_port = g_rxq[from].meta.src_port;
        g_rxq[to].meta.dst_port = g_rxq[from].meta.dst_port;
        g_rxq[to].meta.len = g_rxq[from].meta.len;
        for (unsigned int k = 0; k < UDP_MAX_PAYLOAD; k++){
            g_rxq[to].data[k] = g_rxq[from].data[k];
        }
    }
    g_rx_tail = (g_rx_tail + UDP_RX_QUEUE_LEN - 1u) % UDP_RX_QUEUE_LEN;
    g_rx_count--;
    spin_unlock_irqrestore(&g_udp_lock, irq);
    return (int)n;
}

void udp_dump_stats(void){
    unsigned long irq = spin_lock_irqsave(&g_udp_lock);
    uart_puts("UDP rx=");
    uart_putdec(g_udp_stats.rx_total);
    uart_puts(" valid=");
    uart_putdec(g_udp_stats.rx_valid);
    uart_puts(" short=");
    uart_putdec(g_udp_stats.rx_short);
    uart_puts(" badlen=");
    uart_putdec(g_udp_stats.rx_bad_len);
    uart_puts(" badcsum=");
    uart_putdec(g_udp_stats.rx_bad_checksum);
    uart_puts(" drop=");
    uart_putdec(g_udp_stats.rx_drop);
    uart_puts(" tx=");
    uart_putdec(g_udp_stats.tx_total);
    uart_puts(" tx_ok=");
    uart_putdec(g_udp_stats.tx_ok);
    uart_puts(" tx_fail=");
    uart_putdec(g_udp_stats.tx_fail);
    uart_puts(" q=");
    uart_putdec(g_rx_count);
    uart_puts("\n");
    spin_unlock_irqrestore(&g_udp_lock, irq);
}
