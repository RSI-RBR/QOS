#include "memory.h"

#define HEAP_SIZE 1024*1024 // 1MB heap

static unsigned char heap[HEAP_SIZE];
static unsigned int heap_index = 0;

void memory_init(void){
    heap_index = 0;
}

void *kmalloc(unsigned int size){
    if (heap_index + size >= HEAP_SIZE){
        return 0;
    }

    void *ptr = &heap[heap_index];
    heap_index += size;

    return ptr;

}
