#include "loader.h"
#include "blockdev.h"
#include "mmu.h"


#define PROGRAM_MAX (8 * 1024)
#define PROGRAM_POOL_START 0x02000000UL
#define PROGRAM_POOL_SIZE  (16UL * 1024UL * 1024UL)
#define PROGRAM_SLOT_SIZE  (2UL * 1024UL * 1024UL)
#define PROGRAM_SLOT_COUNT (PROGRAM_POOL_SIZE / PROGRAM_SLOT_SIZE)

static unsigned char program_slot_used[PROGRAM_SLOT_COUNT];


static unsigned char buffer[PROGRAM_MAX];
static volatile int loader_busy = 0;

void loader_mmu_init_pool(void){
    // Keep pool inaccessible to EL0 until a program block is explicitly mapped.
    mmu_map_kernel_private_region(PROGRAM_POOL_START, PROGRAM_POOL_SIZE);
    for (unsigned int i = 0; i < PROGRAM_SLOT_COUNT; i++){
        program_slot_used[i] = 0;
    }
}

static int loader_try_lock(void){
    int taken;
    asm volatile("msr daifset, #2");
    taken = loader_busy;
    if (!taken){
        loader_busy = 1;
    }
    asm volatile("msr daifclr, #2");
    return !taken;
}

static void loader_unlock(void){
    asm volatile("msr daifset, #2");
    loader_busy = 0;
    asm volatile("msr daifclr, #2");
}


int loader_is_busy(void){
    return loader_busy;
}

void* alloc_program_memory(unsigned int size){
    size = (size+15) & ~15;

    if (size == 0 || size > PROGRAM_SLOT_SIZE){
        return 0;
    }

    for (unsigned int i = 0; i < PROGRAM_SLOT_COUNT; i++){
        if (program_slot_used[i]){
            continue;
        }
        program_slot_used[i] = 1;

        unsigned long base = PROGRAM_POOL_START + ((unsigned long)i * PROGRAM_SLOT_SIZE);
        void* addr = (void*)base;

        // Entire slot is user-executable to keep block-level isolation simple.
        mmu_map_user_code_region(base, PROGRAM_SLOT_SIZE);

        return addr;
    }

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
    unsigned long slot = off / PROGRAM_SLOT_SIZE;
    if (slot >= PROGRAM_SLOT_COUNT){
        return;
    }

    unsigned long slot_base = PROGRAM_POOL_START + slot * PROGRAM_SLOT_SIZE;
    volatile unsigned char* wipe = (volatile unsigned char*)slot_base;
    for (unsigned long i = 0; i < PROGRAM_SLOT_SIZE; i++){
        wipe[i] = 0;
    }

    // Re-lock slot to kernel-only/XN when process exits.
    mmu_map_kernel_private_region(slot_base, PROGRAM_SLOT_SIZE);
    program_slot_used[slot] = 0;
}

void* loader_user_stack_top(void* program_base){
    if (!program_base){
        return 0;
    }
    unsigned long p = (unsigned long)program_base;
    if (p < PROGRAM_POOL_START || p >= (PROGRAM_POOL_START + PROGRAM_POOL_SIZE)){
        return 0;
    }
    unsigned long off = p - PROGRAM_POOL_START;
    unsigned long slot = off / PROGRAM_SLOT_SIZE;
    unsigned long slot_base = PROGRAM_POOL_START + slot * PROGRAM_SLOT_SIZE;
    unsigned long top = (slot_base + PROGRAM_SLOT_SIZE) & ~0xFUL;
    return (void*)(top - 16);
}

loaded_program_t load_program_from_sd(void)
{
    uart_puts("Loading program from SD...\n");
    loaded_program_t prog = {0};

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
        size = fat32_read_file("PROGRAM BIN", buffer, PROGRAM_MAX);
        if (size <= 0){
            size = fat32_read_file("PROGRAM BIN", buffer, PROGRAM_MAX);
        }
    }

    if (size <= 0){
        // One-time resync path.
        blockdev_reinit();
        if (fat32_init() == 0){
            size = fat32_read_file("PROGRAM BIN", buffer, PROGRAM_MAX);
        }
    }

    loader_unlock();

    if (size <= 0){
        uart_puts("Load failed.\n");
        return prog;
    }

    uart_puts("File read OK. \n");

    if (size < sizeof(program_header_t)){
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
        uart_puts("Bad magic.\n");
        return prog;
    }

    uart_puts("Valid QOS magic!\n");

    unsigned int code_size = hdr->size;
    unsigned int entry_offset = hdr->entry_offset;

    if (code_size > PROGRAM_MAX){
        uart_puts("Program too large.\n");
        return prog;
    }

    unsigned char *src = buffer + sizeof(program_header_t);
//    unsigned char *dst = (unsigned char*)PROGRAM_ADDR;
//    unsigned char* dst = (unsigned char*)alloc_program_memory(code_size);
    void* dst = alloc_program_memory(code_size);
    unsigned char* d = (unsigned char*)dst;
    if (!dst){
        uart_puts("No memory for program!\n");
        return prog;
    }

//    for (unsigned int i = 0; i < code_size; i++){
//        d[i] = prog;
//    }

    for (unsigned int i = 0; i < code_size; i++){
        d[i] = src[i];
    }

    clean_data_cache();
    invalidate_instruction_cache();

    prog.entry = (program_entry_t)((unsigned long)dst + entry_offset);
    prog.memory = dst;
    prog.size = code_size;
    prog.heap_allocated = 0;
    uart_puts("Program loaded at: ");
    uart_puthex((unsigned long)dst);
    uart_puts("\n");
    uart_puts("Entry at: ");
    uart_puthex((unsigned long)prog.entry);
    uart_puts("\n");
    return prog;
}

void execute_program(unsigned long entry_addr){
    uart_puts("EXEC: jumping ...\n");

    asm volatile ("dsb sy");
    asm volatile ("isb");

    void (*entry_fn)(void) = (void(*)(void))entry_addr;
    entry_fn();

    uart_puts("Program returned to kernel.\n");
}

