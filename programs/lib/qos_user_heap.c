#include "program.h"
#include "qos_user_heap.h"

#define QOS_HEAP_MAGIC 0x51484D31UL
#define QOS_HEAP_ALIGN 16UL
#define QOS_HEAP_MIN_SPLIT 32UL

#if (QOS_PROGRAM_MEMORY_BYTES > QOS_PROGRAM_MAX_MEMORY_BYTES)
#error "QOS_PROGRAM_MEMORY_BYTES exceeds QOS_PROGRAM_MAX_MEMORY_BYTES"
#endif
#if ((QOS_PROGRAM_MEMORY_BYTES % QOS_PROGRAM_ALLOC_GRANULE_BYTES) != 0)
#error "QOS_PROGRAM_MEMORY_BYTES must be a 2 MiB multiple"
#endif

typedef struct qos_heap_block {
    unsigned long magic;
    unsigned long size;
    struct qos_heap_block* next;
    struct qos_heap_block* prev;
    unsigned int free;
} qos_heap_block_t;

extern unsigned char __qos_image_end[];

static qos_heap_block_t* g_heap_head = 0;
static unsigned long g_heap_start = 0;
static unsigned long g_heap_end = 0;
static int g_heap_ready = 0;

static unsigned long align_up(unsigned long v, unsigned long align){
    return (v + align - 1UL) & ~(align - 1UL);
}

static int align_request(size_t size, unsigned long* out){
    if (!out || size > (size_t)(((unsigned long)-1) - (QOS_HEAP_ALIGN - 1UL))){
        return 0;
    }
    *out = align_up((unsigned long)size, QOS_HEAP_ALIGN);
    return 1;
}

static unsigned long header_bytes(void){
    return align_up((unsigned long)sizeof(qos_heap_block_t), QOS_HEAP_ALIGN);
}

static void* block_payload(qos_heap_block_t* block){
    return (void*)((unsigned char*)block + header_bytes());
}

static qos_heap_block_t* payload_block(void* ptr){
    return (qos_heap_block_t*)((unsigned char*)ptr - header_bytes());
}

static int ptr_in_heap(unsigned long p){
    return g_heap_ready && p >= g_heap_start && p < g_heap_end;
}

static unsigned long current_program_base(void){
    unsigned long pc;
    asm volatile("adr %0, ." : "=r"(pc));
    return pc & ~(QOS_PROGRAM_ALLOC_GRANULE_BYTES - 1UL);
}

static unsigned long runtime_image_end(void){
    unsigned long base = current_program_base();
    unsigned long image_end = (unsigned long)__qos_image_end;

    /*
     * Raw QOS program images are loaded at a dynamic slot without applying ELF
     * relocations. Some toolchains therefore materialize linker symbols as
     * offsets from 0. Convert those offsets to the real runtime slot address.
     */
    if (image_end < QOS_PROGRAM_MEMORY_BYTES){
        return base + image_end;
    }
    return image_end;
}

static void heap_init(void){
    if (g_heap_ready){
        return;
    }

    unsigned long image_end = runtime_image_end();
    unsigned long slot_base = image_end & ~(QOS_PROGRAM_ALLOC_GRANULE_BYTES - 1UL);
    unsigned long heap_start = align_up(image_end, QOS_HEAP_ALIGN);
    unsigned long heap_end = slot_base + QOS_PROGRAM_MEMORY_BYTES -
                             QOS_USER_STACK_BYTES -
                             QOS_USER_GUARD_PAGE_BYTES;
    unsigned long hdr = header_bytes();

    g_heap_start = heap_start;
    g_heap_end = heap_end;
    g_heap_head = 0;
    g_heap_ready = 1;

    if (heap_end <= heap_start + hdr + QOS_HEAP_MIN_SPLIT){
        return;
    }

    g_heap_head = (qos_heap_block_t*)heap_start;
    g_heap_head->magic = QOS_HEAP_MAGIC;
    g_heap_head->size = heap_end - heap_start - hdr;
    g_heap_head->next = 0;
    g_heap_head->prev = 0;
    g_heap_head->free = 1u;
}

static int valid_block(qos_heap_block_t* block){
    unsigned long addr = (unsigned long)block;
    if (!ptr_in_heap(addr) || addr + header_bytes() > g_heap_end){
        return 0;
    }
    if (block->magic != QOS_HEAP_MAGIC){
        return 0;
    }
    if ((unsigned long)block_payload(block) + block->size > g_heap_end){
        return 0;
    }
    return 1;
}

static void split_block(qos_heap_block_t* block, unsigned long wanted){
    unsigned long hdr = header_bytes();
    if (!valid_block(block) || block->size < wanted + hdr + QOS_HEAP_MIN_SPLIT){
        return;
    }

    qos_heap_block_t* next = (qos_heap_block_t*)((unsigned char*)block_payload(block) + wanted);
    next->magic = QOS_HEAP_MAGIC;
    next->size = block->size - wanted - hdr;
    next->next = block->next;
    next->prev = block;
    next->free = 1u;

    if (next->next){
        next->next->prev = next;
    }

    block->size = wanted;
    block->next = next;
}

static void coalesce_next(qos_heap_block_t* block){
    unsigned long hdr = header_bytes();
    qos_heap_block_t* next;
    if (!valid_block(block)){
        return;
    }

    next = block->next;
    if (!next || !valid_block(next) || !next->free){
        return;
    }

    block->size += hdr + next->size;
    block->next = next->next;
    if (block->next){
        block->next->prev = block;
    }

    next->magic = 0;
    next->size = 0;
    next->next = 0;
    next->prev = 0;
    next->free = 0u;
}

void *malloc(size_t size){
    unsigned long wanted;
    qos_heap_block_t* block;

    if (size == 0u){
        return 0;
    }

    heap_init();
    if (!align_request(size, &wanted)){
        return 0;
    }

    for (block = g_heap_head; block; block = block->next){
        if (!valid_block(block)){
            return 0;
        }
        if (block->free && block->size >= wanted){
            split_block(block, wanted);
            block->free = 0u;
            return block_payload(block);
        }
    }

    return 0;
}

void free(void *ptr){
    qos_heap_block_t* block;
    if (!ptr){
        return;
    }

    heap_init();
    if (!ptr_in_heap((unsigned long)ptr)){
        return;
    }

    block = payload_block(ptr);
    if (!valid_block(block)){
        return;
    }
    if (block->free){
        return;
    }

    block->free = 1u;
    coalesce_next(block);
    if (block->prev && valid_block(block->prev) && block->prev->free){
        coalesce_next(block->prev);
    }
}

void *calloc(size_t nmemb, size_t size){
    size_t total;
    unsigned char* p;

    if (size != 0u && nmemb > ((size_t)-1) / size){
        return 0;
    }

    total = nmemb * size;
    p = (unsigned char*)malloc(total);
    if (!p){
        return 0;
    }

    for (size_t i = 0; i < total; i++){
        p[i] = 0u;
    }
    return p;
}

void *realloc(void *ptr, size_t size){
    qos_heap_block_t* block;
    unsigned long wanted;
    void* out;

    if (!ptr){
        return malloc(size);
    }
    if (size == 0u){
        free(ptr);
        return 0;
    }

    heap_init();
    if (!ptr_in_heap((unsigned long)ptr)){
        return 0;
    }

    block = payload_block(ptr);
    if (!valid_block(block) || block->free){
        return 0;
    }

    if (!align_request(size, &wanted)){
        return 0;
    }
    if (block->size >= wanted){
        split_block(block, wanted);
        return ptr;
    }

    if (block->next && valid_block(block->next) && block->next->free &&
        block->size + header_bytes() + block->next->size >= wanted){
        coalesce_next(block);
        split_block(block, wanted);
        block->free = 0u;
        return ptr;
    }

    out = malloc(size);
    if (!out){
        return 0;
    }

    unsigned char* dst = (unsigned char*)out;
    unsigned char* src = (unsigned char*)ptr;
    unsigned long copy = block->size < (unsigned long)size ? block->size : (unsigned long)size;
    for (unsigned long i = 0; i < copy; i++){
        dst[i] = src[i];
    }

    free(ptr);
    return out;
}

size_t qos_heap_total(void){
    heap_init();
    if (!g_heap_head){
        return 0u;
    }
    return (size_t)(g_heap_end - g_heap_start - header_bytes());
}

size_t qos_heap_used(void){
    size_t used = 0u;
    qos_heap_block_t* block;
    heap_init();
    for (block = g_heap_head; block; block = block->next){
        if (!valid_block(block)){
            break;
        }
        if (!block->free){
            used += (size_t)block->size;
        }
    }
    return used;
}

size_t qos_heap_free(void){
    size_t free_bytes = 0u;
    qos_heap_block_t* block;
    heap_init();
    for (block = g_heap_head; block; block = block->next){
        if (!valid_block(block)){
            break;
        }
        if (block->free){
            free_bytes += (size_t)block->size;
        }
    }
    return free_bytes;
}

size_t qos_heap_largest_free(void){
    size_t largest = 0u;
    qos_heap_block_t* block;
    heap_init();
    for (block = g_heap_head; block; block = block->next){
        if (!valid_block(block)){
            break;
        }
        if (block->free && block->size > largest){
            largest = (size_t)block->size;
        }
    }
    return largest;
}
