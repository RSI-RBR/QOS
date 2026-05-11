#ifndef QOS_PLATFORM_BOARD_H
#define QOS_PLATFORM_BOARD_H

const char* board_name(void);
int board_has_dwc2_usb(void);
int board_has_usb_ethernet(void);
int board_has_onboard_wifi(void);
int board_has_v3d(void);
int board_has_qpu(void);
int board_power_on_usb(void);

#endif
