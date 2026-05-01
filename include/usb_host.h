#ifndef USB_HOST_H
#define USB_HOST_H

typedef struct {
    unsigned char bmRequestType;
    unsigned char bRequest;
    unsigned short wValue;
    unsigned short wIndex;
    unsigned short wLength;
} usb_setup_packet_t;

// Phase 1: controller bring-up (DWC2 host mode + basic port power).
int usb_host_init(void);
int usb_host_ready(void);
void usb_host_dump_state(void);
int usb_host_reset_root_port(void);

// Phase 1 scaffold for next step (EP0 control transfer engine).
int usb_host_control_transfer(unsigned char dev_addr,
                              const usb_setup_packet_t* setup,
                              unsigned char* data,
                              unsigned int data_len,
                              int in_transfer);

int usb_host_read_device_descriptor(unsigned char* out18, unsigned int len);

typedef struct {
    int present;
    int configured;
    unsigned char address;
    unsigned char ep0_mps;
    unsigned char dev_class;
    unsigned char dev_subclass;
    unsigned char dev_protocol;
    unsigned char config_value;
    unsigned short vid;
    unsigned short pid;
    unsigned short config_total_len;
} usb_root_device_info_t;

// Enumerate the root-port attached device (Default->Address->Configured).
int usb_host_enumerate_root_device(void);
int usb_host_get_root_device_info(usb_root_device_info_t* out_info);

#endif
