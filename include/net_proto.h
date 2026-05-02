#ifndef NET_PROTO_H
#define NET_PROTO_H

// Kernel protocol pipeline scaffold:
// raw frame -> Ethernet parse -> ARP/IPv4 handlers.
void net_proto_init(void);
void net_proto_configure_defaults(void);
void net_proto_handle_frame(const unsigned char* frame, unsigned int len);
void net_proto_dump_stats(void);

#endif
