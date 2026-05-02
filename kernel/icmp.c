#include "icmp.h"
#include "ethernet.h"
#include "arp.h"
#include "net.h"
#include "net_proto.h"
#include "timer.h"
#include "uart.h"

typedef struct __attribute__((packed)) {
    unsigned char type;
    unsigned char code;
    unsigned short checksum_be;
    unsigned short ident_be;
    unsigned short seq_be;
} icmp_echo_hdr_t;

typedef struct {
    unsigned long tx_echo_req;
    unsigned long rx_echo_rep;
    unsigned long rx_other;
    unsigned long timeouts;
    unsigned long bad_reply;
} icmp_stats_t;

static icmp_stats_t g_icmp_stats;
static volatile unsigned short g_ping_seq = 1u;
static const unsigned short g_ping_ident = 0x5153u;
static volatile int g_ping_waiting = 0;
static volatile unsigned short g_ping_wait_seq = 0;
static volatile unsigned long g_ping_send_tick = 0;
static volatile int g_ping_result_ms = -1;

static unsigned long read_daif(void){
    unsigned long v;
    asm volatile("mrs %0, daif" : "=r"(v));
    return v;
}

static void write_daif(unsigned long v){
    asm volatile("msr daif, %0" : : "r"(v) : "memory");
}

static unsigned short be16_read(const unsigned char* p){
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

static void be16_write(unsigned char* p, unsigned short v){
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xFFu);
}

static unsigned short inet_checksum(const unsigned char* data, unsigned int len){
    unsigned long sum = 0;
    unsigned int i = 0;
    while (i + 1u < len){
        sum += ((unsigned long)data[i] << 8) | data[i + 1u];
        i += 2u;
    }
    if (i < len){
        sum += ((unsigned long)data[i] << 8);
    }
    while (sum >> 16){
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (unsigned short)(~sum & 0xFFFFu);
}

void icmp_init(void){
    g_icmp_stats.tx_echo_req = 0;
    g_icmp_stats.rx_echo_rep = 0;
    g_icmp_stats.rx_other = 0;
    g_icmp_stats.timeouts = 0;
    g_icmp_stats.bad_reply = 0;
    g_ping_seq = 1u;
    g_ping_waiting = 0;
    g_ping_wait_seq = 0;
    g_ping_send_tick = 0;
    g_ping_result_ms = -1;
}

void icmp_handle_ipv4_packet(const unsigned char* src_ip,
                             const unsigned char* dst_ip,
                             const unsigned char* payload,
                             unsigned int payload_len){
    (void)dst_ip;
    if (!src_ip || !payload || payload_len < sizeof(icmp_echo_hdr_t)){
        g_icmp_stats.rx_other++;
        return;
    }

    const icmp_echo_hdr_t* icmp = (const icmp_echo_hdr_t*)payload;
    if (icmp->type != 0 || icmp->code != 0){
        g_icmp_stats.rx_other++;
        return;
    }

    g_icmp_stats.rx_echo_rep++;
    if (!g_ping_waiting){
        return;
    }

    unsigned short ident = be16_read((const unsigned char*)&icmp->ident_be);
    unsigned short seq = be16_read((const unsigned char*)&icmp->seq_be);
    if (ident != g_ping_ident || seq != g_ping_wait_seq){
        g_icmp_stats.bad_reply++;
        return;
    }

    unsigned char expected_gateway[4];
    net_proto_get_gateway_ip(expected_gateway);
    if (src_ip[0] != expected_gateway[0] ||
        src_ip[1] != expected_gateway[1] ||
        src_ip[2] != expected_gateway[2] ||
        src_ip[3] != expected_gateway[3]){
        g_icmp_stats.bad_reply++;
        return;
    }

    g_ping_result_ms = (int)(system_ticks - g_ping_send_tick);
    g_ping_waiting = 0;
}

int icmp_ping_gateway(unsigned int timeout_ms){
    unsigned char gateway_mac[ETH_ADDR_LEN];
    unsigned char gateway_ip[4];
    unsigned char local_ip[4];
    unsigned char local_mac[6];
    unsigned char frame[ETH_HEADER_LEN + 20 + 16];
    unsigned char* ip;
    unsigned char* icmp;
    unsigned short seq;
    unsigned int ip_total_len = 20u + 16u;
    unsigned int frame_len = ETH_HEADER_LEN + ip_total_len;
    unsigned long start_tick;
    unsigned long spin_budget;
    unsigned long saved_daif;

    if (timeout_ms == 0){
        timeout_ms = 1000;
    }
    if (!net_ready()){
        return -3;
    }
    if (!arp_gateway_resolved() || arp_get_gateway_mac(gateway_mac) != 0){
        return -2;
    }

    net_proto_get_gateway_ip(gateway_ip);
    net_proto_get_local_ip(local_ip);
    net_proto_get_local_mac(local_mac);

    for (unsigned int i = 0; i < frame_len; i++){
        frame[i] = 0;
    }

    for (unsigned int i = 0; i < ETH_ADDR_LEN; i++){
        frame[i] = gateway_mac[i];
        frame[6 + i] = local_mac[i];
    }
    frame[12] = (unsigned char)(ETH_TYPE_IPV4 >> 8);
    frame[13] = (unsigned char)(ETH_TYPE_IPV4 & 0xFFu);

    ip = frame + ETH_HEADER_LEN;
    ip[0] = 0x45; // v4 + IHL=5
    ip[1] = 0x00;
    be16_write(&ip[2], (unsigned short)ip_total_len);
    seq = g_ping_seq++;
    be16_write(&ip[4], seq);
    be16_write(&ip[6], 0x0000); // flags+frag
    ip[8] = 64;   // TTL
    ip[9] = 1;    // ICMP
    be16_write(&ip[10], 0);
    for (unsigned int i = 0; i < 4; i++){
        ip[12 + i] = local_ip[i];
        ip[16 + i] = gateway_ip[i];
    }
    be16_write(&ip[10], inet_checksum(ip, 20));

    icmp = ip + 20;
    icmp[0] = 8; // echo request
    icmp[1] = 0;
    be16_write(&icmp[2], 0);
    be16_write(&icmp[4], g_ping_ident);
    be16_write(&icmp[6], seq);
    // payload (8 bytes)
    icmp[8] = (unsigned char)((system_ticks >> 24) & 0xFFu);
    icmp[9] = (unsigned char)((system_ticks >> 16) & 0xFFu);
    icmp[10] = (unsigned char)((system_ticks >> 8) & 0xFFu);
    icmp[11] = (unsigned char)(system_ticks & 0xFFu);
    icmp[12] = 'Q';
    icmp[13] = 'O';
    icmp[14] = 'S';
    icmp[15] = '!';
    be16_write(&icmp[2], inet_checksum(icmp, 16));

    g_ping_wait_seq = seq;
    g_ping_send_tick = system_ticks;
    g_ping_result_ms = -1;
    g_ping_waiting = 1;

    if (net_send_raw(frame, frame_len) != 0){
        g_ping_waiting = 0;
        return -3;
    }
    g_icmp_stats.tx_echo_req++;

    // During SVC handling, IRQs may be masked. Allow timer IRQ while we wait,
    // otherwise system_ticks/net RX won't advance and ping appears frozen.
    saved_daif = read_daif();
    asm volatile("msr daifclr, #2" : : : "memory"); // clear I bit (IRQ mask)

    start_tick = system_ticks;
    // Fallback budget so ping can time out even if timer IRQ is stalled/masked.
    // Tuned conservatively to avoid hanging the shell forever in syscall path.
    spin_budget = ((unsigned long)timeout_ms * 200000UL) + 200000UL;
    while (g_ping_waiting){
        if ((long)(system_ticks - start_tick) >= (long)timeout_ms){
            g_ping_waiting = 0;
            g_icmp_stats.timeouts++;
            write_daif(saved_daif);
            return -1;
        }
        if (spin_budget-- == 0){
            g_ping_waiting = 0;
            g_icmp_stats.timeouts++;
            write_daif(saved_daif);
            return -1;
        }
        asm volatile("nop");
    }

    write_daif(saved_daif);

    return g_ping_result_ms >= 0 ? g_ping_result_ms : -1;
}

void icmp_dump_stats(void){
    uart_puts("ICMP tx=");
    uart_puthex((unsigned int)g_icmp_stats.tx_echo_req);
    uart_puts(" rx_rep=");
    uart_puthex((unsigned int)g_icmp_stats.rx_echo_rep);
    uart_puts(" other=");
    uart_puthex((unsigned int)g_icmp_stats.rx_other);
    uart_puts(" bad=");
    uart_puthex((unsigned int)g_icmp_stats.bad_reply);
    uart_puts(" to=");
    uart_puthex((unsigned int)g_icmp_stats.timeouts);
    uart_puts("\n");
}
