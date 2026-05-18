#include "fat32.h"
#include "blockdev.h"
#include "uart.h"
#include "debug.h"
#include "spinlock.h"

#define SECTOR_SIZE 512
#define MAX_CLUSTER_SIZE (64 * 1024)
#define FAT_LFN_ATTR 0x0F
#define FAT_LFN_MAX_CHARS 128
#define FAT_SECTOR_CACHE_ENTRIES 128


static unsigned int fat_start;
static unsigned int data_start;
static unsigned int sectors_per_cluster;
static unsigned int sectors_per_fat_global;
static unsigned int fat_count_global;
static unsigned int total_clusters_global;
static unsigned int root_cluster;
static int fat_initialized;

static unsigned char sector[SECTOR_SIZE] __attribute__((aligned(4096)));
static unsigned char cluster_buf[MAX_CLUSTER_SIZE];
static spinlock_t fat_lock_state;

typedef struct fat_sector_cache_entry {
    unsigned int valid;
    unsigned int lba;
    unsigned char data[SECTOR_SIZE];
} fat_sector_cache_entry_t;

static fat_sector_cache_entry_t sector_cache[FAT_SECTOR_CACHE_ENTRIES] __attribute__((aligned(64)));

typedef struct {
    unsigned char entry[32];
    unsigned int sector_lba;
    unsigned int sector_offset;
} fat_dirent_ref_t;

static unsigned long fat_lock(void){
    /*
     * FAT keeps shared scratch buffers and a shared sector cache. Under SMP,
     * two readers would otherwise corrupt each other's directory/file walks.
     * Keep IRQs enabled because the EMMC/SD wait paths use system_ticks.
     */
    spin_lock(&fat_lock_state);
    return 0;
}

static void fat_unlock(unsigned long irq){
    (void)irq;
    spin_unlock(&fat_lock_state);
}

static void fat_cache_reset(void){
    for (unsigned int i = 0u; i < FAT_SECTOR_CACHE_ENTRIES; i++){
        sector_cache[i].valid = 0u;
        sector_cache[i].lba = 0u;
    }
}

static void fat_copy(unsigned char* dst, const unsigned char* src, unsigned int len){
    for (unsigned int i = 0u; i < len; i++){
        dst[i] = src[i];
    }
}

static int fat_read_sector_cached(unsigned int lba, unsigned char* out){
    fat_sector_cache_entry_t* slot;

    if (!out){
        return -1;
    }

    for (unsigned int i = 0u; i < FAT_SECTOR_CACHE_ENTRIES; i++){
        if (sector_cache[i].valid && sector_cache[i].lba == lba){
            fat_copy(out, sector_cache[i].data, SECTOR_SIZE);
            return 0;
        }
    }

    slot = &sector_cache[lba % FAT_SECTOR_CACHE_ENTRIES];
    if (blockdev_read_block(lba, slot->data)){
        slot->valid = 0u;
        return -1;
    }
    barrier();

    slot->lba = lba;
    slot->valid = 1u;
    fat_copy(out, slot->data, SECTOR_SIZE);
    return 0;
}

static void fat_cache_invalidate_lba(unsigned int lba){
    for (unsigned int i = 0u; i < FAT_SECTOR_CACHE_ENTRIES; i++){
        if (sector_cache[i].valid && sector_cache[i].lba == lba){
            sector_cache[i].valid = 0u;
            sector_cache[i].lba = 0u;
        }
    }
}

static int fat_write_sector_uncached(unsigned int lba, const unsigned char* data){
    if (!data){
        return -1;
    }
    fat_cache_invalidate_lba(lba);
    if (blockdev_write_block(lba, data) != 0){
        fat_cache_reset();
        return -1;
    }
    fat_cache_invalidate_lba(lba);
    return 0;
}

static void fat_spin_delay(unsigned int count){
    while (count--){
        asm volatile("nop");
    }
}

static unsigned int read32(unsigned char *p){
    return ((unsigned int)p[0]) | ((unsigned int)p[1]<<8) | ((unsigned int)p[2]<<16) | ((unsigned int)p[3]<<24);
}

static void write32(unsigned char *p, unsigned int v){
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static unsigned short read16(unsigned char *p){
    return ((unsigned short)p[0]) | ((unsigned short)p[1]<<8);
}

static void write16(unsigned char *p, unsigned int v){
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static int fat32_init_locked(void){
    if (fat_initialized){
        return 0;
    }

    fat_cache_reset();

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
        unsigned int total_sectors = v_sector[32] | (v_sector[33] << 8) |
                                     (v_sector[34] << 16) | (v_sector[35] << 24);
        if (total_sectors == 0u){
            total_sectors = v_sector[19] | (v_sector[20] << 8);
        }

        int spc_pow2 = sectors_per_cluster &&
            ((sectors_per_cluster & (sectors_per_cluster - 1)) == 0);
        if (bytes_per_sector != 512 ||
            !spc_pow2 ||
            reserved == 0 ||
            fats == 0 || fats > 2 ||
            sectors_per_fat == 0 ||
            root_cluster < 2 ||
            total_sectors <= reserved + (fats * sectors_per_fat)){
            if (attempt == 3){
                uart_puts("FAT: boot sector sanity failed\n");
                return -1;
            }
            fat_spin_delay(300000);
            continue;
        }

        fat_start = partition_lba + reserved;
        data_start = fat_start + (fats * sectors_per_fat);
        sectors_per_fat_global = sectors_per_fat;
        fat_count_global = fats;
        total_clusters_global = (total_sectors - reserved - (fats * sectors_per_fat)) / sectors_per_cluster;
        fat_initialized = 1;
        return 0;
    }

    return -1;
}

int fat32_init(void){
    unsigned long irq = fat_lock();
    int rc = fat32_init_locked();
    fat_unlock(irq);
    return rc;
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

static int path_component_is_dot_or_dotdot(const char* start, int len){
    if (!start){
        return 1;
    }
    return (len == 1 && start[0] == '.') ||
           (len == 2 && start[0] == '.' && start[1] == '.');
}

static int path_component_has_ext_ci(const char* start, int len, const char* ext){
    int dot = -1;
    int ext_len = 0;
    if (!start || !ext || len <= 0){
        return 0;
    }
    for (int i = 0; i < len; i++){
        if (start[i] == '.'){
            dot = i;
        }
    }
    if (dot < 0 || dot == len - 1){
        return 0;
    }
    while (ext[ext_len]){
        ext_len++;
    }
    if ((len - dot - 1) != ext_len){
        return 0;
    }
    for (int i = 0; i < ext_len; i++){
        if (fat_ascii_upper(start[dot + 1 + i]) != fat_ascii_upper(ext[i])){
            return 0;
        }
    }
    return 1;
}

static int ascii_equal_ci_n(const char* a, int a_len, const char* b){
    if (!a || !b || a_len < 0){
        return 0;
    }
    for (int i = 0; i < a_len; i++){
        if (!b[i]){
            return 0;
        }
        if (fat_ascii_upper(a[i]) != fat_ascii_upper(b[i])){
            return 0;
        }
    }
    return b[a_len] == 0;
}

static void lfn_reset(char* lfn, int* valid){
    if (valid){
        *valid = 0;
    }
    if (lfn){
        for (int i = 0; i < FAT_LFN_MAX_CHARS; i++){
            lfn[i] = 0;
        }
    }
}

static int lfn_copy_char(char* lfn, unsigned int offset, unsigned short ch){
    if (!lfn || offset + 1u >= FAT_LFN_MAX_CHARS){
        return -1;
    }
    if (ch == 0x0000u){
        lfn[offset] = 0;
        return 1;
    }
    if (ch == 0xFFFFu){
        return 0;
    }
    if (ch < 0x20u || ch > 0x7Eu){
        return -1;
    }
    lfn[offset] = (char)(ch & 0xFFu);
    return 0;
}

static void lfn_apply_entry(const unsigned char* entry, char* lfn, int* valid){
    static const unsigned char lfn_pos[13] = {
        1, 3, 5, 7, 9,
        14, 16, 18, 20, 22, 24,
        28, 30
    };
    if (!entry || !lfn || !valid){
        return;
    }

    unsigned int ord = entry[0] & 0x1Fu;
    if (ord == 0u || ord > 20u){
        lfn_reset(lfn, valid);
        return;
    }

    if (entry[0] & 0x40u){
        lfn_reset(lfn, valid);
        *valid = 1;
    } else if (!*valid){
        return;
    }

    unsigned int base = (ord - 1u) * 13u;
    for (unsigned int i = 0; i < 13u; i++){
        unsigned int p = lfn_pos[i];
        unsigned short ch = (unsigned short)entry[p] | ((unsigned short)entry[p + 1u] << 8);
        int rc = lfn_copy_char(lfn, base + i, ch);
        if (rc < 0){
            lfn_reset(lfn, valid);
            return;
        }
        if (rc > 0){
            break;
        }
    }
}

static int read_cluster(unsigned int cluster, unsigned char *buffer){
    unsigned int lba = data_start + (cluster - 2) * sectors_per_cluster;
    for (unsigned int i = 0; i < sectors_per_cluster; i++){
        if (fat_read_sector_cached(lba + i, buffer + i * SECTOR_SIZE)){
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

static unsigned int cluster_lba(unsigned int cluster);

static unsigned int fat_next(unsigned int cluster){
    unsigned int fat_offset = cluster * 4;
    unsigned int fat_sector = fat_start + (fat_offset / SECTOR_SIZE);
    unsigned int offset = fat_offset % SECTOR_SIZE;

    if (fat_read_sector_cached(fat_sector, sector)){
        uart_puts("FAT fat_next read fail cl=");
        uart_puthex(cluster);
        uart_puts(" fatsec=");
        uart_puthex(fat_sector);
        uart_puts("\n");
        return 0x0FFFFFFF;
    }

    return read32(&sector[offset]) & 0x0FFFFFFF;
}

static int fat_write_entry(unsigned int cluster, unsigned int value){
    unsigned int fat_offset = cluster * 4u;
    unsigned int fat_sector = fat_offset / SECTOR_SIZE;
    unsigned int offset = fat_offset % SECTOR_SIZE;

    if (cluster < 2u || cluster >= total_clusters_global + 2u ||
        sectors_per_fat_global == 0u || fat_count_global == 0u){
        return -1;
    }

    value &= 0x0FFFFFFFu;
    for (unsigned int f = 0u; f < fat_count_global; f++){
        unsigned int lba = fat_start + (f * sectors_per_fat_global) + fat_sector;
        if (fat_read_sector_cached(lba, sector) != 0){
            return -1;
        }
        write32(&sector[offset], value);
        if (fat_write_sector_uncached(lba, sector) != 0){
            return -1;
        }
    }
    return 0;
}

static int fat_find_free_cluster(unsigned int* out_cluster){
    if (!out_cluster || total_clusters_global == 0u){
        return -1;
    }
    for (unsigned int c = 2u; c < total_clusters_global + 2u; c++){
        if (fat_next(c) == 0u){
            *out_cluster = c;
            return 0;
        }
    }
    return -1;
}

static int zero_cluster(unsigned int cluster){
    if (cluster < 2u || sectors_per_cluster == 0u || sectors_per_cluster * SECTOR_SIZE > MAX_CLUSTER_SIZE){
        return -1;
    }
    for (unsigned int i = 0u; i < SECTOR_SIZE; i++){
        sector[i] = 0u;
    }
    unsigned int lba = cluster_lba(cluster);
    for (unsigned int s = 0u; s < sectors_per_cluster; s++){
        if (fat_write_sector_uncached(lba + s, sector) != 0){
            return -1;
        }
    }
    return 0;
}

static unsigned int cluster_lba(unsigned int cluster){
    return data_start + (cluster - 2u) * sectors_per_cluster;
}

static int read_cluster_run_to_buffer(unsigned int first_cluster,
                                      unsigned int run_clusters,
                                      unsigned char* buffer,
                                      unsigned int bytes){
    unsigned int lba;
    unsigned int full_sectors;
    unsigned int tail;

    if (!buffer || first_cluster < 2u || run_clusters == 0u || bytes == 0u){
        return -1;
    }

    lba = cluster_lba(first_cluster);
    full_sectors = bytes / SECTOR_SIZE;
    tail = bytes % SECTOR_SIZE;

    if (full_sectors > 0u){
        if (blockdev_read_blocks(lba, full_sectors, buffer) != 0){
            return -1;
        }
    }
    if (tail > 0u){
        if (fat_read_sector_cached(lba + full_sectors, sector) != 0){
            return -1;
        }
        fat_copy(buffer + (full_sectors * SECTOR_SIZE), sector, tail);
    }

    return 0;
}

static int read_file_cluster_chain(unsigned int first_cluster,
                                   unsigned int size,
                                   unsigned char* buffer,
                                   int max_size){
    unsigned int cluster_size = sectors_per_cluster * SECTOR_SIZE;
    unsigned int cluster = first_cluster;
    unsigned int copied = 0u;

    if (!buffer || first_cluster < 2u || max_size <= 0 || size > (unsigned int)max_size){
        return -1;
    }
    if (cluster_size == 0u || cluster_size > MAX_CLUSTER_SIZE){
        uart_puts("Cluster too big.\n");
        return -1;
    }

    while (cluster < 0x0FFFFFF8u && copied < size){
        unsigned int remaining = size - copied;
        unsigned int max_clusters = (remaining + cluster_size - 1u) / cluster_size;
        unsigned int run_clusters = 1u;
        unsigned int next_after_run = 0x0FFFFFFFu;

        while (run_clusters < max_clusters){
            unsigned int current = cluster + run_clusters - 1u;
            unsigned int next = fat_next(current);
            if (next == cluster + run_clusters){
                run_clusters++;
                continue;
            }
            next_after_run = next;
            break;
        }

        unsigned int run_bytes = run_clusters * cluster_size;
        if (run_bytes > remaining){
            run_bytes = remaining;
        }

        if (read_cluster_run_to_buffer(cluster, run_clusters, buffer + copied, run_bytes) != 0){
            uart_puts("FAT file run read fail cl=");
            uart_puthex(cluster);
            uart_puts(" copied=");
            uart_puthex(copied);
            uart_puts("\n");
            return -1;
        }
        copied += run_bytes;

        if (copied >= size){
            break;
        }

        if (next_after_run >= 0x0FFFFFF8u){
            uart_puts("FAT file chain ended early copied=");
            uart_puthex(copied);
            uart_puts(" size=");
            uart_puthex(size);
            uart_puts("\n");
            return -1;
        }
        cluster = next_after_run;
    }

    return (copied == size) ? (int)copied : -1;
}

static int read_file_cluster_chain_at(unsigned int first_cluster,
                                      unsigned int size,
                                      unsigned int offset,
                                      unsigned char* buffer,
                                      unsigned int max_size){
    unsigned int cluster_size = sectors_per_cluster * SECTOR_SIZE;
    unsigned int cluster = first_cluster;
    unsigned int file_pos = 0u;
    unsigned int copied = 0u;

    if (!buffer || max_size == 0u || first_cluster < 2u){
        return -1;
    }
    if (offset >= size){
        return 0;
    }
    if (cluster_size == 0u || cluster_size > MAX_CLUSTER_SIZE){
        uart_puts("Cluster too big.\n");
        return -1;
    }

    while (cluster < 0x0FFFFFF8u && file_pos + cluster_size <= offset){
        file_pos += cluster_size;
        cluster = fat_next(cluster);
    }

    while (cluster < 0x0FFFFFF8u && copied < max_size && offset + copied < size){
        unsigned int lba = cluster_lba(cluster);
        for (unsigned int s = 0u; s < sectors_per_cluster; s++){
            unsigned int sector_file_pos = file_pos + (s * SECTOR_SIZE);
            unsigned int sector_end = sector_file_pos + SECTOR_SIZE;
            unsigned int start_in_sector = 0u;
            unsigned int chunk;

            if (sector_end <= offset){
                continue;
            }
            if (sector_file_pos >= size){
                break;
            }

            if (fat_read_sector_cached(lba + s, sector) != 0){
                return -1;
            }
            if (offset > sector_file_pos){
                start_in_sector = offset - sector_file_pos;
            }
            chunk = SECTOR_SIZE - start_in_sector;
            if (sector_file_pos + start_in_sector + chunk > size){
                chunk = size - (sector_file_pos + start_in_sector);
            }
            if (chunk > max_size - copied){
                chunk = max_size - copied;
            }
            fat_copy(buffer + copied, sector + start_in_sector, chunk);
            copied += chunk;
            if (copied >= max_size || offset + copied >= size){
                break;
            }
        }
        file_pos += cluster_size;
        if (copied >= max_size || offset + copied >= size){
            break;
        }
        cluster = fat_next(cluster);
    }

    return (int)copied;
}

static unsigned int file_chain_capacity(unsigned int first_cluster){
    unsigned int cluster_size = sectors_per_cluster * SECTOR_SIZE;
    unsigned int cluster = first_cluster;
    unsigned int clusters = 0u;

    if (first_cluster < 2u || cluster_size == 0u || cluster_size > MAX_CLUSTER_SIZE){
        return 0u;
    }

    while (cluster < 0x0FFFFFF8u && clusters < 0x100000u){
        clusters++;
        cluster = fat_next(cluster);
    }

    return clusters * cluster_size;
}

static unsigned int file_chain_cluster_count(unsigned int first_cluster, unsigned int* out_last){
    unsigned int cluster = first_cluster;
    unsigned int clusters = 0u;
    unsigned int last = 0u;

    while (cluster >= 2u && cluster < 0x0FFFFFF8u && clusters < 0x100000u){
        last = cluster;
        clusters++;
        cluster = fat_next(cluster);
    }
    if (out_last){
        *out_last = last;
    }
    return clusters;
}

static int ensure_file_chain_capacity(unsigned int* io_first_cluster,
                                      unsigned int desired_size){
    unsigned int cluster_size = sectors_per_cluster * SECTOR_SIZE;
    unsigned int first;
    unsigned int last = 0u;
    unsigned int have_clusters;
    unsigned int need_clusters;

    if (!io_first_cluster || cluster_size == 0u || cluster_size > MAX_CLUSTER_SIZE){
        return -1;
    }
    if (desired_size == 0u){
        return 0;
    }

    first = *io_first_cluster;
    have_clusters = (first >= 2u) ? file_chain_cluster_count(first, &last) : 0u;
    need_clusters = (desired_size + cluster_size - 1u) / cluster_size;

    while (have_clusters < need_clusters){
        unsigned int new_cluster = 0u;
        if (fat_find_free_cluster(&new_cluster) != 0){
            return -1;
        }
        if (fat_write_entry(new_cluster, 0x0FFFFFFFu) != 0 ||
            zero_cluster(new_cluster) != 0){
            return -1;
        }
        if (have_clusters == 0u){
            *io_first_cluster = new_cluster;
        } else if (fat_write_entry(last, new_cluster) != 0){
            return -1;
        }
        last = new_cluster;
        have_clusters++;
    }

    return 0;
}

static int write_file_cluster_chain_existing(unsigned int first_cluster,
                                             const unsigned char* data,
                                             unsigned int size){
    unsigned int cluster_size = sectors_per_cluster * SECTOR_SIZE;
    unsigned int cluster = first_cluster;
    unsigned int written = 0u;

    if (!data && size != 0u){
        return -1;
    }
    if (first_cluster < 2u || cluster_size == 0u || cluster_size > MAX_CLUSTER_SIZE){
        return -1;
    }

    while (cluster < 0x0FFFFFF8u && written < size){
        unsigned int lba = cluster_lba(cluster);
        for (unsigned int s = 0u; s < sectors_per_cluster && written < size; s++){
            unsigned int chunk = size - written;
            if (chunk > SECTOR_SIZE){
                chunk = SECTOR_SIZE;
            }

            if (chunk == SECTOR_SIZE){
                if (fat_write_sector_uncached(lba + s, data + written) != 0){
                    return -1;
                }
            } else{
                if (fat_read_sector_cached(lba + s, sector) != 0){
                    return -1;
                }
                fat_copy(sector, data + written, chunk);
                if (fat_write_sector_uncached(lba + s, sector) != 0){
                    return -1;
                }
            }
            written += chunk;
        }
        if (written >= size){
            break;
        }
        cluster = fat_next(cluster);
    }

    return (written == size) ? 0 : -1;
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

static int find_entry_in_dir_by_component_ref(unsigned int start_cluster,
                                              const char* name,
                                              int name_len,
                                              fat_dirent_ref_t* out_ref){
    unsigned int cluster_size = sectors_per_cluster * SECTOR_SIZE;
    unsigned int cluster = start_cluster;
    char short83[11];
    int short_valid = 0;
    char lfn[FAT_LFN_MAX_CHARS];
    int lfn_valid = 0;

    if (!name || name_len <= 0 || !out_ref || start_cluster < 2){
        return -1;
    }
    if (path_component_is_dot_or_dotdot(name, name_len)){
        return -1;
    }
    if (cluster_size > MAX_CLUSTER_SIZE){
        uart_puts("Cluster too big.\n");
        return -1;
    }

    short_valid = (path_component_to_83(name, name_len, 0, short83) == 0) ? 1 : 0;
    lfn_reset(lfn, &lfn_valid);

    while (cluster < 0x0FFFFFF8){
        if (read_cluster(cluster, cluster_buf)){
            return -1;
        }

        for (int i = 0; i < (int)cluster_size; i += 32){
            unsigned char *entry = &cluster_buf[i];
            if (entry[0] == 0x00){
                return -1;
            }
            if (entry[0] == 0xE5){
                lfn_reset(lfn, &lfn_valid);
                continue;
            }
            if (entry[11] == FAT_LFN_ATTR){
                lfn_apply_entry(entry, lfn, &lfn_valid);
                continue;
            }
            if (entry[11] & 0x08u){
                lfn_reset(lfn, &lfn_valid);
                continue;
            }

            if ((short_valid && name_match(entry, short83)) ||
                (lfn_valid && ascii_equal_ci_n(name, name_len, lfn))){
                copy_entry(out_ref->entry, entry);
                out_ref->sector_lba = cluster_lba(cluster) + ((unsigned int)i / SECTOR_SIZE);
                out_ref->sector_offset = ((unsigned int)i % SECTOR_SIZE);
                return 0;
            }
            lfn_reset(lfn, &lfn_valid);
        }
        cluster = fat_next(cluster);
    }
    return -1;
}

static int find_entry_in_dir_by_component(unsigned int start_cluster,
                                          const char* name,
                                          int name_len,
                                          unsigned char out_entry[32]){
    fat_dirent_ref_t ref;
    if (find_entry_in_dir_by_component_ref(start_cluster, name, name_len, &ref) != 0){
        return -1;
    }
    copy_entry(out_entry, ref.entry);
    return 0;
}

static int fat32_read_file_locked(const char *name, unsigned char *buffer, int max_size){
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
                /*
                 * Rare SD/EMMC reliability case: if the root directory appears
                 * to end before a requested boot file is found, force one
                 * uncached reread before trusting the marker. The old retry
                 * path could hit the same cached sector and was not a real
                 * media retry.
                 */
                if (!retried_root_once && cluster == root_cluster){
                    retried_root_once = 1;
                    fat_cache_reset();
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

                return read_file_cluster_chain(first_cluster, size, buffer, max_size);
            }
        }

        cluster = fat_next(cluster);
    }

    uart_puts("FAT root chain exhausted\n");
    uart_puts("File not found\n");
    return -1;
}

int fat32_read_file(const char *name, unsigned char *buffer, int max_size){
    unsigned long irq = fat_lock();
    int rc;
    if (!fat_initialized && fat32_init_locked() != 0){
        fat_unlock(irq);
        return -1;
    }
    rc = fat32_read_file_locked(name, buffer, max_size);
    if (rc < 0){
        fat_cache_reset();
    }
    fat_unlock(irq);
    return rc;
}

static int fat32_find_path_entry_locked(const char root_dir_83[11],
                                        const char *relative_path,
                                        fat_dirent_ref_t* out_ref,
                                        const char** out_last_comp,
                                        int* out_last_len){
    unsigned char entry[32];
    unsigned int dir_cluster;
    const char* p = relative_path;
    int path_len = 0;

    if (!root_dir_83 || !relative_path || !out_ref){
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

        while (*p && *p != '/' && *p != '\\'){
            p++;
            comp_len++;
        }
        if (comp_len <= 0 || path_component_is_dot_or_dotdot(comp, comp_len)){
            return -1;
        }
        while (*p == '/' || *p == '\\'){
            p++;
            if (*p == '/' || *p == '\\' || *p == 0){
                return -1;
            }
        }
        last = (*p == 0) ? 1 : 0;

        if (last){
            if (find_entry_in_dir_by_component_ref(dir_cluster, comp, comp_len, out_ref) != 0){
                return -1;
            }
            if (out_last_comp){
                *out_last_comp = comp;
            }
            if (out_last_len){
                *out_last_len = comp_len;
            }
            return 0;
        }

        if (find_entry_in_dir_by_component(dir_cluster, comp, comp_len, entry) != 0){
            return -1;
        }
        {
            if (!entry_is_directory(entry)){
                return -1;
            }
            dir_cluster = entry_first_cluster(entry);
            if (dir_cluster < 2){
                return -1;
            }
            continue;
        }
    }

    return -1;
}

static int fat32_read_file_in_dir_path_locked_ex(const char root_dir_83[11],
                                                 const char *relative_path,
                                                 unsigned int offset,
                                                 unsigned char *buffer,
                                                 unsigned int max_size,
                                                 int require_bmp,
                                                 int allow_partial){
    fat_dirent_ref_t ref;
    const char* last_comp = 0;
    int last_len = 0;

    if (!buffer || max_size == 0u){
        return -1;
    }
    if (fat32_find_path_entry_locked(root_dir_83, relative_path, &ref, &last_comp, &last_len) != 0){
        return -1;
    }
    if (entry_is_directory(ref.entry)){
        return -1;
    }
    if (require_bmp && !path_component_has_ext_ci(last_comp, last_len, "BMP")){
        return -1;
    }

    {
        unsigned int size = read32(&ref.entry[28]);
        unsigned int file_cluster = entry_first_cluster(ref.entry);
        if (!allow_partial && offset == 0u && size > max_size){
            uart_puts("FAT sandbox file exceeds buffer\n");
            return -1;
        }
        if (file_cluster < 2){
            return -1;
        }
        if (!allow_partial && offset == 0u && size <= max_size){
            return read_file_cluster_chain(file_cluster, size, buffer, (int)max_size);
        }
        return read_file_cluster_chain_at(file_cluster, size, offset, buffer, max_size);
    }
}

int fat32_read_file_in_dir_path(const char root_dir_83[11],
                                const char *relative_path,
                                unsigned char *buffer,
                                int max_size){
    unsigned long irq = fat_lock();
    int rc;
    if (!fat_initialized && fat32_init_locked() != 0){
        fat_unlock(irq);
        return -1;
    }
    rc = fat32_read_file_in_dir_path_locked_ex(root_dir_83,
                                               relative_path,
                                               0u,
                                               buffer,
                                               (unsigned int)max_size,
                                               1,
                                               0);
    if (rc < 0){
        fat_cache_reset();
    }
    fat_unlock(irq);
    return rc;
}

int fat32_read_file_in_dir_path_any(const char root_dir_83[11],
                                    const char *relative_path,
                                    unsigned char *buffer,
                                    int max_size){
    unsigned long irq = fat_lock();
    int rc;
    if (!fat_initialized && fat32_init_locked() != 0){
        fat_unlock(irq);
        return -1;
    }
    rc = fat32_read_file_in_dir_path_locked_ex(root_dir_83,
                                               relative_path,
                                               0u,
                                               buffer,
                                               (unsigned int)max_size,
                                               0,
                                               0);
    if (rc < 0){
        fat_cache_reset();
    }
    fat_unlock(irq);
    return rc;
}

int fat32_read_file_in_dir_path_any_at(const char root_dir_83[11],
                                       const char *relative_path,
                                       unsigned int offset,
                                       unsigned char *buffer,
                                       unsigned int max_size){
    unsigned long irq = fat_lock();
    int rc;
    if (!fat_initialized && fat32_init_locked() != 0){
        fat_unlock(irq);
        return -1;
    }
    rc = fat32_read_file_in_dir_path_locked_ex(root_dir_83,
                                               relative_path,
                                               offset,
                                               buffer,
                                               max_size,
                                               0,
                                               1);
    if (rc < 0){
        fat_cache_reset();
    }
    fat_unlock(irq);
    return rc;
}

int fat32_write_file_in_dir_path_existing(const char root_dir_83[11],
                                          const char *relative_path,
                                          const unsigned char *data,
                                          unsigned int size){
    unsigned long irq = fat_lock();
    fat_dirent_ref_t ref;
    unsigned int file_cluster;
    unsigned int capacity;
    int rc = -1;

    if (size != 0u && !data){
        return -1;
    }

    if (!fat_initialized && fat32_init_locked() != 0){
        fat_unlock(irq);
        return -1;
    }

    if (fat32_find_path_entry_locked(root_dir_83, relative_path, &ref, 0, 0) != 0 ||
        entry_is_directory(ref.entry)){
        fat_cache_reset();
        fat_unlock(irq);
        return -1;
    }

    file_cluster = entry_first_cluster(ref.entry);
    if (ensure_file_chain_capacity(&file_cluster, size) != 0){
        uart_puts("FAT write cluster allocation failed\n");
        fat_unlock(irq);
        return -1;
    }
    capacity = file_chain_capacity(file_cluster);
    if ((size > 0u && file_cluster < 2u) || size > capacity){
        uart_puts("FAT write exceeds existing file allocation\n");
        fat_unlock(irq);
        return -1;
    }

    if ((size == 0u || write_file_cluster_chain_existing(file_cluster, data, size) == 0) &&
        fat_read_sector_cached(ref.sector_lba, sector) == 0){
        write16(&sector[ref.sector_offset + 20u], (file_cluster >> 16) & 0xFFFFu);
        write16(&sector[ref.sector_offset + 26u], file_cluster & 0xFFFFu);
        write32(&sector[ref.sector_offset + 28u], size);
        rc = fat_write_sector_uncached(ref.sector_lba, sector);
    }

    if (rc != 0){
        fat_cache_reset();
    }
    fat_unlock(irq);
    return rc;
}
