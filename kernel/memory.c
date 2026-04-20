#include "memory.h"

#define HEAP_SIZE 1024*1024 // 1MB heap

static unsigned char heap[HEAP_SIZE];
static unsigned long heap_top = 0;

typedef struct {
    void *ptr;
    unsigned long size;
    int used;
} alloc_t;

#define MAX_ALLOCS 32
static alloc_t allocs[MAX_ALLOCS];

void memory_init(void){
    heap_top = 0;

    for (int i = 0; i < MAX_ALLOCS; i++){
        allocs[i].used = 0;
    }
}

void *kmalloc(unsigned int size){
    if (heap_top + size >= HEAP_SIZE){
        return 0;
    }

    void *ptr = &heap[heap_top];
    heap_top += size;

    for (int i = 0; i < MAX_ALLOCS; i++){
        if (!allocs[i].used){
            allocs[i].ptr = ptr;
            allocs[i].size = size;
            allocs[i].used = 1;
            break;
        }
    }

    return ptr;

}

void kfree(void *ptr){
    for (int i = 0; i < MAX_ALLOCS; i++){
        if (allocs[i].used && allocs[i].ptr == ptr){
            allocs[i].used = 0;
            return;
        }
    }
}
