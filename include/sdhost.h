#include "uart.h"
#include "debug.h"

unsigned int sdhost_get_resp(void);

void sdhost_reset(void);

int sdhost_cmd(unsigned int cmd, unsigned int arg, unsigned int flags);

void sdhost_check_status(void);

int sdhost_init_card(void);

int sdhost_read_block(unsigned int lba, unsigned char *buffer);
