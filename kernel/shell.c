#include "uart.h"
#include "shell.h"
#include "memory.h"
#include "string.h"


#define MAX_ARGS 8
#define MAX_INPUT 128
#define BUF_SIZE 256

static char buffer[BUF_SIZE];
static int buf_index = 0;

static void shell_print_prompt(){
    uart_puts("\n> ");
}

static void shell_clear_buffer(){
    for (int i = 0; i < BUF_SIZE; i++) buffer[i] = 0;
    buf_index = 0;
}

static int parse_args(char* input, char *argv[]){
    int argc = 0;

    while (*input && argc < MAX_ARGS){
        while (*input == ' ') input ++;

        if (*input == 0) break;

        argv[argc++] = input;

        while (*input && *input != ' ') input ++;

        if (*input) {
            *input = 0;
            input ++;
        }
    }

    return argc;
}

static void cmd_help(int argc, char **argv){
    uart_puts("Commands: \n");
    uart_puts(" echo <text> \n");
    uart_puts(" help \n");
    uart_puts(" mem \n");

    return;
}

static void cmd_echo(int argc, char **argv){
    if (argc < 2){
        uart_puts("Usage: echo <text>\n");
        return;
    }

    for (int i = 1; i < argc; i++){
        uart_puts(argv[i]);
        uart_puts(" ");
    }
    uart_puts("\n");

    return;
}

static void cmd_mem(int argc, char **argv){
    void *p = kmalloc(64);

    if (!p) uart_puts("Out of memory! \n");
    else uart_puts("Allocated 64 bytes \n");

    return;
}

typedef struct {
    const char *name;
    void (*func)(int argc, char **argv);
} command_t;

static command_t commands[] = {
    {"help", cmd_help},
    {"echo", cmd_echo},
    {"mem", cmd_mem}
};

static void shell_execute(char *input){
    char *argv[MAX_ARGS];
    int argc = parse_args(input, argv);

    if (argc == 0) return;

    for (unsigned int i = 0; i < sizeof(commands)/sizeof(commands[0]); i++){
        if (kstrcmp(argv[0], commands[i].name) == 0){
            commands[i].func(argc, argv);
            return;
        }
    }
    uart_puts("Unknown command! \n");

    return;
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
