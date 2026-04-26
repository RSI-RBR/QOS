#ifndef PROCESS_H
#define PROCESS_H

#define MAX_PROCESSES 8
#define STACK_SIZE 4096

typedef struct{
    void *entry;
    void *stack;
    int active;
}

void *alloc_stack(void);
void free_stack(void *stack);

#endif
