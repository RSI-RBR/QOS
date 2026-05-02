#include "memory.h"
#include "uart.h"
#include "mailbox.h"

#define BLOCK_MAGIC 0xB10CB10CUL
#define HEAP_FALLBACK_BASE 0x00000000UL
#define HEAP_FALLBACK_SIZE (1024UL * 1024UL * 1024UL) // 1GB fallback
#define HEAP_PHYS_CEIL 0x3F000000UL // keep below peripheral MMIO window
#define HEAP_MIN_BYTES (64UL * 1024UL)

extern unsigned long core3_stack_top;

typedef struct block {
    unsigned long magic;
    unsigned long size;
    int free;
    struct block *next;
} block_t;

static block_t *free_list = 0;
static unsigned char *heap_base = 0;
static unsigned long heap_size = 0;

#define ALIGN16(x) (((x) + 15) & ~15)

void memory_init(void){
    unsigned int arm_base32 = 0;
    unsigned int arm_size32 = 0;
    unsigned long arm_base = HEAP_FALLBACK_BASE;
    unsigned long arm_size = HEAP_FALLBACK_SIZE;
    if (mailbox_get_arm_memory(&arm_base32, &arm_size32) == 0){
        arm_base = (unsigned long)arm_base32;
        arm_size = (unsigned long)arm_size32;
    }

    unsigned long heap_start = ALIGN16((unsigned long)&core3_stack_top + 0x1000UL);
    unsigned long heap_end = arm_base + arm_size;
    if (heap_end > HEAP_PHYS_CEIL){
        heap_end = HEAP_PHYS_CEIL;
    }
    if (heap_start < arm_base){
        heap_start = ALIGN16(arm_base);
    }
    if (heap_end <= heap_start || (heap_end - heap_start) < (sizeof(block_t) + HEAP_MIN_BYTES)){
        uart_puts("Heap init failed: invalid heap range\n");
        free_list = 0;
        heap_base = 0;
        heap_size = 0;
        return;
    }

    heap_base = (unsigned char*)heap_start;
    heap_size = heap_end - heap_start;
    free_list = (block_t*)heap_base;

    free_list->magic = BLOCK_MAGIC;
    free_list->size = heap_size - sizeof(block_t);
    free_list->free = 1;
    free_list->next = 0;

    uart_puts("Heap initialized\n");
    uart_puts("Heap base=");
    uart_puthex((unsigned int)(unsigned long)heap_base);
    uart_puts(" size=");
    uart_putdec(heap_size / (1024UL * 1024UL));
    uart_puts("MB\n");
}

static void split_block(block_t *block, unsigned long size){
    block_t *new_block = (block_t*)((unsigned char*)block + sizeof(block_t) + size);

    new_block->magic = BLOCK_MAGIC;
    new_block->size = block->size - size - sizeof(block_t);
    new_block->free = 1;
    new_block->next = block->next;

    block->size = size;
    block->next = new_block;
}

void *kmalloc(unsigned long size){
    if (size == 0 || !free_list){
        return 0;
    }

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

static int ptr_in_heap(void *ptr){
    unsigned char *p = (unsigned char*)ptr;
    return heap_base && p >= (heap_base + sizeof(block_t)) && p < (heap_base + heap_size);
}

static block_t* ptr_to_block(void *ptr){
    if (!ptr_in_heap(ptr)){
        return 0;
    }
    block_t *block = (block_t*)((unsigned char*)ptr - sizeof(block_t));
    if (block->magic != BLOCK_MAGIC){
        return 0;
    }
    return block;
}

void kfree(void *ptr){
    if (!ptr) return;

    block_t *block = ptr_to_block(ptr);
    if (!block){
        uart_puts("kfree invalid ptr\n");
        return;
    }
    if (block->free){
        uart_puts("kfree double free\n");
        return;
    }
    block->free = 1;

    merge_blocks();
}

void kfree_secure(void* ptr, unsigned long size){
    if (!ptr){
        return;
    }

    block_t *block = ptr_to_block(ptr);
    if (!block){
        uart_puts("kfree_secure invalid ptr\n");
        return;
    }
    if (block->free){
        uart_puts("kfree_secure double free\n");
        return;
    }

    if (size == 0 || size > block->size){
        size = block->size;
    }

    volatile unsigned char* p = (volatile unsigned char*)ptr;
    for (unsigned long i = 0; i < size; i++){
        p[i] = 0;
    }

    kfree(ptr);

}
