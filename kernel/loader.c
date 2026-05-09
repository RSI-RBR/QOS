#include "loader.h"
#include "blockdev.h"
#include "mmu.h"
#include "trust.h"
#include "klog.h"
#include "spinlock.h"

#define uart_puts klog_puts
#define uart_puthex klog_puthex


#define PROGRAM_MAX (512 * 1024)
#define PROGRAM_FILE_MAX (768 * 1024)
#define PROGRAM_POOL_START QOS_PROGRAM_POOL_START
#define PROGRAM_POOL_SIZE  QOS_PROGRAM_POOL_SIZE
#define PROGRAM_ALLOC_GRANULE QOS_PROGRAM_ALLOC_GRANULE_BYTES
#define PROGRAM_MAX_MEMORY QOS_PROGRAM_MAX_MEMORY_BYTES
#define PROGRAM_UNIT_COUNT (PROGRAM_POOL_SIZE / PROGRAM_ALLOC_GRANULE)
#define PROGRAM_PAGE_SIZE  4096UL
#define PROGRAM_SEC_LAYOUT_V1_BYTES (sizeof(program_sec_layout_v1_t))

static unsigned char program_unit_used[PROGRAM_UNIT_COUNT];
static unsigned char program_unit_span[PROGRAM_UNIT_COUNT];
static spinlock_t g_program_alloc_lock;


static unsigned char buffer[PROGRAM_FILE_MAX];
static volatile int loader_busy = 0;
static const char* DEFAULT_PROGRAM_83 = "PROGRAM BIN";
static const char GAME_PROGRAM_83[11] = {'G','A','M','E',' ',' ',' ',' ','B','I','N'};
static const char GAME_SANDBOX_83[11] = {'Q','F','2','D',' ',' ',' ',' ',' ',' ',' '};

static unsigned long daif_read(void){
    unsigned long v;
    asm volatile("mrs %0, daif" : "=r"(v));
    return v;
}

static void daif_write(unsigned long v){
    asm volatile("msr daif, %0" : : "r"(v));
}

void loader_mmu_init_pool(void){
    extern unsigned long bss_end;
    if ((unsigned long)&bss_end >= PROGRAM_POOL_START){
        uart_puts("ERROR: program pool overlaps kernel BSS.\n");
        uart_puts("bss_end=");
        uart_puthex((unsigned int)(unsigned long)&bss_end);
        uart_puts(" pool_start=");
        uart_puthex((unsigned int)PROGRAM_POOL_START);
        uart_puts("\n");
        while (1){}
    }

    spinlock_init(&g_program_alloc_lock);

    // Keep pool inaccessible to EL0 until a program block is explicitly mapped.
    mmu_map_kernel_private_region(PROGRAM_POOL_START, PROGRAM_POOL_SIZE);
    for (unsigned int i = 0; i < PROGRAM_UNIT_COUNT; i++){
        program_unit_used[i] = 0;
        program_unit_span[i] = 0;
    }
}

static int loader_try_lock(void){
    int taken;
    unsigned long daif_prev = daif_read();
    asm volatile("msr daifset, #2");
    taken = loader_busy;
    if (!taken){
        loader_busy = 1;
    }
    daif_write(daif_prev);
    return !taken;
}

static void loader_unlock(void){
    unsigned long daif_prev = daif_read();
    asm volatile("msr daifset, #2");
    loader_busy = 0;
    daif_write(daif_prev);
}

static int fat83_equal11(const char* a, const char* b){
    if (!a || !b){
        return 0;
    }
    for (unsigned int i = 0; i < 11u; i++){
        if (a[i] != b[i]){
            return 0;
        }
    }
    return 1;
}

static unsigned int get_u32_le_local(const unsigned char* p){
    return (unsigned int)p[0] |
           ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

static unsigned long long get_u64_le_local(const unsigned char* p){
    return (unsigned long long)p[0] |
           ((unsigned long long)p[1] << 8) |
           ((unsigned long long)p[2] << 16) |
           ((unsigned long long)p[3] << 24) |
           ((unsigned long long)p[4] << 32) |
           ((unsigned long long)p[5] << 40) |
           ((unsigned long long)p[6] << 48) |
           ((unsigned long long)p[7] << 56);
}

static void put_u64_le_local(unsigned char* p, unsigned long long v){
    p[0] = (unsigned char)(v & 0xFFULL);
    p[1] = (unsigned char)((v >> 8) & 0xFFULL);
    p[2] = (unsigned char)((v >> 16) & 0xFFULL);
    p[3] = (unsigned char)((v >> 24) & 0xFFULL);
    p[4] = (unsigned char)((v >> 32) & 0xFFULL);
    p[5] = (unsigned char)((v >> 40) & 0xFFULL);
    p[6] = (unsigned char)((v >> 48) & 0xFFULL);
    p[7] = (unsigned char)((v >> 56) & 0xFFULL);
}

static int loader_apply_relative_relocs(unsigned char* image,
                                        unsigned long reservation_size,
                                        const unsigned char* reloc_entries,
                                        unsigned int reloc_count){
    if (!image){
        return -1;
    }
    if (reloc_count == 0u){
        return 0;
    }
    if (!reloc_entries || reloc_count > QOS_PROGRAM_MAX_RELOCS){
        return -1;
    }

    unsigned long base = (unsigned long)image;
    for (unsigned int i = 0; i < reloc_count; i++){
        const unsigned char* e = reloc_entries + ((unsigned long)i * QOS_PROGRAM_RELOC_RELATIVE_ENTRY_BYTES);
        unsigned int target_off = get_u32_le_local(e);
        unsigned long long addend = get_u64_le_local(e + 4u);
        if ((target_off & 7u) != 0u ||
            target_off + 8u < target_off ||
            (unsigned long)target_off + 8UL > reservation_size ||
            addend >= reservation_size){
            return -1;
        }
        put_u64_le_local(image + target_off, (unsigned long long)(base + (unsigned long)addend));
    }
    return 0;
}

static void loader_assign_file_sandbox(loaded_program_t* prog, const char* file_83){
    if (!prog){
        return;
    }
    prog->file_sandbox_enabled = 0;
    for (unsigned int i = 0; i < 11u; i++){
        prog->file_sandbox_83[i] = ' ';
    }

    if (fat83_equal11(file_83, GAME_PROGRAM_83)){
        prog->file_sandbox_enabled = 1;
        for (unsigned int i = 0; i < 11u; i++){
            prog->file_sandbox_83[i] = GAME_SANDBOX_83[i];
        }
    }
}


int loader_is_busy(void){
    return loader_busy;
}

static unsigned long round_up_granule(unsigned long size){
    return (size + PROGRAM_ALLOC_GRANULE - 1UL) & ~(PROGRAM_ALLOC_GRANULE - 1UL);
}

void* alloc_program_memory(unsigned long size){
    if (size == 0 || size > PROGRAM_MAX_MEMORY){
        return 0;
    }

    unsigned long alloc_size = round_up_granule(size);
    if (alloc_size == 0 || alloc_size > PROGRAM_MAX_MEMORY){
        return 0;
    }

    unsigned int units_needed = (unsigned int)(alloc_size / PROGRAM_ALLOC_GRANULE);
    if (units_needed == 0u || units_needed > PROGRAM_UNIT_COUNT || units_needed > 255u){
        return 0;
    }

    unsigned long irq = spin_lock_irqsave(&g_program_alloc_lock);
    for (unsigned int i = 0; i + units_needed <= PROGRAM_UNIT_COUNT; i++){
        unsigned int ok = 1u;
        for (unsigned int j = 0; j < units_needed; j++){
            if (program_unit_used[i + j]){
                ok = 0u;
                i += j;
                break;
            }
        }
        if (!ok){
            continue;
        }

        for (unsigned int j = 0; j < units_needed; j++){
            program_unit_used[i + j] = 1u;
            program_unit_span[i + j] = 0u;
        }
        program_unit_span[i] = (unsigned char)units_needed;

        unsigned long base = PROGRAM_POOL_START + ((unsigned long)i * PROGRAM_ALLOC_GRANULE);
        void* addr = (void*)base;
        spin_unlock_irqrestore(&g_program_alloc_lock, irq);

        // Raw binaries produced via objcopy do not carry .bss contents.
        // Zero the whole reserved region so globals/statics and heap start at 0.
        volatile unsigned char* wipe = (volatile unsigned char*)base;
        for (unsigned long j = 0; j < alloc_size; j++){
            wipe[j] = 0;
        }

        return addr;
    }

    spin_unlock_irqrestore(&g_program_alloc_lock, irq);
    return 0;
}

void loader_free_program_memory(void* ptr, unsigned long size){
    (void)size;
    if (!ptr){
        return;
    }

    unsigned long addr = (unsigned long)ptr;
    if (addr < PROGRAM_POOL_START || addr >= (PROGRAM_POOL_START + PROGRAM_POOL_SIZE)){
        return;
    }

    unsigned long off = addr - PROGRAM_POOL_START;
    if ((off & (PROGRAM_ALLOC_GRANULE - 1UL)) != 0UL){
        return;
    }
    unsigned long unit = off / PROGRAM_ALLOC_GRANULE;
    if (unit >= PROGRAM_UNIT_COUNT){
        return;
    }

    unsigned long irq = spin_lock_irqsave(&g_program_alloc_lock);
    if (!program_unit_used[unit]){
        spin_unlock_irqrestore(&g_program_alloc_lock, irq);
        return;
    }

    unsigned int units = program_unit_span[unit];
    if (units == 0u){
        unsigned long rounded = round_up_granule(size);
        units = (rounded > 0UL) ? (unsigned int)(rounded / PROGRAM_ALLOC_GRANULE) : 1u;
    }
    if (units == 0u || unit + units > PROGRAM_UNIT_COUNT){
        spin_unlock_irqrestore(&g_program_alloc_lock, irq);
        return;
    }
    spin_unlock_irqrestore(&g_program_alloc_lock, irq);

    unsigned long alloc_size = (unsigned long)units * PROGRAM_ALLOC_GRANULE;
    unsigned long base = PROGRAM_POOL_START + unit * PROGRAM_ALLOC_GRANULE;
    volatile unsigned char* wipe = (volatile unsigned char*)base;
    for (unsigned long i = 0; i < alloc_size; i++){
        wipe[i] = 0;
    }

    // Re-lock the whole reservation to kernel-only/XN when process exits.
    mmu_map_kernel_private_region(base, alloc_size);
    irq = spin_lock_irqsave(&g_program_alloc_lock);
    for (unsigned int i = 0; i < units; i++){
        program_unit_used[unit + i] = 0u;
        program_unit_span[unit + i] = 0u;
    }
    spin_unlock_irqrestore(&g_program_alloc_lock, irq);
}

void* loader_user_stack_top(void* program_base,
                            unsigned long program_size,
                            unsigned long user_rw_offset,
                            unsigned long user_rw_size){
    if (!program_base){
        return 0;
    }
    unsigned long p = (unsigned long)program_base;
    if (p < PROGRAM_POOL_START || p >= (PROGRAM_POOL_START + PROGRAM_POOL_SIZE)){
        return 0;
    }
    unsigned long off = p - PROGRAM_POOL_START;
    if ((off & (PROGRAM_ALLOC_GRANULE - 1UL)) != 0UL){
        return 0;
    }
    unsigned long base = PROGRAM_POOL_START + off;

    if ((user_rw_offset & (PROGRAM_PAGE_SIZE - 1UL)) != 0UL){
        return 0;
    }
    if (program_size == 0UL ||
        program_size > PROGRAM_MAX_MEMORY ||
        (program_size & (PROGRAM_ALLOC_GRANULE - 1UL)) != 0UL){
        return 0;
    }
    if (user_rw_offset >= program_size || user_rw_size == 0UL){
        return 0;
    }
    if (user_rw_size > (program_size - user_rw_offset)){
        return 0;
    }
    if (user_rw_size <= (QOS_USER_STACK_BYTES + QOS_USER_GUARD_PAGE_BYTES + PROGRAM_PAGE_SIZE)){
        return 0;
    }

    unsigned long rw_end = base + user_rw_offset + user_rw_size;
    unsigned long stack_start = rw_end - QOS_USER_STACK_BYTES;
    unsigned long guard_start = stack_start - QOS_USER_GUARD_PAGE_BYTES;
    if (guard_start < (base + user_rw_offset)){
        return 0;
    }
    unsigned long top = rw_end & ~0xFUL;
    return (void*)(top - 16);
}

loaded_program_t load_program_from_sd_named(const char* fat_name_83)
{
    uart_puts("Loading program from SD...\n");
    loaded_program_t prog = {0};
    const char* file_83 = fat_name_83 ? fat_name_83 : DEFAULT_PROGRAM_83;
    loader_assign_file_sandbox(&prog, file_83);

    if (!loader_try_lock()){
        uart_puts("Loader busy.\n");
        return prog;
    }

    int size = -1;

    if (blockdev_reinit() != 0){
        loader_unlock();
        uart_puts("Storage reinit failed.\n");
        return prog;
    }

    if (fat32_init() == 0){
        size = fat32_read_file(file_83, buffer, PROGRAM_FILE_MAX);
        if (size <= 0){
            size = fat32_read_file(file_83, buffer, PROGRAM_FILE_MAX);
        }
    }

    if (size <= 0){
        // One-time resync path.
        blockdev_reinit();
        if (fat32_init() == 0){
            size = fat32_read_file(file_83, buffer, PROGRAM_FILE_MAX);
        }
    }

    if (size <= 0){
        loader_unlock();
        uart_puts("Load failed.\n");
        return prog;
    }

    uart_puts("File read OK. \n");

    if (size < (int)sizeof(program_header_t)){
        loader_unlock();
        uart_puts("Invalid program (too small)");
        return prog;
    }

    program_header_t *hdr = (program_header_t*)buffer;

    uart_puts("MAGIC raw: ");
    uart_puthex(buffer[0]);
    uart_puthex(buffer[1]);
    uart_puthex(buffer[2]);
    uart_puthex(buffer[3]);
    uart_puts("\n");

    if (hdr->magic != QOS_MAGIC){
        loader_unlock();
        uart_puts("Bad magic.\n");
        return prog;
    }

    uart_puts("Valid QOS magic!\n");

    unsigned int code_size = hdr->size;
    unsigned int entry_offset = hdr->entry_offset;
    unsigned int code_off = (unsigned int)sizeof(program_header_t);
    const program_sec_header_t* sec = 0;

    if (code_size > PROGRAM_MAX){
        loader_unlock();
        uart_puts("Program too large.\n");
        return prog;
    }

    if ((unsigned int)size < code_off || code_size > ((unsigned int)size - code_off)){
        loader_unlock();
        uart_puts("Program truncated.\n");
        return prog;
    }

    // Security extension is mandatory: signatures are required for all programs.
    if ((unsigned int)size < (unsigned int)(sizeof(program_header_t) + sizeof(program_sec_header_t))){
        loader_unlock();
        uart_puts("Program missing security header.\n");
        return prog;
    }
    unsigned int user_rw_offset = 0;
    unsigned int user_rw_size = 0;
    unsigned int reloc_count = 0;
    const unsigned char* reloc_entries = 0;
    {
        const unsigned int sec_min = (unsigned int)sizeof(program_sec_header_t);
        const program_sec_header_t* cand = (const program_sec_header_t*)(buffer + sizeof(program_header_t));
        if (cand->magic != QOS_SEC_MAGIC){
            loader_unlock();
            uart_puts("Program missing SEC1 header.\n");
            return prog;
        }
        if (cand->header_size < (sec_min + PROGRAM_SEC_LAYOUT_V1_BYTES) ||
            cand->header_size > QOS_PROGRAM_SEC_MAX_HEADER_BYTES){
            loader_unlock();
            uart_puts("Bad security header size.\n");
            return prog;
        }
        if ((cand->flags & ~(QOS_PROG_FLAG_SHA256 |
                             QOS_PROG_FLAG_MEM_LAYOUT_V1 |
                             QOS_PROG_FLAG_RELOC_RELATIVE_V1)) != 0u){
            loader_unlock();
            uart_puts("Program has unsupported security flags.\n");
            return prog;
        }
        if (code_off > ((unsigned int)size - cand->header_size)){
            loader_unlock();
            uart_puts("Program/security header overflow.\n");
            return prog;
        }
        code_off += cand->header_size;
        if ((unsigned int)size < code_off || code_size > ((unsigned int)size - code_off)){
            loader_unlock();
            uart_puts("Program/security header size mismatch.\n");
            return prog;
        }

        if ((cand->flags & QOS_PROG_FLAG_MEM_LAYOUT_V1) == 0u){
            loader_unlock();
            uart_puts("Program missing MEM_LAYOUT_V1 flag.\n");
            return prog;
        }
        {
            const unsigned char* ext = (const unsigned char*)cand + sec_min;
            unsigned int ext_used = PROGRAM_SEC_LAYOUT_V1_BYTES;
            user_rw_offset = (unsigned int)ext[0] |
                             ((unsigned int)ext[1] << 8) |
                             ((unsigned int)ext[2] << 16) |
                             ((unsigned int)ext[3] << 24);
            user_rw_size = (unsigned int)ext[4] |
                           ((unsigned int)ext[5] << 8) |
                           ((unsigned int)ext[6] << 16) |
                           ((unsigned int)ext[7] << 24);
            if (user_rw_offset >= PROGRAM_MAX_MEMORY || user_rw_size == 0u){
                loader_unlock();
                uart_puts("Program memory layout invalid.\n");
                return prog;
            }
            if ((user_rw_offset & (PROGRAM_PAGE_SIZE - 1UL)) != 0u){
                loader_unlock();
                uart_puts("Program RW offset must be page-aligned.\n");
                return prog;
            }
            if (user_rw_size > (PROGRAM_MAX_MEMORY - user_rw_offset)){
                loader_unlock();
                uart_puts("Program RW size beyond slot bounds.\n");
                return prog;
            }
            unsigned long requested_size = (unsigned long)user_rw_offset + (unsigned long)user_rw_size;
            if (requested_size > PROGRAM_MAX_MEMORY ||
                requested_size < (unsigned long)user_rw_offset ||
                (requested_size & (PROGRAM_ALLOC_GRANULE - 1UL)) != 0UL){
                loader_unlock();
                uart_puts("Program memory reservation invalid.\n");
                return prog;
            }
            if ((cand->flags & QOS_PROG_FLAG_RELOC_RELATIVE_V1) != 0u){
                const unsigned int reloc_hdr_bytes = (unsigned int)sizeof(program_sec_reloc_v1_t);
                const unsigned int reloc_header_off = sec_min + ext_used;
                if (cand->header_size < reloc_header_off + reloc_hdr_bytes){
                    loader_unlock();
                    uart_puts("Program relocation header truncated.\n");
                    return prog;
                }
                const unsigned char* reloc = (const unsigned char*)cand + reloc_header_off;
                reloc_count = get_u32_le_local(reloc);
                unsigned int reloc_entry_size = get_u32_le_local(reloc + 4u);
                if (reloc_entry_size != QOS_PROGRAM_RELOC_RELATIVE_ENTRY_BYTES ||
                    reloc_count > QOS_PROGRAM_MAX_RELOCS){
                    loader_unlock();
                    uart_puts("Program relocation table invalid.\n");
                    return prog;
                }
                if (reloc_count > ((QOS_PROGRAM_SEC_MAX_HEADER_BYTES - reloc_header_off - reloc_hdr_bytes) /
                                   QOS_PROGRAM_RELOC_RELATIVE_ENTRY_BYTES)){
                    loader_unlock();
                    uart_puts("Program relocation count invalid.\n");
                    return prog;
                }
                unsigned int reloc_bytes = reloc_hdr_bytes +
                    (reloc_count * QOS_PROGRAM_RELOC_RELATIVE_ENTRY_BYTES);
                if (cand->header_size != reloc_header_off + reloc_bytes){
                    loader_unlock();
                    uart_puts("Program relocation header size mismatch.\n");
                    return prog;
                }
                reloc_entries = reloc + reloc_hdr_bytes;
                for (unsigned int r = 0; r < reloc_count; r++){
                    const unsigned char* e = reloc_entries +
                        ((unsigned long)r * QOS_PROGRAM_RELOC_RELATIVE_ENTRY_BYTES);
                    unsigned int target_off = get_u32_le_local(e);
                    unsigned long long addend = get_u64_le_local(e + 4u);
                    if ((target_off & 7u) != 0u ||
                        target_off + 8u < target_off ||
                        (unsigned long)target_off + 8UL > requested_size ||
                        addend >= requested_size){
                        loader_unlock();
                        uart_puts("Program relocation entry invalid.\n");
                        return prog;
                    }
                }
            } else if (cand->header_size != sec_min + ext_used){
                loader_unlock();
                uart_puts("Program has unsigned security extension bytes.\n");
                return prog;
            }
        }
        sec = cand;
    }

    if (entry_offset >= code_size){
        loader_unlock();
        uart_puts("Bad entry offset.\n");
        return prog;
    }
    if (entry_offset >= user_rw_offset){
        loader_unlock();
        uart_puts("Bad layout: entry in writable region.\n");
        return prog;
    }

    unsigned char *src = buffer + code_off;
    uart_puts("Program trust verification start.\n");
    if (trust_verify_program_image(file_83, sec, src, code_size) != 0){
        loader_unlock();
        uart_puts("Program trust verification failed.\n");
        return prog;
    }
    uart_puts("Program trust verification OK.\n");

    unsigned long program_reservation_size = (unsigned long)user_rw_offset + (unsigned long)user_rw_size;
    void* dst = alloc_program_memory(program_reservation_size);
    unsigned char* d = (unsigned char*)dst;
    if (!dst){
        loader_unlock();
        uart_puts("No memory for program!\n");
        return prog;
    }

//    for (unsigned int i = 0; i < code_size; i++){
//        d[i] = prog;
//    }

    for (unsigned int i = 0; i < code_size; i++){
        d[i] = src[i];
    }

    if (loader_apply_relative_relocs(d,
                                     program_reservation_size,
                                     reloc_entries,
                                     reloc_count) != 0){
        loader_free_program_memory(dst, program_reservation_size);
        loader_unlock();
        uart_puts("Program relocation apply failed.\n");
        return prog;
    }

    // W^X policy: code is copied while this slot is kernel-private RW+XN.
    // EL0 execute permissions are granted later when process tables map
    // [0, user_rw_offset) as RX and data/stack pages as RW+NX.
    clean_data_cache();
    invalidate_instruction_cache();

    prog.entry = (program_entry_t)((unsigned long)dst + entry_offset);
    prog.memory = dst;
    prog.size = program_reservation_size;
    prog.user_rw_offset = user_rw_offset;
    prog.user_rw_size = user_rw_size;
    prog.heap_allocated = 0;
    prog.signer_key_id = sec->signer_key_id;
    {
        const trust_key_t* key = trust_find_key(sec->signer_key_id);
        if (!key){
            loader_free_program_memory(dst, program_reservation_size);
            loader_unlock();
            uart_puts("Program signer key missing post-verify.\n");
            return (loaded_program_t){0};
        }
        prog.signer_role_mask = key->role_mask;
        prog.signer_scope_mask = key->scope_mask;
    }
    uart_puts("Program loaded at: ");
    uart_puthex((unsigned long)dst);
    uart_puts("\n");
    uart_puts("Entry at: ");
    uart_puthex((unsigned long)prog.entry);
    uart_puts("\n");
    loader_unlock();
    return prog;
}

loaded_program_t load_program_from_sd(void){
    return load_program_from_sd_named(DEFAULT_PROGRAM_83);
}

void execute_program(unsigned long entry_addr){
    uart_puts("EXEC: jumping ...\n");

    asm volatile ("dsb sy");
    asm volatile ("isb");

    void (*entry_fn)(void) = (void(*)(void))entry_addr;
    entry_fn();

    uart_puts("Program returned to kernel.\n");
}

