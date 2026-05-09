#ifndef QOS_USER_HEAP_H
#define QOS_USER_HEAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

size_t qos_heap_total(void);
size_t qos_heap_used(void);
size_t qos_heap_free(void);
size_t qos_heap_largest_free(void);

#ifdef __cplusplus
}
#endif

#endif
