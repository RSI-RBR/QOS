#ifndef SD_H
#define SD_H

#include <stdint.h>

int sd_init();
int sd_read_block(uint32_t lba, uint8_t *buffer);

int sd_read_file(const char *path, void *buf, int max_size);

int sd_read_blocks(uint32_t lba, uint8_t *buffer, int count);

#endif
