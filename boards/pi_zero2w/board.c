#include "platform/board.h"
#include "platform/board_config.h"
#include "mailbox.h"

const char* board_name(void){
    return QOS_BOARD_NAME;
}

int board_has_dwc2_usb(void){
    return QOS_BOARD_HAS_DWC2_USB;
}

int board_has_usb_ethernet(void){
    return QOS_BOARD_HAS_USB_ETHERNET;
}

int board_has_onboard_wifi(void){
    return QOS_BOARD_HAS_ONBOARD_WIFI;
}

int board_has_v3d(void){
    return QOS_BOARD_HAS_V3D;
}

int board_has_qpu(void){
    return QOS_BOARD_HAS_QPU;
}

int board_power_on_usb(void){
    return board_has_dwc2_usb() ? mailbox_power_on_usb() : 0;
}
