#include "arp.h"
#include "net.h"
#include "timer.h"
#include "uart.h"

typedef struct {
    unsigned long rx_total;
    unsigned long rx_valid;
    unsigned long rx_request;
    unsigned long rx_reply;
    unsigned long rx_unsupported;
} arp_stats_t;

static arp_stats_t g_arp_stats;
static unsigned char g_local_mac[ETH_ADDR_LEN];
static unsigned char g_local_ip[4];
static int g_iface_ready = 0;
static unsigned long g_tx_req = 0;
static unsigned long g_tx_rep = 0;
static unsigned long g_rx_req_for_us = 0;
static unsigned long g_rx_req_other = 0;
static unsigned long g_rx_rep_for_us = 0;
static unsigned long g_rx_rep_other = 0;
static unsigned long g_rx_from_gateway = 0;
static unsigned long g_tx_rep_fail = 0;
static unsigned char g_last_req_spa[4];
static unsigned char g_last_req_tpa[4];
static unsigned char g_last_rep_spa[4];
static unsigned char g_last_rep_tpa[4];
static unsigned char g_last_tx_spa[4];
static unsigned char g_last_tx_tpa[4];
static unsigned char g_periodic_target_ip[4];
static unsigned long g_periodic_interval = 0;
static unsigned long g_periodic_next_tick = 0;
static unsigned long g_periodic_attempts = 0;
static int g_periodic_enabled = 0;
static int g_gateway_resolved = 0;
static unsigned char g_gateway_ip[4];
static unsigned char g_gateway_mac[ETH_ADDR_LEN];

static unsigned short be16(const unsigned char* p){
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

static int ip4_eq(const unsigned char a[4], const unsigned char b[4]){
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static int arp_learn_gateway_from_sender(const unsigned char spa[4],
                                         const unsigned char sha[ETH_ADDR_LEN]){
    if (!spa || !sha){
        return 0;
    }
    if (!ip4_eq(spa, g_gateway_ip) && !ip4_eq(spa, g_periodic_target_ip)){
        return 0;
    }
    for (unsigned int i = 0; i < 4; i++){
        g_gateway_ip[i] = spa[i];
    }
    for (unsigned int i = 0; i < ETH_ADDR_LEN; i++){
        g_gateway_mac[i] = sha[i];
    }
    g_gateway_resolved = 1;
    g_periodic_attempts = 0;
    g_periodic_enabled = 0;
    g_rx_from_gateway++;
    return 1;
}

static int arp_send_reply(const unsigned char target_mac[ETH_ADDR_LEN],
                          const unsigned char target_ip[4]){
    unsigned char frame[ETH_MIN_FRAME_LEN];
    arp_packet_t* arp;
    if (!g_iface_ready || !target_mac || !target_ip){
        return -1;
    }

    for (unsigned int i = 0; i < sizeof(frame); i++){
        frame[i] = 0;
    }

    for (unsigned int i = 0; i < ETH_ADDR_LEN; i++){
        frame[i] = target_mac[i];
        frame[6 + i] = g_local_mac[i];
    }
    frame[12] = (unsigned char)(ETH_TYPE_ARP >> 8);
    frame[13] = (unsigned char)(ETH_TYPE_ARP & 0xFFu);

    arp = (arp_packet_t*)(frame + ETH_HEADER_LEN);
    arp->htype_be = (unsigned short)((ARP_HTYPE_ETHERNET >> 8) | (ARP_HTYPE_ETHERNET << 8));
    arp->ptype_be = (unsigned short)((ARP_PTYPE_IPV4 >> 8) | (ARP_PTYPE_IPV4 << 8));
    arp->hlen = ARP_HLEN_ETHERNET;
    arp->plen = ARP_PLEN_IPV4;
    arp->oper_be = (unsigned short)((ARP_OP_REPLY >> 8) | (ARP_OP_REPLY << 8));
    for (unsigned int i = 0; i < ETH_ADDR_LEN; i++){
        arp->sha[i] = g_local_mac[i];
        arp->tha[i] = target_mac[i];
    }
    for (unsigned int i = 0; i < 4; i++){
        arp->spa[i] = g_local_ip[i];
        arp->tpa[i] = target_ip[i];
    }

    if (net_send_raw(frame, sizeof(frame)) != 0){
        return -1;
    }
    g_tx_rep++;
    return 0;
}

void arp_init(void){
    g_arp_stats.rx_total = 0;
    g_arp_stats.rx_valid = 0;
    g_arp_stats.rx_request = 0;
    g_arp_stats.rx_reply = 0;
    g_arp_stats.rx_unsupported = 0;
    g_iface_ready = 0;
    g_tx_req = 0;
    g_tx_rep = 0;
    g_rx_req_for_us = 0;
    g_rx_req_other = 0;
    g_rx_rep_for_us = 0;
    g_rx_rep_other = 0;
    g_rx_from_gateway = 0;
    g_tx_rep_fail = 0;
    for (unsigned int i = 0; i < 4; i++){
        g_last_req_spa[i] = 0;
        g_last_req_tpa[i] = 0;
        g_last_rep_spa[i] = 0;
        g_last_rep_tpa[i] = 0;
        g_last_tx_spa[i] = 0;
        g_last_tx_tpa[i] = 0;
    }
    g_periodic_interval = 0;
    g_periodic_next_tick = 0;
    g_periodic_attempts = 0;
    g_periodic_enabled = 0;
    g_gateway_resolved = 0;
}

void arp_handle_frame(const unsigned char* frame, unsigned int len){
    g_arp_stats.rx_total++;
    if (!frame || len < sizeof(arp_packet_t)){
        return;
    }

    const arp_packet_t* arp = (const arp_packet_t*)frame;
    unsigned short htype = be16((const unsigned char*)&arp->htype_be);
    unsigned short ptype = be16((const unsigned char*)&arp->ptype_be);
    unsigned short oper = be16((const unsigned char*)&arp->oper_be);

    if (htype != ARP_HTYPE_ETHERNET ||
        ptype != ARP_PTYPE_IPV4 ||
        arp->hlen != ARP_HLEN_ETHERNET ||
        arp->plen != ARP_PLEN_IPV4){
        g_arp_stats.rx_unsupported++;
        return;
    }

    g_arp_stats.rx_valid++;
    if (oper == ARP_OP_REQUEST){
        g_arp_stats.rx_request++;
        for (unsigned int i = 0; i < 4; i++){
            g_last_req_spa[i] = arp->spa[i];
            g_last_req_tpa[i] = arp->tpa[i];
        }
        (void)arp_learn_gateway_from_sender(arp->spa, arp->sha);
        if (g_iface_ready && ip4_eq(arp->tpa, g_local_ip)){
            g_rx_req_for_us++;
            if (arp_send_reply(arp->sha, arp->spa) != 0){
                g_tx_rep_fail++;
            }
        } else{
            g_rx_req_other++;
        }
    } else if (oper == ARP_OP_REPLY){
        g_arp_stats.rx_reply++;
        for (unsigned int i = 0; i < 4; i++){
            g_last_rep_spa[i] = arp->spa[i];
            g_last_rep_tpa[i] = arp->tpa[i];
        }
        int learned_gateway = arp_learn_gateway_from_sender(arp->spa, arp->sha);
        // Learn gateway MAC from replies addressed to our local IP.
        // Accept either the configured gateway IP hint or the current periodic target.
        int reply_for_us = g_iface_ready && ip4_eq(arp->tpa, g_local_ip);
        int sender_is_gateway_hint = ip4_eq(arp->spa, g_gateway_ip);
        int sender_is_periodic_target = ip4_eq(arp->spa, g_periodic_target_ip);
        if (reply_for_us){
            g_rx_rep_for_us++;
        } else{
            g_rx_rep_other++;
        }
        if (reply_for_us && (sender_is_gateway_hint || sender_is_periodic_target)){
            if (learned_gateway){
                uart_puts("ARP gateway learned; periodic requests stopped\n");
            }
        }
    } else{
        g_arp_stats.rx_unsupported++;
    }
}

void arp_set_local_interface(const unsigned char mac[ETH_ADDR_LEN], const unsigned char ip[4]){
    if (!mac || !ip){
        g_iface_ready = 0;
        return;
    }
    for (unsigned int i = 0; i < ETH_ADDR_LEN; i++){
        g_local_mac[i] = mac[i];
    }
    for (unsigned int i = 0; i < 4; i++){
        g_local_ip[i] = ip[i];
    }
    g_iface_ready = 1;
}

int arp_send_request(const unsigned char target_ip[4]){
    unsigned char frame[ETH_MIN_FRAME_LEN];
    arp_packet_t* arp;
    if (!target_ip || !g_iface_ready){
        return -1;
    }

    for (unsigned int i = 0; i < sizeof(frame); i++){
        frame[i] = 0;
    }

    for (unsigned int i = 0; i < ETH_ADDR_LEN; i++){
        frame[i] = 0xFFu; // Broadcast destination MAC.
        frame[6 + i] = g_local_mac[i];
    }
    frame[12] = (unsigned char)(ETH_TYPE_ARP >> 8);
    frame[13] = (unsigned char)(ETH_TYPE_ARP & 0xFFu);

    arp = (arp_packet_t*)(frame + ETH_HEADER_LEN);
    arp->htype_be = (unsigned short)((ARP_HTYPE_ETHERNET >> 8) | (ARP_HTYPE_ETHERNET << 8));
    arp->ptype_be = (unsigned short)((ARP_PTYPE_IPV4 >> 8) | (ARP_PTYPE_IPV4 << 8));
    arp->hlen = ARP_HLEN_ETHERNET;
    arp->plen = ARP_PLEN_IPV4;
    arp->oper_be = (unsigned short)((ARP_OP_REQUEST >> 8) | (ARP_OP_REQUEST << 8));
    for (unsigned int i = 0; i < ETH_ADDR_LEN; i++){
        arp->sha[i] = g_local_mac[i];
        arp->tha[i] = 0;
    }
    for (unsigned int i = 0; i < 4; i++){
        arp->spa[i] = g_local_ip[i];
        arp->tpa[i] = target_ip[i];
        g_last_tx_spa[i] = g_local_ip[i];
        g_last_tx_tpa[i] = target_ip[i];
    }

    if (net_send_raw(frame, sizeof(frame)) != 0){
        return -1;
    }
    g_tx_req++;
    return 0;
}

void arp_set_periodic_target(const unsigned char target_ip[4], unsigned int interval_ms){
    if (!target_ip || interval_ms == 0){
        g_periodic_enabled = 0;
        return;
    }
    for (unsigned int i = 0; i < 4; i++){
        g_periodic_target_ip[i] = target_ip[i];
        g_gateway_ip[i] = target_ip[i];
    }
    g_periodic_interval = interval_ms;
    g_periodic_next_tick = 0;
    g_periodic_attempts = 0;
    g_periodic_enabled = 1;
    g_gateway_resolved = 0;
}

void arp_periodic_tick(unsigned long now_ticks){
    if (!g_periodic_enabled || !g_iface_ready){
        return;
    }
    if (g_gateway_resolved){
        g_periodic_enabled = 0;
        return;
    }
    if ((long)(now_ticks - g_periodic_next_tick) < 0){
        return;
    }

    unsigned long interval = g_periodic_interval;
    // Rate-limit unresolved ARP retries after initial probes.
    if (g_periodic_attempts >= 10u){
        interval = 60000u;
    }

    if (!net_link_up()){
        g_periodic_next_tick = now_ticks + interval;
        return;
    }

    if (arp_send_request(g_periodic_target_ip) == 0 && g_periodic_attempts < 0xFFFFFFFFUL){
        g_periodic_attempts++;
    }
    g_periodic_next_tick = now_ticks + interval;
}

void arp_dump_stats(void){
    uart_puts("ARP rx=");
    uart_putdec(g_arp_stats.rx_total);
    uart_puts(" valid=");
    uart_putdec(g_arp_stats.rx_valid);
    uart_puts(" req=");
    uart_putdec(g_arp_stats.rx_request);
    uart_puts(" rep=");
    uart_putdec(g_arp_stats.rx_reply);
    uart_puts(" bad=");
    uart_putdec(g_arp_stats.rx_unsupported);
    uart_puts(" tx_req=");
    uart_putdec(g_tx_req);
    uart_puts(" tx_rep=");
    uart_putdec(g_tx_rep);
    uart_puts(" rep_fail=");
    uart_putdec(g_tx_rep_fail);
    uart_puts(" req_us=");
    uart_putdec(g_rx_req_for_us);
    uart_puts(" req_other=");
    uart_putdec(g_rx_req_other);
    uart_puts(" rep_us=");
    uart_putdec(g_rx_rep_for_us);
    uart_puts(" rep_other=");
    uart_putdec(g_rx_rep_other);
    uart_puts(" gw_seen=");
    uart_putdec(g_rx_from_gateway);
    uart_puts(" gw=");
    uart_puts(g_gateway_resolved ? "yes" : "no");
    uart_puts(" arp_retry=");
    uart_putdec(g_periodic_attempts);
    uart_puts("\n");
    uart_puts("ARP last_req ");
    uart_putdec(g_last_req_spa[0]);
    uart_puts(".");
    uart_putdec(g_last_req_spa[1]);
    uart_puts(".");
    uart_putdec(g_last_req_spa[2]);
    uart_puts(".");
    uart_putdec(g_last_req_spa[3]);
    uart_puts(" -> ");
    uart_putdec(g_last_req_tpa[0]);
    uart_puts(".");
    uart_putdec(g_last_req_tpa[1]);
    uart_puts(".");
    uart_putdec(g_last_req_tpa[2]);
    uart_puts(".");
    uart_putdec(g_last_req_tpa[3]);
    uart_puts("\n");
    uart_puts("ARP last_rep ");
    uart_putdec(g_last_rep_spa[0]);
    uart_puts(".");
    uart_putdec(g_last_rep_spa[1]);
    uart_puts(".");
    uart_putdec(g_last_rep_spa[2]);
    uart_puts(".");
    uart_putdec(g_last_rep_spa[3]);
    uart_puts(" -> ");
    uart_putdec(g_last_rep_tpa[0]);
    uart_puts(".");
    uart_putdec(g_last_rep_tpa[1]);
    uart_puts(".");
    uart_putdec(g_last_rep_tpa[2]);
    uart_puts(".");
    uart_putdec(g_last_rep_tpa[3]);
    uart_puts("\n");
    uart_puts("ARP last_tx ");
    uart_putdec(g_last_tx_spa[0]);
    uart_puts(".");
    uart_putdec(g_last_tx_spa[1]);
    uart_puts(".");
    uart_putdec(g_last_tx_spa[2]);
    uart_puts(".");
    uart_putdec(g_last_tx_spa[3]);
    uart_puts(" -> ");
    uart_putdec(g_last_tx_tpa[0]);
    uart_puts(".");
    uart_putdec(g_last_tx_tpa[1]);
    uart_puts(".");
    uart_putdec(g_last_tx_tpa[2]);
    uart_puts(".");
    uart_putdec(g_last_tx_tpa[3]);
    uart_puts("\n");
}

int arp_gateway_resolved(void){
    return g_gateway_resolved ? 1 : 0;
}

int arp_get_gateway_mac(unsigned char out_mac[ETH_ADDR_LEN]){
    if (!out_mac || !g_gateway_resolved){
        return -1;
    }
    for (unsigned int i = 0; i < ETH_ADDR_LEN; i++){
        out_mac[i] = g_gateway_mac[i];
    }
    return 0;
}

int arp_resolve_gateway(unsigned int timeout_ms){
    if (g_gateway_resolved){
        return 0;
    }
    if (!g_iface_ready || !net_link_up()){
        return -1;
    }

    if (timeout_ms == 0u){
        timeout_ms = 1000u;
    }

    unsigned long start = system_ticks;
    unsigned long next_req = start;
    while ((unsigned long)(system_ticks - start) < (unsigned long)timeout_ms){
        if ((long)(system_ticks - next_req) >= 0){
            (void)arp_send_request(g_gateway_ip);
            next_req = system_ticks + 200u;
        }
        (void)net_poll();
        if (g_gateway_resolved){
            return 0;
        }
        asm volatile("nop");
    }
    return -1;
}
