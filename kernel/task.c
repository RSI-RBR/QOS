#include "task.h"
#include "memory.h"
#include "context.h"
#include "uart.h"

#define STACK_SIZE 4096

static task_t tasks[MAX_TASKS];

void task_init(){
    for (int i = 0; i < MAX_TASKS; i++){
        tasks[i].state = TASK_UNUSED;
    }
}

int task_create(void (*entry)(void *), void *arg){
    for (int i = 0; i < MAX_TASKS; i++){
        if (tasks[i].state == TASK_UNUSED){

            void *stack = kmalloc(STACK_SIZE);
            if (!stack) return -1;

            tasks[i].entry = entry;
            tasks[i].arg = arg;
            tasks[i].stack = stack + STACK_SIZE;
            tasks[i].state = TASK_READY;

            return i;
        }
    }
    return -1;
}

void task_run_all(){
    for (int i = 0; i < MAX_TASKS; i++){
        if (tasks[i].state == TASK_READY){

            tasks[i].state = TASK_RUNNING;

            run_program(tasks[i].entry, tasks[i].stack, tasks[i].arg);

            tasks[i].state = TASK_DONE;
        }
    }
}
