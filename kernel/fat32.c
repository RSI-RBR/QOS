#include "fat32.h"
#include "blockdev.h"
#include "uart.h"
#include "debug.h"

#define SECTOR_SIZE 512
#define MAX_CLUSTER_SIZE (64 * 1024)


static unsigned int fat_start;
static unsigned int data_start;
static unsigned int sectors_per_cluster;
static unsigned int root_cluster;

static unsigned char sector[SECTOR_SIZE] __attribute__((aligned(4096)));
static unsigned char cluster_buf[MAX_CLUSTER_SIZE];
static void fat_spin_delay(unsigned int count){
    while (count--){
        asm volatile("nop");
    }
}

static unsigned int read32(unsigned char *p){
    return ((unsigned int)p[0]) | ((unsigned int)p[1]<<8) | ((unsigned int)p[2]<<16) | ((unsigned int)p[3]<<24);
}

static unsigned short read16(unsigned char *p){
    return ((unsigned short)p[0]) | ((unsigned short)p[1]<<8);
}

int fat32_init(void){
    // Read MBR
    if (blockdev_read_block(0, sector)){
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
    unsigned int partition_lba = read32(&sector[0x1BE + 8]);
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
    // Read FAT32 boot sector with sanity retries.
    for (int attempt = 0; attempt < 4; attempt++){
        if (blockdev_read_block(partition_lba, sector)){
            if (attempt == 3){
                uart_puts("FAT: failed to read boot sector\n");
                return -1;
            }
            fat_spin_delay(300000);
            continue;
        }

        barrier();
        v_sector = (volatile unsigned char*)sector;

        if (v_sector[510] != 0x55 || v_sector[511] != 0xAA){
            if (attempt == 3){
                uart_puts("FAT: invalid boot sector signature\n");
                return -1;
            }
            fat_spin_delay(300000);
            continue;
        }

        unsigned int bytes_per_sector = v_sector[11] | (v_sector[12] << 8);
        sectors_per_cluster = v_sector[13];
        unsigned int reserved = v_sector[14] | (v_sector[15] << 8);
        unsigned int fats = v_sector[16];
        unsigned int sectors_per_fat = v_sector[36] | (v_sector[37] << 8) | (v_sector[38] << 16) | (v_sector[39] << 24);
        root_cluster = v_sector[44] | (v_sector[45] << 8) | (v_sector[46] << 16) | (v_sector[47] << 24);

        int spc_pow2 = sectors_per_cluster &&
            ((sectors_per_cluster & (sectors_per_cluster - 1)) == 0);
        if (bytes_per_sector != 512 ||
            !spc_pow2 ||
            reserved == 0 ||
            fats == 0 || fats > 2 ||
            sectors_per_fat == 0 ||
            root_cluster < 2){
            if (attempt == 3){
                uart_puts("FAT: boot sector sanity failed\n");
                return -1;
            }
            fat_spin_delay(300000);
            continue;
        }

        fat_start = partition_lba + reserved;
        data_start = fat_start + (fats * sectors_per_fat);
        return 0;
    }

    return -1;
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

static void copy_entry(unsigned char dst[32], const unsigned char* src){
    for (int i = 0; i < 32; i++){
        dst[i] = src[i];
    }
}

static int entry_is_directory(const unsigned char* entry){
    return entry && ((entry[11] & 0x10u) != 0u);
}

static unsigned int entry_first_cluster(const unsigned char* entry){
    return ((unsigned int)read16((unsigned char*)&entry[20]) << 16) |
           (unsigned int)read16((unsigned char*)&entry[26]);
}

static char fat_ascii_upper(char c){
    if (c >= 'a' && c <= 'z'){
        return (char)(c - ('a' - 'A'));
    }
    return c;
}

static int fat83_component_char_allowed(char c){
    if (c == '_' || c == '-' || c == '$' || c == '~'){
        return 1;
    }
    if (c >= 'A' && c <= 'Z'){
        return 1;
    }
    if (c >= '0' && c <= '9'){
        return 1;
    }
    return 0;
}

static int path_component_to_83(const char* start,
                                int len,
                                int require_ext,
                                char out83[11]){
    int base_len = 0;
    int ext_len = 0;
    int dot = -1;

    if (!start || !out83 || len <= 0){
        return -1;
    }
    if (len == 1 && start[0] == '.'){
        return -1;
    }
    if (len == 2 && start[0] == '.' && start[1] == '.'){
        return -1;
    }

    for (int i = 0; i < 11; i++){
        out83[i] = ' ';
    }
    for (int i = 0; i < len; i++){
        if (start[i] == '.'){
            if (dot >= 0){
                return -1;
            }
            dot = i;
        }
    }

    int base_end = (dot >= 0) ? dot : len;
    if (base_end <= 0){
        return -1;
    }
    for (int i = 0; i < base_end; i++){
        char c = fat_ascii_upper(start[i]);
        if (!fat83_component_char_allowed(c) || base_len >= 8){
            return -1;
        }
        out83[base_len++] = c;
    }

    if (dot >= 0){
        for (int i = dot + 1; i < len; i++){
            char c = fat_ascii_upper(start[i]);
            if (!fat83_component_char_allowed(c) || ext_len >= 3){
                return -1;
            }
            out83[8 + ext_len++] = c;
        }
    }

    if (require_ext && ext_len == 0){
        return -1;
    }
    return 0;
}

static int read_cluster(unsigned int cluster, unsigned char *buffer){
    unsigned int lba = data_start + (cluster - 2) * sectors_per_cluster;
    for (unsigned int i = 0; i < sectors_per_cluster; i++){
        if (blockdev_read_block(lba + i, buffer + i * SECTOR_SIZE)){
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

    if (blockdev_read_block(fat_sector, sector)){
        uart_puts("FAT fat_next read fail cl=");
        uart_puthex(cluster);
        uart_puts(" fatsec=");
        uart_puthex(fat_sector);
        uart_puts("\n");
        return 0x0FFFFFFF;
    }

    return read32(&sector[offset]) & 0x0FFFFFFF;
}

static int find_entry_in_dir(unsigned int start_cluster,
                             const char name83[11],
                             unsigned char out_entry[32]){
    unsigned int cluster_size = sectors_per_cluster * SECTOR_SIZE;
    unsigned int cluster = start_cluster;

    if (!name83 || !out_entry || start_cluster < 2){
        return -1;
    }
    if (cluster_size > MAX_CLUSTER_SIZE){
        uart_puts("Cluster too big.\n");
        return -1;
    }

    while (cluster < 0x0FFFFFF8){
        if (read_cluster(cluster, cluster_buf)){
            return -1;
        }

        for (int i = 0; i < (int)cluster_size; i += 32){
            unsigned char *entry = &cluster_buf[i];
            if (entry[0] == 0x00){
                return -1;
            }
            if (entry[0] == 0xE5) continue;
            if (entry[11] == 0x0F) continue;
            if (name_match(entry, name83)){
                copy_entry(out_entry, entry);
                return 0;
            }
        }
        cluster = fat_next(cluster);
    }
    return -1;
}

int fat32_read_file(const char *name, unsigned char *buffer, int max_size){
    unsigned int cluster_size = sectors_per_cluster * SECTOR_SIZE;
    unsigned int cluster = root_cluster;
    int retried_root_once = 0;

    if (!name || !buffer || max_size <= 0){
        return -1;
    }

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
                // Rare delayed-run reliability case: if very first root entry looks like end marker,
                // retry a fresh root-cluster read once before declaring file-not-found.
                if (!retried_root_once && i == 0 && cluster == root_cluster){
                    retried_root_once = 1;
                    if (read_cluster(cluster, cluster_buf) != 0){
                        uart_puts("FAT root retry read failed\n");
                        return -1;
                    }
                    i = -32; // loop will add +32, re-check entry 0
                    continue;
                }
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
                if (size > (unsigned int)max_size){
                    uart_puts("FAT file exceeds loader buffer\n");
                    uart_puts("size=");
                    uart_puthex(size);
                    uart_puts(" max=");
                    uart_puthex((unsigned int)max_size);
                    uart_puts("\n");
                    return -1;
                }

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
                    if (to_copy > ((unsigned int)max_size - copied)){
                        to_copy = (unsigned int)max_size - copied;
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

int fat32_read_file_in_dir_path(const char root_dir_83[11],
                                const char *relative_path,
                                unsigned char *buffer,
                                int max_size){
    unsigned char entry[32];
    unsigned int dir_cluster;
    const char* p = relative_path;
    int path_len = 0;

    if (!root_dir_83 || !relative_path || !buffer || max_size <= 0){
        return -1;
    }
    if (relative_path[0] == '/' || relative_path[0] == '\\'){
        return -1;
    }
    for (const char* q = relative_path; *q; q++){
        path_len++;
        if (path_len > 96){
            return -1;
        }
        if (*q == ':' || *q < 32 || *q == 127){
            return -1;
        }
    }
    if (path_len == 0){
        return -1;
    }

    if (find_entry_in_dir(root_cluster, root_dir_83, entry) != 0 ||
        !entry_is_directory(entry)){
        return -1;
    }
    dir_cluster = entry_first_cluster(entry);
    if (dir_cluster < 2){
        return -1;
    }

    while (*p){
        const char* comp = p;
        int comp_len = 0;
        int last = 0;
        char name83[11];

        while (*p && *p != '/' && *p != '\\'){
            p++;
            comp_len++;
        }
        if (comp_len <= 0){
            return -1;
        }
        while (*p == '/' || *p == '\\'){
            p++;
            if (*p == '/' || *p == '\\' || *p == 0){
                return -1;
            }
        }
        last = (*p == 0) ? 1 : 0;

        if (path_component_to_83(comp, comp_len, last, name83) != 0){
            return -1;
        }

        if (find_entry_in_dir(dir_cluster, name83, entry) != 0){
            return -1;
        }

        if (!last){
            if (!entry_is_directory(entry)){
                return -1;
            }
            dir_cluster = entry_first_cluster(entry);
            if (dir_cluster < 2){
                return -1;
            }
            continue;
        }

        if (entry_is_directory(entry)){
            return -1;
        }
        if (name83[8] != 'B' || name83[9] != 'M' || name83[10] != 'P'){
            return -1;
        }

        unsigned int size = read32(&entry[28]);
        if (size > (unsigned int)max_size){
            uart_puts("FAT sandbox file exceeds buffer\n");
            return -1;
        }

        unsigned int file_cluster = entry_first_cluster(entry);
        unsigned int cluster_size = sectors_per_cluster * SECTOR_SIZE;
        unsigned int copied = 0;
        if (file_cluster < 2){
            return -1;
        }
        if (cluster_size > MAX_CLUSTER_SIZE){
            uart_puts("Cluster too big.\n");
            return -1;
        }

        while (file_cluster < 0x0FFFFFF8 && copied < size){
            if (read_cluster(file_cluster, cluster_buf)){
                return -1;
            }
            unsigned int to_copy = cluster_size;
            if (to_copy > (size - copied)){
                to_copy = size - copied;
            }
            for (unsigned int j = 0; j < to_copy; j++){
                buffer[copied++] = cluster_buf[j];
            }
            if (copied < size){
                file_cluster = fat_next(file_cluster);
            }
        }

        return (copied == size) ? (int)copied : -1;
    }

    return -1;
}
