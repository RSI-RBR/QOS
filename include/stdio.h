#ifndef QOS_PUBLIC_STDIO_H
#define QOS_PUBLIC_STDIO_H

#ifdef QOS_USERSPACE
#include "qos_stdio.h"
#else

#include <stdarg.h>
#include <stddef.h>

typedef struct qos_kernel_stdio_FILE {
    int unused;
} FILE;

#ifndef EOF
#define EOF (-1)
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

int printf(const char* fmt, ...);
int fprintf(FILE* stream, const char* fmt, ...);
int sprintf(char* out, const char* fmt, ...);
int snprintf(char* out, size_t cap, const char* fmt, ...);
int vprintf(const char* fmt, va_list ap);
int vfprintf(FILE* stream, const char* fmt, va_list ap);
int vsprintf(char* out, const char* fmt, va_list ap);
int vsnprintf(char* out, size_t cap, const char* fmt, va_list ap);

#endif

#endif
