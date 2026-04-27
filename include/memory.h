#ifndef MEMORY_H
#define MEMORY_H

void memory_init(void);

void *kmalloc(unsigned long size);
void kfree(void *ptr);
void kfree_secure(void* ptr, unsigned long size);

#endif
