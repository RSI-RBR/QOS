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
    char country[4];
    unsigned int sdpcm_tx_seq;
    unsigned int last_scan_count;
} cyw43_status_t;

typedef struct {
    unsigned char fw_running;
    unsigned char iface_up;
    unsigned char joined;
    unsigned char func2_ready;
    unsigned int net_tx_ok;
    unsigned int net_tx_fail;
    unsigned int net_rx_data;
    unsigned int net_rx_event;
    unsigned int net_rx_control;
    unsigned int net_rx_other;
    unsigned int sdpcm_tx_seq;
    unsigned int tx_window;
    unsigned int flow_mask;
    unsigned int rframe_count;
    unsigned int int_pending;
} cyw43_net_diag_t;

typedef struct {
    unsigned int enabled;
    unsigned int queued;
    unsigned int rx_frames;
    unsigned int dropped;
    unsigned int truncated;
    unsigned int last_len;
    unsigned int last_kind;
    unsigned int radiotap_frames;
    unsigned int dot11_frames;
    unsigned int ethernet_frames;
    unsigned int unknown_frames;
} cyw43_raw_capture_status_t;

typedef struct {
    unsigned int enabled;
    unsigned int requested_mode;
    unsigned int monitor;
    unsigned int promisc;
    unsigned int scansuppress;
    unsigned int channel;
    unsigned int raw_enabled;
    int last_rc;
} cyw43_monitor_status_t;

typedef void (*cyw43_rx_handler_t)(const unsigned char* frame, unsigned int len);

int cyw43_init(void);
int cyw43_upload_firmware_from_buffers(const unsigned char* fw_bin,
                                       unsigned int fw_len,
                                       const char* nvram_txt,
                                       unsigned int nvram_len);
int cyw43_upload_firmware_from_fat(const char* fw_bin_83,
                                   const char* nvram_txt_83,
                                   const char* clm_blob_83);

int cyw43_ioctl_up(void);
int cyw43_ioctl_up_monitor(void);
int cyw43_ioctl_down(void);
int cyw43_ioctl_scan(cyw43_scan_result_t* out, unsigned int cap, unsigned int* out_count);
int cyw43_ioctl_scan_ssid(const char* ssid, cyw43_scan_result_t* out, unsigned int cap, unsigned int* out_count);
int cyw43_ioctl_join(const char* ssid, const char* password);
int cyw43_ioctl_set_mac(const unsigned char mac[6]);
int cyw43_ioctl_randomize_mac(void);
int cyw43_get_firmware_version(char* out, unsigned int out_cap);

int cyw43_build_sdpcm(cyw43_sdpcm_hdr_t* hdr,
                      unsigned char channel,
                      unsigned int payload_len,
                      unsigned int seq);

int cyw43_get_status(cyw43_status_t* out);
int cyw43_get_net_diag(cyw43_net_diag_t* out);
void cyw43_dump_status(void);
int cyw43_release_emmc_for_storage(void);
int cyw43_shared_emmc_active(void);
int cyw43_monitor_capture_active(void);

// Minimal Ethernet datapath bridge for the kernel NIC layer.
int cyw43_net_set_rx_handler(cyw43_rx_handler_t handler);
int cyw43_net_poll(void);
int cyw43_net_send_ethernet(const unsigned char* frame, unsigned int len);
int cyw43_net_link_up(void);
int cyw43_net_ready(void);
int cyw43_raw_capture_set_enabled(unsigned int enabled);
int cyw43_raw_capture_is_enabled(void);
int cyw43_raw_capture_poll(void);
int cyw43_raw_capture_poll_lite(void);
int cyw43_raw_capture_recv(unsigned char* out, unsigned int out_cap);
int cyw43_raw_capture_get_status(cyw43_raw_capture_status_t* out);
int cyw43_ioctl_monitor(unsigned int mode, unsigned int channel);
int cyw43_ioctl_monitor_status(cyw43_monitor_status_t* out);
int cyw43_monitor_hard_recover(unsigned int channel);

#endif
