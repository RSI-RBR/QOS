#ifndef BLOCKDEV_H
#define BLOCKDEV_H

int blockdev_init(void);
int blockdev_reinit(void);
int blockdev_read_block(unsigned int lba, unsigned char *buffer);
const char* blockdev_name(void);

#endif
