#include "uart.h"
#include "shell.h"
#include "memory.h"
#include "task.h"
#include "framebuffer.h"


//extern void vectors(void);


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
    fb_clear(0x00FF0000); // RED SCREEN

    while(1){
        char c = uart_getc();
        uart_send(c);
    }

}
