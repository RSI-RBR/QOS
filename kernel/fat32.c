#include "fat32.h"
#include "sd.h"
#include "uart.h"

#define SECTOR_SIZE 512

static unsigned int fat_start;
static unsigned int data_start;
static unsigned int sectors_per_cluster;
static unsigned int root_cluster;

static unsigned char sector[SECTOR_SIZE];

static unsigned int read32(unsigned char *p){
    return p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24);
}

static unsigned short read16(unsigned char *p){
    return p[0] | (p[1]<<8);
}

int fat32_init(void){
    if (sd_read_block(0, sector)){
        uart_puts("FAT: failed to read boot sector\n");
        return -1;
    }

    unsigned int reserved = read16(&sector[14]);
    unsigned int fats = sector[16];
    unsigned int sectors_per_fat = read32(&sector[36]);

    sectors_per_cluster = sector[13];
    root_cluster = read32(&sector[44]);

    fat_start = reserved;
    data_start = reserved + (fats * sectors_per_fat);

    uart_puts("FAT32 initialized\n");
    return 0;
}
