#include "fat32.h"
//#include "sd.h"
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
    // Read MBR
    if (sdhost_read_block(0, sector)){
        uart_puts("FAT: failed to read MBR\n");
        return -1;
    }

    // Check for partition (offset 0x1BE)
    unsigned int partition_lba = read32(&sector[0x1BE + 8]);

    uart_puts("Partition LBA = ");
    uart_puthex(partition_lba);
    uart_puts("\n");

    // Read FAT32 boot sector
    if (sdhost_read_block(partition_lba, sector)){
        uart_puts("FAT: failed to read boot sector\n");
        return -1;
    }

    unsigned int bytes_per_sector = read16(&sector[11]);
    sectors_per_cluster = sector[13];
    unsigned int reserved = read16(&sector[14]);
    unsigned int fats = sector[16];
    unsigned int sectors_per_fat = read32(&sector[36]);

    root_cluster = read32(&sector[44]);

    fat_start = partition_lba + reserved;
    data_start = fat_start + (fats * sectors_per_fat);

    uart_puts("FAT32 initialized\n");

    uart_puts("SPC=");
    uart_puthex(sectors_per_cluster);
    uart_puts(" ROOT=");
    uart_puthex(root_cluster);
    uart_puts("\n");

    return 0;
}

static int name_match(unsigned char *entry, const char *name){
    // FAT uses 8.3 uppercase
    for (int i = 0; i < 11; i++){
        char c = entry[i];
        if (c == ' ') c = 0;

        if (name[i] != c) return 0;
    }
    return 1;
}


static int read_cluster(unsigned int cluster, unsigned char *buffer){
    unsigned int lba = data_start + (cluster - 2) * sectors_per_cluster;

    for (unsigned int i = 0; i < sectors_per_cluster; i++){
        if (sd_read_block(lba + i, buffer + i * SECTOR_SIZE)){
            return -1;
        }
    }
    return 0;
}

static unsigned int fat_next(unsigned int cluster){
    unsigned int fat_offset = cluster * 4;
    unsigned int fat_sector = fat_start + (fat_offset / SECTOR_SIZE);
    unsigned int offset = fat_offset % SECTOR_SIZE;

    if (sd_read_block(fat_sector, sector)){
        return 0x0FFFFFFF;
    }

    return read32(&sector[offset]) & 0x0FFFFFFF;
}

int fat32_read_file(const char *name, unsigned char *buffer, int max_size){
    unsigned char cluster_buf[4096]; // assume <=8 sectors

    unsigned int cluster = root_cluster;

    while (cluster < 0x0FFFFFF8){
        if (read_cluster(cluster, cluster_buf)){
            return -1;
        }

        for (int i = 0; i < 4096; i += 32){
            unsigned char *entry = &cluster_buf[i];

            if (entry[0] == 0x00) return -1; // end
            if (entry[0] == 0xE5) continue;  // deleted
            if (entry[11] == 0x0F) continue; // long name

            if (name_match(entry, name)){
                unsigned int first_cluster =
                    (read16(&entry[20]) << 16) |
                     read16(&entry[26]);

                unsigned int size = read32(&entry[28]);

                uart_puts("File found\n");

                unsigned int copied = 0;

                while (first_cluster < 0x0FFFFFF8 && copied < size){
                    if (read_cluster(first_cluster, cluster_buf)){
                        return -1;
                    }

                    for (int j = 0; j < 4096 && copied < size; j++){
                        buffer[copied++] = cluster_buf[j];
                    }

                    first_cluster = fat_next(first_cluster);
                }

                return copied;
            }
        }

        cluster = fat_next(cluster);
    }

    uart_puts("File not found\n");
    return -1;
}
