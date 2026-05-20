#ifndef DHCP_H
#define DHCP_H

typedef struct {
    unsigned char yiaddr[4];
    unsigned char gateway[4];
    unsigned char dns[4];
    unsigned int lease_seconds;
} dhcp_lease_t;

typedef struct {
    unsigned int stage;
    unsigned int rx_udp68;
    unsigned int parse_ok;
    unsigned int bad_len;
    unsigned int bad_header;
    unsigned int bad_xid;
    unsigned int bad_mac;
    unsigned int bad_magic;
    unsigned int bad_options;
    unsigned int bad_yiaddr;
    unsigned int wrong_type;
    unsigned int send_fail;
    unsigned char last_msg_type;
    unsigned char last_offer_ip[4];
    unsigned char last_server_id[4];
} dhcp_diag_t;

/*
 * Minimal DHCPv4 client for headless WiFi probes.
 * Returns 0 on success and applies the lease to net_proto.
 */
int dhcp_acquire(unsigned int timeout_ms, dhcp_lease_t* lease_out);
void dhcp_get_diag(dhcp_diag_t* out);

#endif
