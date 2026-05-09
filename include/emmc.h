#ifndef EMMC_H
#define EMMC_H

int emmc_init(void);
int emmc_read_block(unsigned int lba, unsigned char *buffer);
int emmc_read_blocks(unsigned int lba, unsigned int count, unsigned char *buffer);

#endif
