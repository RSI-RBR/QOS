#ifndef CYW43_H
#define CYW43_H

// SDPCM channel values used by Broadcom/Cypress fullmac transports.
enum {
    CYW43_SDPCM_CH_CONTROL = 0,
    CYW43_SDPCM_CH_EVENT = 1,
    CYW43_SDPCM_CH_DATA = 2,
    CYW43_SDPCM_CH_GLOM = 3,
    CYW43_SDPCM_CH_TEST = 15
};

typedef struct __attribute__((packed)) {
    unsigned short frame_len;
    unsigned short frame_len_cksum;
    unsigned char seq;
    unsigned char channel;
    unsigned char next_len;
    unsigned char hdr_len;
    unsigned char flow_control;
    unsigned char credit;
    unsigned short reserved;
} cyw43_sdpcm_hdr_t;

typedef struct {
    char ssid[33];
    unsigned char channel;
    int rssi_dbm;
    unsigned char auth;
} cyw43_scan_result_t;

typedef struct {
    unsigned char enabled;
    unsigned char ready;
    unsigned char func1_ready;
    unsigned char func2_ready;
    unsigned char fw_loaded;
    unsigned char fw_running;
    unsigned char iface_up;
    unsigned char joined;
    unsigned char mac[6];
    char country[3];
    unsigned int sdpcm_tx_seq;
    unsigned int last_scan_count;
} cyw43_status_t;

typedef void (*cyw43_rx_handler_t)(const unsigned char* frame, unsigned int len);

int cyw43_init(void);
int cyw43_upload_firmware_from_buffers(const unsigned char* fw_bin,
                                       unsigned int fw_len,
                                       const char* nvram_txt,
                                       unsigned int nvram_len);
int cyw43_upload_firmware_from_fat(const char* fw_bin_83, const char* nvram_txt_83);

int cyw43_ioctl_up(void);
int cyw43_ioctl_down(void);
int cyw43_ioctl_scan(cyw43_scan_result_t* out, unsigned int cap, unsigned int* out_count);
int cyw43_ioctl_join(const char* ssid, const char* password);
int cyw43_get_firmware_version(char* out, unsigned int out_cap);

int cyw43_build_sdpcm(cyw43_sdpcm_hdr_t* hdr,
                      unsigned char channel,
                      unsigned int payload_len,
                      unsigned int seq);

int cyw43_get_status(cyw43_status_t* out);
void cyw43_dump_status(void);
int cyw43_release_emmc_for_storage(void);

// Minimal Ethernet datapath bridge for the kernel NIC layer.
int cyw43_net_set_rx_handler(cyw43_rx_handler_t handler);
int cyw43_net_poll(void);
int cyw43_net_send_ethernet(const unsigned char* frame, unsigned int len);
int cyw43_net_link_up(void);
int cyw43_net_ready(void);

#endif
