#ifndef QOS_STDIO_H
#define QOS_STDIO_H

#ifndef QOS_USERSPACE
#error "qos_stdio.h is for QOS user programs only; kernel code should use klog/uart"
#endif

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qos_FILE {
    int fd;
} FILE;

extern FILE* const qos_stdin;
extern FILE* const qos_stdout;
extern FILE* const qos_stderr;

#ifndef stdin
#define stdin qos_stdin
#endif
#ifndef stdout
#define stdout qos_stdout
#endif
#ifndef stderr
#define stderr qos_stderr
#endif

int qos_vsnprintf(char* out, size_t cap, const char* fmt, va_list ap);
int qos_snprintf(char* out, size_t cap, const char* fmt, ...);
int qos_vprintf(const char* fmt, va_list ap);
int qos_printf(const char* fmt, ...);
int qos_vfprintf(FILE* stream, const char* fmt, va_list ap);
int qos_fprintf(FILE* stream, const char* fmt, ...);

#ifndef QOS_STDIO_NO_MACROS
#define QOS_LOG(...) qos_printf(__VA_ARGS__)
#define QOS_ERR(...) qos_fprintf(qos_stderr, __VA_ARGS__)
#define printf qos_printf
#define fprintf qos_fprintf
#define snprintf qos_snprintf
#endif

#ifdef __cplusplus
}
#endif

#endif
