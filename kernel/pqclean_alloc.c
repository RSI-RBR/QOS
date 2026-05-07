#include <stddef.h>
#include "memory.h"

void *malloc(size_t size){
    return kmalloc((unsigned long)size);
}

void free(void *ptr){
    kfree(ptr);
}
