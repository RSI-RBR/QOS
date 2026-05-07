#ifndef STRING_H
#define STRING_H

#include <stddef.h>

int kstrlen(const char *str);
int kstrcmp(const char *a, const char *b);
void kstrcpy(char *dest, const char *src);

void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

#endif
