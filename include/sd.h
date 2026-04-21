#ifndef SD_H
#define SD_H

int sd_init();
int sd_read_block(unsigned int lba, unsigned char *buffer);

#endif
