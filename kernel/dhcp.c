#include "dhcp.h"
#include "ethernet.h"
#include "net.h"
#include "net_proto.h"
#include "udp.h"
#include "uart.h"
#include "timer.h"

#define DHCP_CLIENT_PORT 68u
#define DHCP_SERVER_PORT 67u
#define DHCP_MAGIC_OFF   236u
#define DHCP_OPTIONS_OFF 240u
#define DHCP_MIN_LEN     300u

#define DHCP_OPT_PAD        0u
#define DHCP_OPT_END        255u
#define DHCP_OPT_MSG_TYPE   53u
#define DHCP_OPT_REQ_IP     50u
#define DHCP_OPT_LEASE      51u
#define DHCP_OPT_SERVER_ID  54u
#define DHCP_OPT_PARAM_REQ  55u
#define DHCP_OPT_ROUTER     3u
#define DHCP_OPT_DNS        6u
#define DHCP_OPT_HOSTNAME   12u
#define DHCP_OPT_MAX_MSG    57u
#define DHCP_OPT_CLIENT_ID  61u

#define DHCPDISCOVER 1u
#define DHCPOFFER    2u
#define DHCPREQUEST  3u
#define DHCPACK      5u

typedef struct {
    unsigned char msg_type;
    unsigned char yiaddr[4];
    unsigned char router[4];
    unsigned char dns[4];
    unsigned char server_id[4];
    unsigned int have_router;
    unsigned int have_dns;
    unsigned int have_server_id;
    unsigned int lease_seconds;
} dhcp_msg_t;

static dhcp_diag_t g_dhcp_diag;

static void dhcp_diag_reset(void){
    unsigned char* p = (unsigned char*)&g_dhcp_diag;
    for (unsigned int i = 0u; i < sizeof(g_dhcp_diag); i++){
        p[i] = 0u;
    }
}

static void be16_write(unsigned char* p, unsigned short v){
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xFFu);
}

static unsigned int be32_read(const unsigned char* p){
    return ((unsigned int)p[0] << 24) |
           ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) |
           (unsigned int)p[3];
}

static void be32_write(unsigned char* p, unsigned int v){
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
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

static void ip_copy(unsigned char dst[4], const unsigned char src[4]){
    for (unsigned int i = 0u; i < 4u; i++){
        dst[i] = src[i];
    }
}

static void dhcp_diag_copy_ip(unsigned char dst[4], const unsigned char src[4]){
    for (unsigned int i = 0u; i < 4u; i++){
        dst[i] = src ? src[i] : 0u;
    }
}

static int ip_is_zero(const unsigned char ip[4]){
    return ip[0] == 0u && ip[1] == 0u && ip[2] == 0u && ip[3] == 0u;
}

static void print_ip(const unsigned char ip[4]){
    uart_putdec(ip[0]);
    uart_puts(".");
    uart_putdec(ip[1]);
    uart_puts(".");
    uart_putdec(ip[2]);
    uart_puts(".");
    uart_putdec(ip[3]);
}

static unsigned int make_xid(const unsigned char mac[6]){
    unsigned int xid = (unsigned int)system_ticks ^ 0x514F5344u;
    for (unsigned int i = 0u; i < 6u; i++){
        xid ^= ((unsigned int)mac[i] << ((i & 3u) * 8u));
        xid = (xid << 5) | (xid >> 27);
    }
    return xid ? xid : 0x514F5301u;
}

static unsigned int build_dhcp_payload(unsigned char* out,
                                       unsigned int cap,
                                       unsigned char msg_type,
                                       unsigned int xid,
                                       const unsigned char mac[6],
                                       const unsigned char req_ip[4],
                                       const unsigned char server_id[4]){
    unsigned int opt;
    if (!out || cap < DHCP_MIN_LEN || !mac){
        return 0u;
    }
    for (unsigned int i = 0u; i < cap; i++){
        out[i] = 0u;
    }

    out[0] = 1u; /* BOOTREQUEST */
    out[1] = 1u; /* Ethernet */
    out[2] = 6u;
    be32_write(&out[4], xid);
    be16_write(&out[10], 0x8000u); /* ask server to broadcast replies */
    for (unsigned int i = 0u; i < 6u; i++){
        out[28u + i] = mac[i];
    }
    out[DHCP_MAGIC_OFF + 0u] = 99u;
    out[DHCP_MAGIC_OFF + 1u] = 130u;
    out[DHCP_MAGIC_OFF + 2u] = 83u;
    out[DHCP_MAGIC_OFF + 3u] = 99u;

    opt = DHCP_OPTIONS_OFF;
    out[opt++] = DHCP_OPT_MSG_TYPE;
    out[opt++] = 1u;
    out[opt++] = msg_type;

    if (msg_type == DHCPREQUEST && req_ip && !ip_is_zero(req_ip)){
        out[opt++] = DHCP_OPT_REQ_IP;
        out[opt++] = 4u;
        for (unsigned int i = 0u; i < 4u; i++){
            out[opt++] = req_ip[i];
        }
    }
    if (msg_type == DHCPREQUEST && server_id && !ip_is_zero(server_id)){
        out[opt++] = DHCP_OPT_SERVER_ID;
        out[opt++] = 4u;
        for (unsigned int i = 0u; i < 4u; i++){
            out[opt++] = server_id[i];
        }
    }

    /*
     * Some lightweight AP stacks are picky about client identity/options.
     * These are harmless for normal DHCP servers and make our probe packet
     * look less like a malformed minimal client.
     */
    out[opt++] = DHCP_OPT_CLIENT_ID;
    out[opt++] = 7u;
    out[opt++] = 1u; /* Ethernet hardware type */
    for (unsigned int i = 0u; i < 6u; i++){
        out[opt++] = mac[i];
    }

    out[opt++] = DHCP_OPT_HOSTNAME;
    out[opt++] = 3u;
    out[opt++] = (unsigned char)'q';
    out[opt++] = (unsigned char)'o';
    out[opt++] = (unsigned char)'s';

    out[opt++] = DHCP_OPT_MAX_MSG;
    out[opt++] = 2u;
    out[opt++] = 0x05u;
    out[opt++] = 0xDCu; /* 1500 bytes */

    out[opt++] = DHCP_OPT_PARAM_REQ;
    out[opt++] = 3u;
    out[opt++] = DHCP_OPT_ROUTER;
    out[opt++] = DHCP_OPT_DNS;
    out[opt++] = DHCP_OPT_LEASE;
    out[opt++] = DHCP_OPT_END;

    return DHCP_MIN_LEN;
}

static int send_dhcp_broadcast(unsigned char msg_type,
                               unsigned int xid,
                               const unsigned char mac[6],
                               const unsigned char req_ip[4],
                               const unsigned char server_id[4]){
    unsigned char frame[ETH_HEADER_LEN + 20u + 8u + DHCP_MIN_LEN];
    unsigned char* ip;
    unsigned char* udp;
    unsigned char* dhcp;
    unsigned int dhcp_len;
    unsigned int ip_len;
    unsigned int frame_len;
    int rc;

    dhcp = frame + ETH_HEADER_LEN + 20u + 8u;
    dhcp_len = build_dhcp_payload(dhcp, DHCP_MIN_LEN, msg_type, xid, mac, req_ip, server_id);
    if (dhcp_len == 0u){
        return -1;
    }

    ip_len = 20u + 8u + dhcp_len;
    frame_len = ETH_HEADER_LEN + ip_len;
    for (unsigned int i = 0u; i < ETH_ADDR_LEN; i++){
        frame[i] = 0xFFu;
        frame[6u + i] = mac[i];
    }
    frame[12] = (unsigned char)(ETH_TYPE_IPV4 >> 8);
    frame[13] = (unsigned char)(ETH_TYPE_IPV4 & 0xFFu);

    ip = frame + ETH_HEADER_LEN;
    ip[0] = 0x45u;
    ip[1] = 0u;
    be16_write(&ip[2], (unsigned short)ip_len);
    be16_write(&ip[4], (unsigned short)(xid & 0xFFFFu));
    be16_write(&ip[6], 0u);
    ip[8] = 64u;
    ip[9] = 17u;
    be16_write(&ip[10], 0u);
    ip[12] = 0u; ip[13] = 0u; ip[14] = 0u; ip[15] = 0u;
    ip[16] = 255u; ip[17] = 255u; ip[18] = 255u; ip[19] = 255u;
    be16_write(&ip[10], inet_checksum(ip, 20u));

    udp = ip + 20u;
    be16_write(&udp[0], DHCP_CLIENT_PORT);
    be16_write(&udp[2], DHCP_SERVER_PORT);
    be16_write(&udp[4], (unsigned short)(8u + dhcp_len));
    be16_write(&udp[6], 0u); /* IPv4 permits UDP checksum zero. */

    rc = net_send_raw(frame, frame_len);
    if (rc == 0){
        if (msg_type == DHCPDISCOVER){
            g_dhcp_diag.tx_discover++;
        } else if (msg_type == DHCPREQUEST){
            g_dhcp_diag.tx_request++;
        }
    }
    if (rc != 0){
        g_dhcp_diag.send_fail++;
    }
    return rc;
}

static int parse_dhcp(const unsigned char* data,
                      unsigned int len,
                      unsigned int xid,
                      const unsigned char mac[6],
                      dhcp_msg_t* msg){
    if (!data || !mac || !msg){
        g_dhcp_diag.bad_len++;
        return -1;
    }
    if (len < DHCP_OPTIONS_OFF){
        g_dhcp_diag.bad_len++;
        return -1;
    }
    if (data[0] != 2u || data[1] != 1u || data[2] != 6u){
        g_dhcp_diag.bad_header++;
        return -1;
    }
    if (be32_read(&data[4]) != xid){
        g_dhcp_diag.bad_xid++;
        return -1;
    }
    for (unsigned int i = 0u; i < 6u; i++){
        if (data[28u + i] != mac[i]){
            g_dhcp_diag.bad_mac++;
            return -1;
        }
    }
    if (data[DHCP_MAGIC_OFF] != 99u ||
        data[DHCP_MAGIC_OFF + 1u] != 130u ||
        data[DHCP_MAGIC_OFF + 2u] != 83u ||
        data[DHCP_MAGIC_OFF + 3u] != 99u){
        g_dhcp_diag.bad_magic++;
        return -1;
    }

    for (unsigned int i = 0u; i < sizeof(*msg); i++){
        ((unsigned char*)msg)[i] = 0u;
    }
    ip_copy(msg->yiaddr, &data[16]);

    unsigned int p = DHCP_OPTIONS_OFF;
    while (p < len){
        unsigned char opt = data[p++];
        if (opt == DHCP_OPT_PAD){
            continue;
        }
        if (opt == DHCP_OPT_END){
            break;
        }
        if (p >= len){
            g_dhcp_diag.bad_options++;
            return -1;
        }
        unsigned int opt_len = data[p++];
        if (p + opt_len > len){
            g_dhcp_diag.bad_options++;
            return -1;
        }
        if (opt == DHCP_OPT_MSG_TYPE && opt_len >= 1u){
            msg->msg_type = data[p];
        } else if (opt == DHCP_OPT_ROUTER && opt_len >= 4u){
            ip_copy(msg->router, &data[p]);
            msg->have_router = 1u;
        } else if (opt == DHCP_OPT_DNS && opt_len >= 4u){
            ip_copy(msg->dns, &data[p]);
            msg->have_dns = 1u;
        } else if (opt == DHCP_OPT_SERVER_ID && opt_len >= 4u){
            ip_copy(msg->server_id, &data[p]);
            msg->have_server_id = 1u;
        } else if (opt == DHCP_OPT_LEASE && opt_len >= 4u){
            msg->lease_seconds = be32_read(&data[p]);
        }
        p += opt_len;
    }
    if (msg->msg_type == 0u){
        g_dhcp_diag.bad_options++;
        return -1;
    }
    if (ip_is_zero(msg->yiaddr)){
        g_dhcp_diag.bad_yiaddr++;
        return -1;
    }
    return 0;
}

static void drain_dhcp_rx(void){
    unsigned char buf[UDP_MAX_PAYLOAD];
    udp_meta_t meta;
    int n;
    /*
     * DHCP runs while the interface address is being replaced. Any stale UDP
     * packet can fill the small UDP queue and cause the OFFER/ACK to be
     * dropped before this client sees it, so clear the whole queue first.
     */
    do {
        n = udp_recv_next(buf, sizeof(buf), &meta);
    } while (n > 0);

    do {
        n = udp_recv_filtered(DHCP_CLIENT_PORT, 0, 0, 0, buf, sizeof(buf), &meta);
    } while (n > 0);
}

static int wait_dhcp_msg(unsigned char want_type,
                         unsigned int xid,
                         const unsigned char mac[6],
                         unsigned int timeout_ms,
                         dhcp_msg_t* out_msg){
    unsigned long start = system_ticks;
    unsigned char buf[UDP_MAX_PAYLOAD];
    udp_meta_t meta;
    unsigned int loops = 0u;
    unsigned int max_loops = (timeout_ms * 80u) + 4000u;
    while ((unsigned long)(system_ticks - start) < (unsigned long)timeout_ms &&
           loops < max_loops){
        (void)net_poll();
        for (;;){
            int n = udp_recv_filtered(DHCP_CLIENT_PORT, 0, 0, 0, buf, sizeof(buf), &meta);
            if (n <= 0){
                break;
            }
            dhcp_msg_t msg;
            g_dhcp_diag.rx_udp68++;
            if (parse_dhcp(buf, (unsigned int)n, xid, mac, &msg) == 0){
                g_dhcp_diag.parse_ok++;
                g_dhcp_diag.last_msg_type = msg.msg_type;
                dhcp_diag_copy_ip(g_dhcp_diag.last_offer_ip, msg.yiaddr);
                if (msg.have_server_id){
                    dhcp_diag_copy_ip(g_dhcp_diag.last_server_id, msg.server_id);
                }
                if (msg.msg_type == want_type){
                    *out_msg = msg;
                    return 0;
                }
                g_dhcp_diag.wrong_type++;
            }
        }
        for (volatile unsigned int spin = 0u; spin < 250u; spin++){
            asm volatile("yield" : : : "memory");
        }
        loops++;
    }
    return -1;
}

static int send_wait_dhcp_msg(unsigned char send_type,
                              unsigned char want_type,
                              unsigned int xid,
                              const unsigned char mac[6],
                              const unsigned char req_ip[4],
                              const unsigned char server_id[4],
                              unsigned int total_timeout_ms,
                              unsigned int send_stage,
                              unsigned int wait_stage,
                              dhcp_msg_t* out_msg){
    unsigned long start = system_ticks;
    unsigned int slice_ms = 700u;

    if (total_timeout_ms < slice_ms){
        slice_ms = total_timeout_ms;
    }
    if (slice_ms < 250u){
        slice_ms = 250u;
    }

    while ((unsigned long)(system_ticks - start) < (unsigned long)total_timeout_ms){
        unsigned long elapsed = (unsigned long)(system_ticks - start);
        unsigned int remain = (elapsed >= (unsigned long)total_timeout_ms)
            ? 0u
            : (unsigned int)((unsigned long)total_timeout_ms - elapsed);
        unsigned int wait_ms = (remain < slice_ms) ? remain : slice_ms;

        g_dhcp_diag.stage = send_stage;
        if (send_dhcp_broadcast(send_type, xid, mac, req_ip, server_id) != 0){
            return -1;
        }

        g_dhcp_diag.stage = wait_stage;
        if (wait_dhcp_msg(want_type, xid, mac, wait_ms, out_msg) == 0){
            return 0;
        }
    }
    return -1;
}

void dhcp_get_diag(dhcp_diag_t* out){
    if (!out){
        return;
    }
    *out = g_dhcp_diag;
}

int dhcp_acquire(unsigned int timeout_ms, dhcp_lease_t* lease_out){
    unsigned char old_ip[4];
    unsigned char old_gw[4];
    unsigned char zero_ip[4] = {0u, 0u, 0u, 0u};
    unsigned char mac[6];
    unsigned int xid;
    dhcp_msg_t offer;
    dhcp_msg_t ack;
    unsigned int half_timeout = timeout_ms / 2u;
    if (half_timeout < 1000u){
        half_timeout = 1000u;
    }

    net_proto_get_local_ip(old_ip);
    net_proto_get_gateway_ip(old_gw);
    net_proto_get_local_mac(mac);
    xid = make_xid(mac);
    dhcp_diag_reset();
    g_dhcp_diag.xid = xid;
    for (unsigned int i = 0u; i < 6u; i++){
        g_dhcp_diag.client_mac[i] = mac[i];
    }

    net_proto_set_local_ip(zero_ip);
    net_proto_set_gateway_ip(zero_ip);
    drain_dhcp_rx();

    uart_puts("DHCP: discover\n");
    if (send_wait_dhcp_msg(DHCPDISCOVER, DHCPOFFER, xid, mac, 0, 0,
                           half_timeout, 1u, 2u, &offer) != 0){
        uart_puts("DHCP: offer timeout\n");
        net_proto_set_local_ip(old_ip);
        net_proto_set_gateway_ip(old_gw);
        return -1;
    }

    uart_puts("DHCP: offer ip=");
    print_ip(offer.yiaddr);
    uart_puts(" gw=");
    print_ip(offer.have_router ? offer.router : zero_ip);
    uart_puts("\n");

    if (send_wait_dhcp_msg(DHCPREQUEST, DHCPACK, xid, mac, offer.yiaddr,
                           offer.have_server_id ? offer.server_id : zero_ip,
                           half_timeout, 3u, 4u, &ack) != 0){
        uart_puts("DHCP: ack timeout\n");
        net_proto_set_local_ip(old_ip);
        net_proto_set_gateway_ip(old_gw);
        return -2;
    }

    net_proto_set_local_ip(ack.yiaddr);
    if (ack.have_router){
        net_proto_set_gateway_ip(ack.router);
    } else if (offer.have_router){
        net_proto_set_gateway_ip(offer.router);
    } else{
        net_proto_set_gateway_ip(zero_ip);
    }

    if (lease_out){
        ip_copy(lease_out->yiaddr, ack.yiaddr);
        if (ack.have_router){
            ip_copy(lease_out->gateway, ack.router);
        } else if (offer.have_router){
            ip_copy(lease_out->gateway, offer.router);
        } else{
            ip_copy(lease_out->gateway, zero_ip);
        }
        if (ack.have_dns){
            ip_copy(lease_out->dns, ack.dns);
        } else if (offer.have_dns){
            ip_copy(lease_out->dns, offer.dns);
        } else{
            ip_copy(lease_out->dns, zero_ip);
        }
        lease_out->lease_seconds = ack.lease_seconds ? ack.lease_seconds : offer.lease_seconds;
    }

    uart_puts("DHCP: ack ip=");
    print_ip(ack.yiaddr);
    uart_puts(" gw=");
    if (ack.have_router){
        print_ip(ack.router);
    } else if (offer.have_router){
        print_ip(offer.router);
    } else{
        print_ip(zero_ip);
    }
    uart_puts("\n");
    g_dhcp_diag.stage = 5u;
    return 0;
}
