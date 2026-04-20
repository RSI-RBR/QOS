#include "uart.h"
#include "shell.h"
#include "memory.h"
#include "string.h"
#include "api.h"
#include "context.h"
#include "programs.h"
#include "process.h"


static kernel_api_t kapi = {
    .puts = uart_puts,
    .putc = uart_send
};

typedef void (*program_entry_t)(kernel_api_t *api);
extern void run_program(void *entry, void *stack, kernel_api_t *api);

#define MAX_ARGS 8
#define MAX_INPUT 128
#define BUF_SIZE 256

#define MAX_PTRS 16

static void *ptrs[MAX_PTRS];

static char buffer[BUF_SIZE];
static int buf_index = 0;

//extern kernel_api_t kapi;

#define USER_PROGRAM_ADDR 0x400000

static void cmd_runbin(int argc, char **argv){
    extern unsigned char _binary_build_hello_bin_start[];
    extern unsigned char _binary_build_hello_bin_end[];

    unsigned char *src = _binary_build_hello_bin_start;
    unsigned char *dst = (unsigned char*)USER_PROGRAM_ADDR;

    while (src < _binary_build_hello_bin_end){
        *dst++ = *src++;
    }

    uart_puts("Running program...\n");

    void (*prog)(kernel_api_t *) = (void*)USER_PROGRAM_ADDR;

    void *stack = alloc_stack();

    run_program(prog, stack, &kapi);

    uart_puts("Program returned!\n");

    free_stack(stack);
}

static void shell_print_prompt(){
    uart_puts("\n*QOS* > ");
}

static void cmd_run(int argc, char **argv){
    if (argc < 2){
        uart_puts("Usage: run <program>\n");
        return;
    }

    for (int i = 0; i < program_count; i++){
        if (kstrcmp(argv[1], programs[i].name) == 0){

            void *stack = alloc_stack();

            if (!stack){
                uart_puts("No free stacks!\n");
                return;
            }

            uart_puts("Executing program...\n");

            run_program(programs[i].entry, stack, &kapi);

            uart_puts("Program returned!\n");

            free_stack(stack);

            return;
        }
    }

    uart_puts("Program not found\n");
}

static void cmd_lsprog(int argc, char **argv){
    uart_puts("Programs: \n");

    for (int i = 0; i < program_count; i++){
        uart_puts(" - ");
        uart_puts(programs[i].name);
        uart_puts("\n");
    }
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
    uart_puts(" alloc \n");
    uart_puts(" free \n");
    uart_puts(" memlist \n");
    uart_puts(" run <program> \n");
    uart_puts(" lsprog \n");
    uart_puts(" runbin\n");

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

static void cmd_alloc(int argc, char **argv){
    if (argc < 2){
        uart_puts("Usage: alloc <size> \n");
        return;
    }

    int size = 0;
    char *s = argv[1];

    while (*s){
        size = size * 10 + (*s - '0');
        s++;
    }

    void *p = kmalloc(size);

    if (!p){
        uart_puts("Allocation failed! \n");
        return;
    }

    for (int i = 0; i < MAX_PTRS; i++){
        if (!ptrs[i]){
            ptrs[i] = p;
            uart_puts("Allocated at index ");
            uart_send('0' + i);
            uart_puts("\n");
            return;
        }
    }

    uart_puts("Pointer table full! \n");

    return;
}

static void cmd_free(int argc, char **argv){
    if (argc < 2){
        uart_puts("Usage: free <index> \n");
        return;
    }

    int idx = argv[1][0] - '0';

    if (idx < 0 || idx >= MAX_PTRS || !ptrs[idx]){
        uart_puts("Invalid index \n");
        return;
    }

    kfree(ptrs[idx]);
    ptrs[idx] = 0;

    uart_puts("Freed \n");

    return;
}

static void cmd_memlist(int argc, char **argv){
    for (int i = 0; i < MAX_PTRS; i++){
        if (ptrs[i]){
            uart_puts("Index ");
            uart_send('0' + i);
            uart_puts(" allocated \n");
        }
    }
}

typedef struct {
    const char *name;
    void (*func)(int argc, char **argv);
} command_t;

static command_t commands[] = {
    {"help", cmd_help},
    {"echo", cmd_echo},
    {"alloc", cmd_alloc},
    {"free", cmd_free},
    {"memlist", cmd_memlist},
    {"run", cmd_run},
    {"lsprog", cmd_lsprog},
    {"runbin", cmd_runbin}
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
