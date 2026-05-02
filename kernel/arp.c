#include "arp.h"
#include "net.h"
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
static unsigned char g_periodic_target_ip[4];
static unsigned long g_periodic_interval = 0;
static unsigned long g_periodic_next_tick = 0;
static int g_periodic_enabled = 0;
static int g_gateway_resolved = 0;
static unsigned char g_gateway_ip[4];
static unsigned char g_gateway_mac[ETH_ADDR_LEN];

static unsigned short be16(const unsigned char* p){
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

void arp_init(void){
    g_arp_stats.rx_total = 0;
    g_arp_stats.rx_valid = 0;
    g_arp_stats.rx_request = 0;
    g_arp_stats.rx_reply = 0;
    g_arp_stats.rx_unsupported = 0;
    g_iface_ready = 0;
    g_tx_req = 0;
    g_periodic_interval = 0;
    g_periodic_next_tick = 0;
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
        // Scaffold hook: respond if target IP matches local IP.
    } else if (oper == ARP_OP_REPLY){
        g_arp_stats.rx_reply++;
        uart_puts("ARP reply from ");
        uart_puthex((unsigned int)arp->spa[0]);
        uart_puts(".");
        uart_puthex((unsigned int)arp->spa[1]);
        uart_puts(".");
        uart_puthex((unsigned int)arp->spa[2]);
        uart_puts(".");
        uart_puthex((unsigned int)arp->spa[3]);
        uart_puts(" mac=");
        for (unsigned int i = 0; i < ETH_ADDR_LEN; i++){
            unsigned char b = arp->sha[i];
            unsigned char hi = (unsigned char)((b >> 4) & 0x0Fu);
            unsigned char lo = (unsigned char)(b & 0x0Fu);
            uart_send((char)(hi < 10 ? ('0' + hi) : ('A' + (hi - 10))));
            uart_send((char)(lo < 10 ? ('0' + lo) : ('A' + (lo - 10))));
            if (i + 1u < ETH_ADDR_LEN){
                uart_send(':');
            }
        }
        uart_puts("\n");

        // If this reply is for our periodic gateway probe, store it and stop retries.
        if (g_periodic_enabled &&
            arp->spa[0] == g_periodic_target_ip[0] &&
            arp->spa[1] == g_periodic_target_ip[1] &&
            arp->spa[2] == g_periodic_target_ip[2] &&
            arp->spa[3] == g_periodic_target_ip[3]){
            for (unsigned int i = 0; i < 4; i++){
                g_gateway_ip[i] = arp->spa[i];
            }
            for (unsigned int i = 0; i < ETH_ADDR_LEN; i++){
                g_gateway_mac[i] = arp->sha[i];
            }
            g_gateway_resolved = 1;
            g_periodic_enabled = 0;
            uart_puts("ARP gateway learned; periodic requests stopped\n");
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
    }
    g_periodic_interval = interval_ms;
    g_periodic_next_tick = 0;
    g_periodic_enabled = 1;
    g_gateway_resolved = 0;
}

void arp_periodic_tick(unsigned long now_ticks){
    if (!g_periodic_enabled || !g_iface_ready){
        return;
    }
    if ((long)(now_ticks - g_periodic_next_tick) < 0){
        return;
    }
    if (!net_link_up()){
        uart_puts("ARP periodic skip: link down\n");
        g_periodic_next_tick = now_ticks + g_periodic_interval;
        return;
    }

    if (arp_send_request(g_periodic_target_ip) == 0){
        uart_puts("ARP who-has sent\n");
    } else{
        uart_puts("ARP who-has send failed\n");
    }
    g_periodic_next_tick = now_ticks + g_periodic_interval;
}

void arp_dump_stats(void){
    uart_puts("ARP rx=");
    uart_puthex((unsigned int)g_arp_stats.rx_total);
    uart_puts(" valid=");
    uart_puthex((unsigned int)g_arp_stats.rx_valid);
    uart_puts(" req=");
    uart_puthex((unsigned int)g_arp_stats.rx_request);
    uart_puts(" rep=");
    uart_puthex((unsigned int)g_arp_stats.rx_reply);
    uart_puts(" bad=");
    uart_puthex((unsigned int)g_arp_stats.rx_unsupported);
    uart_puts(" tx_req=");
    uart_puthex((unsigned int)g_tx_req);
    uart_puts(" gw=");
    uart_puts(g_gateway_resolved ? "yes" : "no");
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
