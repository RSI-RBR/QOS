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
    int eof;
    int error;
} FILE;

extern FILE* const qos_stdin;
extern FILE* const qos_stdout;
extern FILE* const qos_stderr;

#ifndef EOF
#define EOF (-1)
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

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
FILE* qos_fopen(const char* path, const char* mode);
int qos_fclose(FILE* stream);
size_t qos_fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t qos_fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
char* qos_fgets(char* s, int size, FILE* stream);
int qos_fputs(const char* s, FILE* stream);
int qos_fgetc(FILE* stream);
int qos_fputc(int c, FILE* stream);
int qos_putline(const char* s);
int qos_feof(FILE* stream);
int qos_ferror(FILE* stream);
void qos_clearerr(FILE* stream);
int qos_fseek(FILE* stream, long offset, int whence);
long qos_ftell(FILE* stream);
void qos_rewind(FILE* stream);

#ifndef QOS_STDIO_NO_MACROS
#define QOS_LOG(...) qos_printf(__VA_ARGS__)
#define QOS_ERR(...) qos_fprintf(qos_stderr, __VA_ARGS__)
#define printf qos_printf
#define fprintf qos_fprintf
#define snprintf qos_snprintf
#define fopen qos_fopen
#define fclose qos_fclose
#define fread qos_fread
#define fwrite qos_fwrite
#define fgets qos_fgets
#define fputs qos_fputs
#define fgetc qos_fgetc
#define fputc qos_fputc
#define puts qos_putline
#define feof qos_feof
#define ferror qos_ferror
#define clearerr qos_clearerr
#define fseek qos_fseek
#define ftell qos_ftell
#define rewind qos_rewind
#endif

#ifdef __cplusplus
}
#endif

#endif
