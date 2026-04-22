#ifndef FAT32_H
#define FAT32_H

int fat32_init(void);
int fat32_read_file(const char *name, unsigned char *buffer, int max_size);

#endif
