#ifndef BLOCKDEV_H
#define BLOCKDEV_H

int blockdev_init(void);
int blockdev_reinit(void);
int blockdev_reinit_emmc(void);
int blockdev_read_block(unsigned int lba, unsigned char *buffer);
int blockdev_read_blocks(unsigned int lba, unsigned int count, unsigned char *buffer);
const char* blockdev_name(void);
void blockdev_reserve_emmc_for_wifi(int reserved);

#endif
