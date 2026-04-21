#include "memory.h"
#include "uart.h"

#define HEAP_SIZE (16 * 1024 * 1024) // 16MB for now (expand later)

static unsigned char heap[HEAP_SIZE];

typedef struct block {
    unsigned long size;
    int free;
    struct block *next;
} block_t;

static block_t *free_list = 0;

#define ALIGN16(x) (((x) + 15) & ~15)

void memory_init(void){
    free_list = (block_t*)heap;

    free_list->size = HEAP_SIZE - sizeof(block_t);
    free_list->free = 1;
    free_list->next = 0;

    uart_puts("Heap initialized\n");
}

static void split_block(block_t *block, unsigned long size){
    block_t *new_block = (block_t*)((unsigned char*)block + sizeof(block_t) + size);

    new_block->size = block->size - size - sizeof(block_t);
    new_block->free = 1;
    new_block->next = block->next;

    block->size = size;
    block->next = new_block;
}

void *kmalloc(unsigned long size){
    if (size == 0) return 0;

    size = ALIGN16(size);

    block_t *curr = free_list;

    while (curr){
        if (curr->free && curr->size >= size){

            if (curr->size > size + sizeof(block_t)){
                split_block(curr, size);
            }

            curr->free = 0;
            return (void*)((unsigned char*)curr + sizeof(block_t));
        }

        curr = curr->next;
    }

    uart_puts("kmalloc failed!\n");
    return 0;
}

static void merge_blocks(){
    block_t *curr = free_list;

    while (curr && curr->next){
        if (curr->free && curr->next->free){
            curr->size += sizeof(block_t) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

void kfree(void *ptr){
    if (!ptr) return;

    block_t *block = (block_t*)((unsigned char*)ptr - sizeof(block_t));
    block->free = 1;

    merge_blocks();
}
