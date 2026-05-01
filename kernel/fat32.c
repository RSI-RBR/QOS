#include "fat32.h"
#include "sdhost.h"
#include "uart.h"
#include "debug.h"

#define SECTOR_SIZE 512
#define MAX_CLUSTER_SIZE (64 * 1024)


static unsigned int fat_start;
static unsigned int data_start;
static unsigned int sectors_per_cluster;
static unsigned int root_cluster;

static unsigned char sector[SECTOR_SIZE] __attribute__((aligned(4096)));

static unsigned int read32(unsigned char *p){
    return ((unsigned int)p[0]) | ((unsigned int)p[1]<<8) | ((unsigned int)p[2]<<16) | ((unsigned int)p[3]<<24);
}

static unsigned short read16(unsigned char *p){
    return ((unsigned short)p[0]) | ((unsigned short)p[1]<<8);
}

int fat32_init(void){
    // Read MBR
    if (sdhost_read_block(0, sector)){
        uart_puts("FAT: failed to read MBR\n");
        return -1;
    }

    if (sector[510] != 0x55 || sector[511] != 0xAA){
        uart_puts("Invalid FAT boot sector.\n");
        return -1;
    }

//    for (int i = 0; i < 512; i++){
//        if (sector[i] == 0xAA){
//            uart_puts("AA found!\n");
//        }
//    }
//
//    uart_puts("ADDR sector = ");
//    uart_puthex((unsigned int)sector);
//    uart_puts("\n");

//    uart_puts("ADDR b0 = ");
//    uart_puthex((unsigned int)&sector[0x1BE + 8]);
//    uart_puts("\n");


    volatile unsigned char *v_sector = (volatile unsigned char *)sector;
    barrier();
    unsigned int b0 = v_sector[454];
//    uart_puts("B");
    unsigned int b1 = v_sector[455];
//    uart_puts("B");
    unsigned int b2 = v_sector[456];
//    uart_puts("B");
    unsigned int b3 = v_sector[457];

    unsigned int test = b0 | (b1 << 8);

    unsigned int test2 = test | (b2 << 16);

    unsigned int partition_lba = test2 | (b3 << 24);
    volatile unsigned int v_partition_lba = (volatile unsigned int)partition_lba;
    check_stack();
//    unsigned int test = sector[0x1BE];
//    unsigned int test2 = sector[0x1BE + 8];
//    unsigned int test3 = sector[0x1BE + 11];
//    uart_puts("Byte OK!\n");
//    unsigned int partition_lba = sector[0x1BE + 8] | (sector[0x1BE + 9] << 8) | (sector[0x1BE + 10] << 16) | (sector[0x1BE + 11] << 24);
//    unsigned int partition_lba = read32(&sector[0x1BE + 8]);
//    unsigned int partition_lba = 0x12345678;
    
//    uart_puts("Reading partition_lba to sector...\n");
    barrier();
    // Read FAT32 boot sector
    if (sdhost_read_block(partition_lba, sector)){
        uart_puts("FAT: failed to read boot sector\n");
        return -1;
    }else{
        uart_puts("Read boot sector!\n");
    }

    barrier();

    v_sector = (volatile unsigned char*)sector;

    unsigned int bytes_per_sector = v_sector[11] | (v_sector[12] << 8);
    sectors_per_cluster = v_sector[13];
    unsigned int reserved = v_sector[14] | (v_sector[15] << 8);
    unsigned int fats = v_sector[16];
    unsigned int sectors_per_fat = v_sector[36] | (v_sector[37] << 8) | (v_sector[38] << 16) | (v_sector[39] << 24);

    root_cluster = v_sector[44] | (v_sector[45] << 8) | (v_sector[46] << 16) | (v_sector[47] << 24);

    fat_start = partition_lba + reserved;
    data_start = fat_start + (fats * sectors_per_fat);

    return 0;
}

static int name_match(unsigned char *entry, const char *name){
    // FAT uses 8.3 uppercase
    for (int i = 0; i < 11; i++){
        char c1 = entry[i];
        char c2 = name[i];
        if (c2 == '\0') c2 = ' ';

        if (name[i] != entry[i]) return 0;
    }
    return 1;
}


static int read_cluster(unsigned int cluster, unsigned char *buffer){
    unsigned int lba = data_start + (cluster - 2) * sectors_per_cluster;
    for (unsigned int i = 0; i < sectors_per_cluster; i++){
        if (sdhost_read_block(lba + i, buffer + i * SECTOR_SIZE)){
            uart_puts("FAT read_cluster fail cl=");
            uart_puthex(cluster);
            uart_puts(" lba=");
            uart_puthex(lba + i);
            uart_puts("\n");
            return -1;
        }
    }
    return 0;
}

static unsigned int fat_next(unsigned int cluster){
    unsigned int fat_offset = cluster * 4;
    unsigned int fat_sector = fat_start + (fat_offset / SECTOR_SIZE);
    unsigned int offset = fat_offset % SECTOR_SIZE;

    if (sdhost_read_block(fat_sector, sector)){
        uart_puts("FAT fat_next read fail cl=");
        uart_puthex(cluster);
        uart_puts(" fatsec=");
        uart_puthex(fat_sector);
        uart_puts("\n");
        return 0x0FFFFFFF;
    }

    return read32(&sector[offset]) & 0x0FFFFFFF;
}

int fat32_read_file(const char *name, unsigned char *buffer, int max_size){
    static unsigned char cluster_buf[MAX_CLUSTER_SIZE]; // assume <=8 sectors
    unsigned int cluster_size = sectors_per_cluster * SECTOR_SIZE;
    unsigned int cluster = root_cluster;

    if (cluster_size > MAX_CLUSTER_SIZE){
        uart_puts("Cluster too big.\n");
        return -1;
    }

    while (cluster < 0x0FFFFFF8){
        if (read_cluster(cluster, cluster_buf)){
            uart_puts("FAT root walk fail at cl=");
            uart_puthex(cluster);
            uart_puts("\n");
            return -1;
        }

        for (int i = 0; i < cluster_size; i += 32){
            unsigned char *entry = &cluster_buf[i];

            if (entry[0] == 0x00){
                uart_puts("FAT end marker reached (file not found in chain)\n");
                return -1; // end
            }
            if (entry[0] == 0xE5) continue;  // deleted
            if (entry[11] == 0x0F) continue; // long name

            if (name_match(entry, name)){
                unsigned int first_cluster =
                    (read16(&entry[20]) << 16) |
                     read16(&entry[26]);

                if (first_cluster < 2){
                    uart_puts("Invalid cluster.\n");
                    return -1;
                }

                unsigned int size = read32(&entry[28]);

                unsigned int copied = 0;

                while (first_cluster < 0x0FFFFFF8 && copied < size){
                    if (read_cluster(first_cluster, cluster_buf)){
                        uart_puts("FAT file cluster read fail cl=");
                        uart_puthex(first_cluster);
                        uart_puts(" copied=");
                        uart_puthex(copied);
                        uart_puts("\n");
                        return -1;
                    }
                    unsigned int to_copy = cluster_size;
                    if (to_copy > (size - copied)){
                        to_copy = size - copied;
                    }
                    for (int j = 0; j < to_copy; j++){
                        buffer[copied++] = cluster_buf[j];
                    }
                    if (copied < size){
                        first_cluster = fat_next(first_cluster);
                        if (first_cluster >= 0x0FFFFFF8 && copied < size){
                            uart_puts("FAT file chain ended early copied=");
                            uart_puthex(copied);
                            uart_puts(" size=");
                            uart_puthex(size);
                            uart_puts("\n");
                        }
                    }
                }

                return copied;
            }
        }

        cluster = fat_next(cluster);
    }

    uart_puts("FAT root chain exhausted\n");
    uart_puts("File not found\n");
    return -1;
}

