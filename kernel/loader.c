#include "loader.h"


#define PROGRAM_MAX (8 * 1024)
//#define PROGRAM_ADDR ((unsigned long)0x40000)
#define PROGRAM_POOL_START 0x40000
#define PROGRAM_POOL_SIZE (1024*1024)

static unsigned long program_next = PROGRAM_POOL_START;


static unsigned char buffer[PROGRAM_MAX];


void* alloc_program_memory(unsigned int size){
    size = (size+15) & ~15;

    if (program_next + size > PROGRAM_POOL_START + PROGRAM_POOL_SIZE){ return 0; }

    void* addr = (void*)program_next;
    program_next += size;
    return addr;
}

loaded_program_t load_program_from_sd(void)
{
    uart_puts("Loading program from SD...\n");
    loaded_program_t prog = {0};
    if (fat32_init()){
        uart_puts("FAT init failed.\n");
        return prog;
    }

    int size = fat32_read_file("PROGRAM BIN", buffer, PROGRAM_MAX);

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
    void* dst = kmalloc(code_size);
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
    uart_puts("Program loaded at: ");
    uart_puthex((unsigned long)dst);
    uart_puts("\n");
    uart_puts("Entry at: ");
    uart_puthex((unsigned long)prog.entry);
    uart_puts("\n");
    return prog;
}

void execute_program(unsigned long entry_addr){
    extern kernel_api_t kapi;

    uart_puts("EXEC: jumping ...\n");

    asm volatile ("dsb sy");
    asm volatile ("isb");

    void (*entry_fn)(kernel_api_t*) = (void(*)(kernel_api_t*))entry_addr;

    entry_fn(&kapi);

    uart_puts("Program returned to kernel.\n");
}

