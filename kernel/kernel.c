#include "uart.h"
#include "shell.h"
#include "memory.h"
#include "task.h"
#include "framebuffer.h"
#include "context.h"
#include "loader.h"
#include "process.h"


//extern void vectors(void);

extern kernel_api_t kapi;

void memzero(unsigned long start, unsigned long size){
    for (unsigned long i = 0; i < size; i++)
        ((char*)start)[i] = 0;
}

void test_task(void *arg){
    char id = (char)(unsigned long)arg;
    uart_puts("Task ");
    uart_send(id);
    uart_puts(" running\n");
    return;
}

void kernel_main(void){
//    asm volatile("msr VBAR_EL1, %0" :: "r"(&vectors));
    
    uart_init();
    uart_puts("Uart initialized!\n");
    memory_init();
    uart_puts("Memory initialized!\n");

    task_init();

    
    uart_puts("Kernel booted successfully! \n");

    //shell_init();
    //shell_run();

//    task_create(test_task, (void*)'A');
//    task_create(test_task, (void*)'B');
//    task_run_all();

    fb_init();
    uart_puts("Frame buffer initialized!\n");

    void (*prog)(kernel_api_t *);
    prog = load_program();

    void *stack = alloc_stack();

    run_program(prog, stack, &kapi);

    uart_puts("Program returned!\n");

//    if (!fb){
//        uart_puts("FB null!\n");
//    }else{
//        uart_puts("FB OK!\n");
//    }
//    fb_clear(0x00000000); // RED SCREEN

//    fb_draw_rect(100, 100, 200, 150, 0x00FF0000);
//    fb_draw_rect(400, 200, 100, 100, 0x0000FF00);
//    fb_draw_rect(600, 300, 150, 200, 0x000000FF);

    while(1){
        char c = uart_getc();
        uart_send(c);
    }

}
