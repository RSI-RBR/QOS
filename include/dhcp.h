#ifndef DHCP_H
#define DHCP_H

typedef struct {
    unsigned char yiaddr[4];
    unsigned char gateway[4];
    unsigned char dns[4];
    unsigned int lease_seconds;
} dhcp_lease_t;

/*
 * Minimal DHCPv4 client for headless WiFi probes.
 * Returns 0 on success and applies the lease to net_proto.
 */
int dhcp_acquire(unsigned int timeout_ms, dhcp_lease_t* lease_out);

#endif
