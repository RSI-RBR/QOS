#ifndef NET_PROTO_H
#define NET_PROTO_H

// Kernel protocol pipeline scaffold:
// raw frame -> Ethernet parse -> ARP/IPv4 handlers.
void net_proto_init(void);
void net_proto_configure_defaults(void);
void net_proto_handle_frame(const unsigned char* frame, unsigned int len);
void net_proto_dump_stats(void);
void net_proto_get_local_mac(unsigned char out_mac[6]);
void net_proto_get_local_ip(unsigned char out_ip[4]);
void net_proto_get_gateway_ip(unsigned char out_ip[4]);
void net_proto_set_local_ip(const unsigned char ip[4]);

#endif
