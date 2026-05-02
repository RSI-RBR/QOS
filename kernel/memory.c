#include "memory.h"
#include "uart.h"
#include "mailbox.h"
#include "spinlock.h"

#define HEAP_FALLBACK_SIZE (16UL * 1024UL * 1024UL)
#define HEAP_MIN_SIZE      (2UL * 1024UL * 1024UL)
#define HEAP_ALIGN         16UL
#define BLOCK_MAGIC 0xB10CB10CUL
// Keep in sync with loader PROGRAM_POOL_START.
#define HEAP_HARD_STOP     0x08000000UL

static unsigned char heap_fallback[HEAP_FALLBACK_SIZE] __attribute__((aligned(16)));
static unsigned char* heap_base = heap_fallback;
static unsigned long heap_size = HEAP_FALLBACK_SIZE;
static spinlock_t heap_lock;

typedef struct block {
    unsigned long magic;
    unsigned long size;
    int free;
    struct block *next;
} block_t;

static block_t *free_list = 0;

#define ALIGN16(x) (((x) + 15) & ~15)

static unsigned long align_up(unsigned long v, unsigned long a){
    return (v + (a - 1UL)) & ~(a - 1UL);
}

static void heap_init_region(unsigned char* base, unsigned long size){
    heap_base = base;
    heap_size = size;
    free_list = (block_t*)heap_base;
    free_list->magic = BLOCK_MAGIC;
    free_list->size = heap_size - sizeof(block_t);
    free_list->free = 1;
    free_list->next = 0;
}

static int heap_init_dynamic(void){
    extern unsigned long core3_stack_top;
    unsigned int arm_base = 0;
    unsigned int arm_size = 0;

    if (mailbox_get_arm_memory(&arm_base, &arm_size) != 0 || arm_size == 0u){
        return -1;
    }

    unsigned long dyn_start = align_up((unsigned long)&core3_stack_top + 0x1000UL, HEAP_ALIGN);
    unsigned long dyn_end = (unsigned long)arm_base + (unsigned long)arm_size;
    if (dyn_end > HEAP_HARD_STOP){
        dyn_end = HEAP_HARD_STOP;
    }
    if (dyn_end <= dyn_start){
        return -1;
    }

    unsigned long dyn_size = dyn_end - dyn_start;
    dyn_size &= ~(HEAP_ALIGN - 1UL);
    if (dyn_size < HEAP_MIN_SIZE || dyn_size <= sizeof(block_t)){
        return -1;
    }

    heap_init_region((unsigned char*)dyn_start, dyn_size);
    return 0;
}

void memory_init(void){
    spinlock_init(&heap_lock);

    if (heap_init_dynamic() != 0){
        heap_init_region(heap_fallback, HEAP_FALLBACK_SIZE);
    }

    uart_puts("Heap initialized\n");
    uart_puts("Heap base=");
    uart_puthex((unsigned int)(unsigned long)heap_base);
    uart_puts(" size=");
    uart_puthex((unsigned int)heap_size);
    uart_puts("\n");
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
    if (size == 0){
        return 0;
    }

    size = ALIGN16(size);
    unsigned long irq = spin_lock_irqsave(&heap_lock);

    block_t *curr = free_list;

    while (curr){
        if (curr->free && curr->size >= size){
            if (curr->size > size + sizeof(block_t)){
                split_block(curr, size);
            }

            curr->free = 0;
            void* out = (void*)((unsigned char*)curr + sizeof(block_t));
            spin_unlock_irqrestore(&heap_lock, irq);
            return out;
        }

        curr = curr->next;
    }

    spin_unlock_irqrestore(&heap_lock, irq);
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
    return p >= (heap_base + sizeof(block_t)) && p < (heap_base + heap_size);
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
    if (!ptr){
        return;
    }

    unsigned long irq = spin_lock_irqsave(&heap_lock);
    block_t *block = ptr_to_block(ptr);
    if (!block){
        spin_unlock_irqrestore(&heap_lock, irq);
        uart_puts("kfree invalid ptr\n");
        return;
    }
    if (block->free){
        spin_unlock_irqrestore(&heap_lock, irq);
        uart_puts("kfree double free\n");
        return;
    }
    block->free = 1;

    merge_blocks();
    spin_unlock_irqrestore(&heap_lock, irq);
}

void kfree_secure(void* ptr, unsigned long size){
    if (!ptr){
        return;
    }

    unsigned long irq = spin_lock_irqsave(&heap_lock);
    block_t *block = ptr_to_block(ptr);
    if (!block){
        spin_unlock_irqrestore(&heap_lock, irq);
        uart_puts("kfree_secure invalid ptr\n");
        return;
    }
    if (block->free){
        spin_unlock_irqrestore(&heap_lock, irq);
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

    block->free = 1;
    merge_blocks();
    spin_unlock_irqrestore(&heap_lock, irq);
}
