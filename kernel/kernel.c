#include "uart.h"
#include "shell.h"
#include "memory.h"
#include "task.h"
#include "framebuffer.h"
#include "context.h"
#include "loader.h"
#include "process.h"
#include "fat32.h"
//#include "sd.h"
//#include "gpio.h"
//#include "clock.h"
//#include "mailbox.h"


extern kernel_api_t kapi;

unsigned char sector[512];

//void cm_probe(void)
//{
//    volatile unsigned int *ctl = (unsigned int*)0x3F10101C;
//
//    uart_puts("CM BEFORE=");
//    uart_puthex(*ctl);
//    uart_puts("\n");
//
//    *ctl = 0xFFFFFFFF;
//
//    uart_puts("CM AFTER=");
//    uart_puthex(*ctl);
//    uart_puts("\n");
//}

//void mmio_test(void)
//{
//    volatile unsigned int *addr = (volatile unsigned int*)0x3F200000; // GPIO base
//
//    uart_puts("GPIO BEFORE=");
//    uart_puthex(*addr);
//    uart_puts("\n");
//
//    *addr = 0xAAAAAAAA;
//
//    uart_puts("GPIO AFTER=");
//    uart_puthex(*addr);
//    uart_puts("\n");
//}
//
//void test_sd(void){
////    uart_puts("sd_gpio_init: \n");
////    sd_gpio_init();
//    uart_puts("Testing SD read... (step 1) \n");
//
//    if (sd_init() != 0){
//        uart_puts("SD init failed.\n");
//        return;
//    }
//    uart_puts("SD init OK... (Step 2) \n");
//    if (sd_read_block(0, sector) != 0){
//        uart_puts("read fail\n");
//        return;
//    }
//
//    uart_puts("Read success (step 3) First bytes: \n");
//    for (int i = 0; i < 16; i++){
//        unsigned char b = sector[i];
//        uart_puthex(b);
//    } uart_send('\n');
//
//}

void memzero(unsigned long start, unsigned long size){
    for (unsigned long i = 0; i < size; i++)
        ((char*)start)[i] = 0;
}

void test_task(void *arg){
    char id = (char)(unsigned long)arg;

    uart_puts("Task ");
    uart_send(id);
    uart_puts(" running\n");
}

void kernel_main(void){
    uart_init();
    uart_puts("Uart initialized!\n");

    memory_init();
    uart_puts("Memory initialized!\n");

    task_init();
    uart_puts("Task system initialized!\n");

    fb_init();
    uart_puts("Frame buffer initialized!\n");

    kapi.clear(0x00000000);
//    kapi.draw_rect(100, 100, 500, 300, 0x00FFFFFF);

    uart_puts("Kernel booted successfully!\n");

    // -----------------------------
    // OPTION 1: RUN SHELL (RECOMMENDED)
    // -----------------------------

//    program_entry_t prog;
//    prog = load_program_from_sd();

//    if (prog){
//        void *stack = alloc_stack();
//        if (!stack){
//            uart_puts("No stack available!\n");
//            return;
//        }
//        run_program(prog, stack, &kapi);
//        free_stack(stack);
//    }

    uart_puts("\n--- START SD PIPELINE ---\n");
//    extern void sdhost_reset(void);
//    extern int sdhost_cmd(unsigned int cmd, unsigned int arg);
//    extern int sdhost_init_card(void);
//    extern int sdhost_read_block(unsigned int lba, unsigned char* buffer);
//    gpio_init_sd();
//    sdhost_reset();
//    if (sdhost_init_card() != 0){
//        uart_puts("SD INIT FAILED!\n");
//        return;
//    }

//    uart_puts("INIT OK, attempting read...\n");

//    if (sdhost_read_block(2048, sector) != 0){
//        uart_puts("READ FAILED!\n");
//        return;
//    }

//    uart_puts("Read success. First 16 bytes: \n");

//    for (int i = 0; i < 16; i++){
//        uart_puthex(sector[i]);
//    } uart_puts("\n");

    unsigned char prog[8192];

    if (fat32_init() != 0){
        uart_puts("FAT init failed!\n");
        return;
    }
    uart_puts("FAT INITIALIZED!\n");
    int size = fat32_read_file("PROGRAM BIN", prog, sizeof(prog));

    if (size < 0){
        uart_puts("File read failed.\n");
        return;
    }

    uart_puts("PROGRAM.BIN loaded, size = ");
    uart_puthex(size);
    uart_puts("\n");

    uart_puts("--- END SD PIPELINE ---\n");
    
    shell_init();
    shell_run();

    // -----------------------------
    // OPTION 2: TASK DEMO (COMMENTED)
    // -----------------------------
    /*
    task_create(test_task, (void*)'A');
    task_create(test_task, (void*)'B');
    task_run_all();
    */

    // never reach here normally
    while (1){
        char c = uart_getc();
        uart_send(c);
    }
}
