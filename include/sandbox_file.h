#ifndef SANDBOX_FILE_H
#define SANDBOX_FILE_H

#define SANDBOX_FILE_PATH_MAX 96u
#define SANDBOX_FILE_WRITE_MAX 8192u
#define SANDBOX_FILE_MAX_BYTES (1024u * 1024u)

void sandbox_file_init(void);

int sandbox_file_append(const char sandbox83[11],
                        const char* relative_path,
                        const unsigned char* data,
                        unsigned int len);

int sandbox_file_read(const char sandbox83[11],
                      const char* relative_path,
                      unsigned char* out,
                      unsigned int out_cap);

int sandbox_file_size(const char sandbox83[11], const char* relative_path);
int sandbox_file_clear(const char sandbox83[11], const char* relative_path);

#endif
