#ifndef NIC_H
#define NIC_H

typedef void (*nic_rx_handler_t)(const unsigned char* frame, unsigned int len);

typedef struct {
    const char* name;
    int (*init)(void);
    int (*poll)(void);
    int (*send)(const unsigned char* frame, unsigned int len);
    int (*set_rx_handler)(nic_rx_handler_t handler);
    int (*link_up)(void);
} nic_driver_t;

// Returns the default NIC backend for this build.
// Current implementation provides a loopback stub to unblock protocol work.
const nic_driver_t* nic_probe_default(void);
const nic_driver_t* nic_probe_smsc95xx(void);
const nic_driver_t* nic_probe_stub(void);

#endif
