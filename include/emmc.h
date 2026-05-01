#ifndef EMMC_H
#define EMMC_H

int emmc_init(void);
int emmc_read_block(unsigned int lba, unsigned char *buffer);

#endif
