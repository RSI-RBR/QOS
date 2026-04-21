#include "sd.h"

int sd_init(){
    return 1;
}

int sd_read_block(uint32_t lba, uint8_t *buffer){
    // placeholder (real driver later)
    return -1;
}

int sd_read_file(const char *path, void *buf, int max_size){
    (void)path;

    sd_read_blocks(100, buf, (max_size / 512));

    return max_size;
}

int sd_read_blocks(uint32_t lba, uint8_t *buffer, int count){
    for (int i = 0; i < count; i++){
        if (sd_read_block(lba + i, buffer + (i * 512)) !=0) return -1;
    }

    return 0;
}

