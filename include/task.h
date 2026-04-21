#ifndef TASK_H
#define TASK_H

#define MAX_TASKS 8

typedef enum {
    TASK_UNUSED,
    TASK_READY,
    TASK_RUNNING,
    TASK_DONE
} task_state_t;

typedef struct {
    void (*entry)(void *);
    void *arg;
    void *stack;
    task_state_t state;
} task_t;

void task_init();
int task_create(void (*entry)(void *), void *arg);
void task_run_all();

#endif
