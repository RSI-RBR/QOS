#ifndef FAT32_H
#define FAT32_H

int fat32_init(void);
int fat32_read_file(const char *name, unsigned char *buffer, int max_size);
int fat32_read_file_in_dir_path(const char root_dir_83[11],
                                const char *relative_path,
                                unsigned char *buffer,
                                int max_size);

#endif
