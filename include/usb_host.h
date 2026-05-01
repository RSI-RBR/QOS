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

// Phase 1 scaffold for next step (EP0 control transfer engine).
int usb_host_control_transfer(unsigned char dev_addr,
                              const usb_setup_packet_t* setup,
                              unsigned char* data,
                              unsigned int data_len,
                              int in_transfer);

#endif
