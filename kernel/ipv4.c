#include "ipv4.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "arp.h"
#include "net.h"
#include "ethernet.h"
#include "uart.h"

typedef struct {
    unsigned long rx_total;
    unsigned long rx_valid;
    unsigned long rx_too_short;
    unsigned long rx_bad_version;
    unsigned long rx_bad_ihl;
    unsigned long rx_bad_checksum;
    unsigned long rx_not_for_us;
    unsigned long rx_frag;
    unsigned long rx_other_proto;
    unsigned long tx_ok;
    unsigned long tx_fail;
} ipv4_stats_t;

static ipv4_stats_t g_ipv4_stats;
static unsigned char g_local_mac[6];
static unsigned char g_local_ip[4];
static int g_endpoint_ready = 0;
static unsigned short g_ident = 1u;

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

static int ip4_eq(const unsigned char a[4], const unsigned char b[4]){
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static int ip4_is_broadcast(const unsigned char ip[4]){
    return ip[0] == 255u && ip[1] == 255u && ip[2] == 255u && ip[3] == 255u;
}

static int ip4_same_lan24(const unsigned char a[4], const unsigned char b[4]){
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

void ipv4_init(void){
    g_ipv4_stats.rx_total = 0;
    g_ipv4_stats.rx_valid = 0;
    g_ipv4_stats.rx_too_short = 0;
    g_ipv4_stats.rx_bad_version = 0;
    g_ipv4_stats.rx_bad_ihl = 0;
    g_ipv4_stats.rx_bad_checksum = 0;
    g_ipv4_stats.rx_not_for_us = 0;
    g_ipv4_stats.rx_frag = 0;
    g_ipv4_stats.rx_other_proto = 0;
    g_ipv4_stats.tx_ok = 0;
    g_ipv4_stats.tx_fail = 0;
    g_endpoint_ready = 0;
    g_ident = 1u;
}

void ipv4_set_local_endpoint(const unsigned char mac[6],
                             const unsigned char ip[4],
                             const unsigned char gateway_ip[4]){
    (void)gateway_ip;
    if (!mac || !ip || !gateway_ip){
        g_endpoint_ready = 0;
        return;
    }
    for (unsigned int i = 0; i < 6; i++){
        g_local_mac[i] = mac[i];
    }
    for (unsigned int i = 0; i < 4; i++){
        g_local_ip[i] = ip[i];
    }
    g_endpoint_ready = 1;
}

int ipv4_send_via_gateway(unsigned char protocol,
                          const unsigned char dst_ip[4],
                          const unsigned char* payload,
                          unsigned int payload_len){
    unsigned char dst_mac[ETH_ADDR_LEN];
    unsigned char frame[NET_MAX_FRAME_SIZE];
    unsigned char* ip;
    unsigned int frame_len;
    unsigned int ip_total_len = 20u + payload_len;
    int direct_lan = 0;

    if (!g_endpoint_ready || !dst_ip){
        g_ipv4_stats.tx_fail++;
        return -1;
    }
    if (!payload && payload_len > 0){
        g_ipv4_stats.tx_fail++;
        return -1;
    }
    if (payload_len > (NET_MAX_FRAME_SIZE - ETH_HEADER_LEN - 20u)){
        g_ipv4_stats.tx_fail++;
        return -1;
    }

    direct_lan = ip4_same_lan24(dst_ip, g_local_ip) &&
                 !ip4_eq(dst_ip, g_local_ip) &&
                 !ip4_is_broadcast(dst_ip);
    if (direct_lan){
        if (arp_get_mac_for_ip(dst_ip, dst_mac) != 0){
            (void)arp_resolve_ip(dst_ip, 400u);
        }
        if (arp_get_mac_for_ip(dst_ip, dst_mac) != 0){
            direct_lan = 0;
        }
    }
    if (!direct_lan){
        if (arp_get_gateway_mac(dst_mac) != 0){
            (void)arp_resolve_gateway(1200u);
        }
        if (arp_get_gateway_mac(dst_mac) != 0){
            /*
             * If no gateway is known yet but the destination is local, make a
             * final direct attempt. This keeps UDP discovery usable on Wi-Fi
             * networks where the AP does not hairpin same-subnet frames sent
             * to the router MAC.
             */
            if (ip4_same_lan24(dst_ip, g_local_ip) &&
                arp_resolve_ip(dst_ip, 400u) == 0 &&
                arp_get_mac_for_ip(dst_ip, dst_mac) == 0){
                direct_lan = 1;
            } else{
                g_ipv4_stats.tx_fail++;
                return -2;
            }
        }
    }

    frame_len = ETH_HEADER_LEN + ip_total_len;
    for (unsigned int i = 0; i < frame_len; i++){
        frame[i] = 0;
    }

    for (unsigned int i = 0; i < ETH_ADDR_LEN; i++){
        frame[i] = dst_mac[i];
        frame[6 + i] = g_local_mac[i];
    }
    frame[12] = (unsigned char)(ETH_TYPE_IPV4 >> 8);
    frame[13] = (unsigned char)(ETH_TYPE_IPV4 & 0xFFu);

    ip = frame + ETH_HEADER_LEN;
    ip[0] = 0x45; // Version=4, IHL=5
    ip[1] = 0x00;
    be16_write(&ip[2], (unsigned short)ip_total_len);
    be16_write(&ip[4], g_ident++);
    be16_write(&ip[6], 0x0000);
    ip[8] = 64;
    ip[9] = protocol;
    be16_write(&ip[10], 0);
    for (unsigned int i = 0; i < 4; i++){
        ip[12 + i] = g_local_ip[i];
        ip[16 + i] = dst_ip[i];
    }
    be16_write(&ip[10], inet_checksum(ip, 20u));

    for (unsigned int i = 0; i < payload_len; i++){
        ip[20 + i] = payload[i];
    }

    if (net_send_raw(frame, frame_len) != 0){
        g_ipv4_stats.tx_fail++;
        return -1;
    }
    g_ipv4_stats.tx_ok++;
    return 0;
}

void ipv4_handle_frame(const unsigned char* frame, unsigned int len){
    g_ipv4_stats.rx_total++;
    if (!frame || len < sizeof(ipv4_header_t)){
        g_ipv4_stats.rx_too_short++;
        return;
    }

    const ipv4_header_t* ip = (const ipv4_header_t*)frame;
    unsigned int version = (ip->ver_ihl >> 4) & 0xFu;
    unsigned int ihl_words = ip->ver_ihl & 0xFu;
    unsigned int ihl_bytes = ihl_words * 4u;
    if (version != 4u){
        g_ipv4_stats.rx_bad_version++;
        return;
    }
    if (ihl_bytes < 20u || ihl_bytes > len){
        g_ipv4_stats.rx_bad_ihl++;
        return;
    }

    unsigned int total_len = be16_read((const unsigned char*)&ip->total_len_be);
    if (total_len < ihl_bytes || total_len > len){
        g_ipv4_stats.rx_too_short++;
        return;
    }
    if (inet_checksum((const unsigned char*)ip, ihl_bytes) != 0){
        g_ipv4_stats.rx_bad_checksum++;
        return;
    }
    int allow_dhcp_client_reply = 0;
    if (ip->protocol == IPV4_PROTO_UDP &&
        total_len >= ihl_bytes + 8u &&
        be16_read(frame + ihl_bytes + 2u) == 68u){
        /*
         * DHCP servers may unicast OFFER/ACK packets to the offered address
         * before the client has installed that address locally. Accept UDP/68
         * here so the minimal DHCP client can complete reliably.
         */
        allow_dhcp_client_reply = 1;
    }
    if (g_endpoint_ready &&
        !ip4_eq(ip->dst, g_local_ip) &&
        !ip4_is_broadcast(ip->dst) &&
        !allow_dhcp_client_reply){
        g_ipv4_stats.rx_not_for_us++;
        return;
    }

    unsigned short flags_frag = be16_read((const unsigned char*)&ip->flags_frag_be);
    if ((flags_frag & 0x3FFFu) != 0){
        g_ipv4_stats.rx_frag++;
        return;
    }

    g_ipv4_stats.rx_valid++;
    if (ip->protocol == IPV4_PROTO_ICMP){
        icmp_handle_ipv4_packet(ip->src, ip->dst, frame + ihl_bytes, total_len - ihl_bytes);
        return;
    }
    if (ip->protocol == IPV4_PROTO_UDP){
        udp_handle_ipv4_packet(ip->src, ip->dst, frame + ihl_bytes, total_len - ihl_bytes);
        return;
    }
    if (ip->protocol == IPV4_PROTO_TCP){
        tcp_handle_ipv4_packet(ip->src, ip->dst, frame + ihl_bytes, total_len - ihl_bytes);
        return;
    }
    g_ipv4_stats.rx_other_proto++;
}

void ipv4_dump_stats(void){
    uart_puts("IPv4 rx=");
    uart_putdec(g_ipv4_stats.rx_total);
    uart_puts(" valid=");
    uart_putdec(g_ipv4_stats.rx_valid);
    uart_puts(" short=");
    uart_putdec(g_ipv4_stats.rx_too_short);
    uart_puts(" ver=");
    uart_putdec(g_ipv4_stats.rx_bad_version);
    uart_puts(" ihl=");
    uart_putdec(g_ipv4_stats.rx_bad_ihl);
    uart_puts(" csum=");
    uart_putdec(g_ipv4_stats.rx_bad_checksum);
    uart_puts(" dst=");
    uart_putdec(g_ipv4_stats.rx_not_for_us);
    uart_puts(" frag=");
    uart_putdec(g_ipv4_stats.rx_frag);
    uart_puts(" other=");
    uart_putdec(g_ipv4_stats.rx_other_proto);
    uart_puts(" tx_ok=");
    uart_putdec(g_ipv4_stats.tx_ok);
    uart_puts(" tx_fail=");
    uart_putdec(g_ipv4_stats.tx_fail);
    uart_puts("\n");
}
