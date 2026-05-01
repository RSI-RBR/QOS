#ifndef NET_H
#define NET_H

#define NET_MAX_FRAME_SIZE 1536

typedef void (*net_rx_callback_t)(const unsigned char* frame, unsigned int len);

int net_init(void);
int net_ready(void);
const char* net_driver_name(void);
int net_link_up(void);

int net_send_raw(const unsigned char* frame, unsigned int len);
int net_recv_raw(unsigned char* out, unsigned int out_cap);
int net_poll(void);
int net_send_test_frame(void);

void net_set_rx_callback(net_rx_callback_t cb);
void net_dump_stats(void);

// Called by NIC drivers when a frame is received.
void net_ingest_rx_from_driver(const unsigned char* frame, unsigned int len);

#endif
