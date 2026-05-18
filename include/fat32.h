#ifndef FAT32_H
#define FAT32_H

int fat32_init(void);
int fat32_read_file(const char *name, unsigned char *buffer, int max_size);
int fat32_read_file_in_dir_path(const char root_dir_83[11],
                                const char *relative_path,
                                unsigned char *buffer,
                                int max_size);
int fat32_read_file_in_dir_path_any(const char root_dir_83[11],
                                    const char *relative_path,
                                    unsigned char *buffer,
                                    int max_size);
int fat32_read_file_in_dir_path_any_at(const char root_dir_83[11],
                                       const char *relative_path,
                                       unsigned int offset,
                                       unsigned char *buffer,
                                       unsigned int max_size);
int fat32_write_file_in_dir_path_existing(const char root_dir_83[11],
                                          const char *relative_path,
                                          const unsigned char *data,
                                          unsigned int size);

#endif
