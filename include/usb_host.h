#ifndef USB_HOST_H
#define USB_HOST_H

#define USB_HOST_MAX_TRACKED_PORTS 8u

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
int usb_host_bulk_transfer(unsigned char dev_addr,
                           unsigned char ep_addr,
                           unsigned int ep_mps,
                           unsigned char* data,
                           unsigned int len,
                           int in_transfer);

typedef struct {
    int present;
    int configured;
    int child_present;
    int child_configured;
    int child_hid_kbd_present;
    int child_hid_mouse_present;
    unsigned char address;
    unsigned char child_address;
    unsigned char child_hid_kbd_address;
    unsigned char child_hid_mouse_address;
    unsigned char ep0_mps;
    unsigned char dev_class;
    unsigned char child_class;
    unsigned char dev_subclass;
    unsigned char dev_protocol;
    unsigned char config_value;
    unsigned char child_config_value;
    unsigned char child_hub_address;
    unsigned char child_hub_port;
    unsigned char child_bulk_in_ep;
    unsigned char child_bulk_out_ep;
    unsigned char child_hid_kbd_ep;
    unsigned char child_hid_mouse_ep;
    unsigned char child_hid_kbd_iface;
    unsigned char child_hid_mouse_iface;
    unsigned short child_bulk_in_mps;
    unsigned short child_bulk_out_mps;
    unsigned short child_hid_kbd_mps;
    unsigned short child_hid_mouse_mps;
    unsigned short vid;
    unsigned short pid;
    unsigned short child_vid;
    unsigned short child_pid;
    unsigned short config_total_len;
    unsigned int hid_poll_count;
    unsigned int hid_report_count;
    unsigned int hid_nodata_count;
    unsigned int hid_error_count;
    unsigned int hid_last_actual;
    unsigned int hid_stale_clear_count;
    unsigned char hub_ports;
    unsigned char hub_enum_attempts;
    unsigned char hub_enum_success;
    unsigned char hub_hid_candidates;
    unsigned int hub_connected_mask;
    unsigned int hub_enum_success_mask;
    unsigned int hub_hid_candidate_mask;
    unsigned char port_addr[USB_HOST_MAX_TRACKED_PORTS];
    unsigned char port_class[USB_HOST_MAX_TRACKED_PORTS];
    unsigned char port_config[USB_HOST_MAX_TRACKED_PORTS];
    unsigned char port_intr_in_ep[USB_HOST_MAX_TRACKED_PORTS];
    unsigned short port_vid[USB_HOST_MAX_TRACKED_PORTS];
    unsigned short port_pid[USB_HOST_MAX_TRACKED_PORTS];
    unsigned short port_intr_in_mps[USB_HOST_MAX_TRACKED_PORTS];
} usb_root_device_info_t;

typedef struct {
    int present;
    int x;
    int y;
    unsigned int buttons;
    unsigned int seq;
} usb_mouse_state_t;

// Enumerate the root-port attached device (Default->Address->Configured).
int usb_host_enumerate_root_device(void);
int usb_host_get_root_device_info(usb_root_device_info_t* out_info);
void usb_host_poll(void);
void usb_host_poll_mouse(void);
int usb_host_try_getc(char* out);
void usb_host_flush_input(void);
int usb_host_get_mouse_state(usb_mouse_state_t* out);

#endif
