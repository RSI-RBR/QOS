#include "uart.h"
#include "shell.h"
#include "memory.h"

#define BUF_SIZE 128

static char buffer[BUF_SIZE];
static int buf_index = 0;

static void shell_print_prompt(){
    uart_puts("\n> ");
}

static void shell_clear_buffer(){
    for (int i = 0; i < BUF_SIZE; i++) buffer[i] = 0;
    buf_index = 0;
}

static void shell_execute(char *cmd){
    if (cmd[0] == 0) return;

    // echo command
    if (cmd[0] == 'e' && cmd[1] == 'c' && cmd[2] == 'h' && cmd[3] == 'o'){
        uart_puts(cmd+5);
        return;
    }

    if (cmd[0] == 'h' && cmd[1] == 'e' && cmd[2] == 'l' && cmd[3] == 'p'){
        uart_puts("Commands: \n");
        uart_puts(" echo <text>\n");
        uart_puts(" help\n");
        return;
    }

    if (cmd[0] == 'm' && cmd[1] == 'e' && cmd[2] == 'm'){
        void *p = kmalloc(64);

        if (p == 0){
            uart_puts("Out of memory!\n");
        }else{
            uart_puts("Allocated 64 bytes");
        }

        return;
    }

    uart_puts("Unknown command!");

}

void shell_init(void){
    shell_clear_buffer();
    uart_puts("Simple shell ready");
    shell_print_prompt();
}

void shell_run(void){
    while (1){
        char c = uart_getc();

        if (c == '\r' || c == '\n'){
            uart_puts("\n");
            buffer[buf_index] = 0;
            shell_execute(buffer);
            shell_clear_buffer();
            shell_print_prompt();
            continue;
        }

        if (c == 127 || c == '\b'){
            if (buf_index > 0){
                buf_index--;
                buffer[buf_index] = 0;
                uart_puts("\b \b");
            }
            continue;
        }

        if (buf_index < BUF_SIZE - 1){
            buffer[buf_index++] = c;
            uart_send(c);

        }

    }

}
