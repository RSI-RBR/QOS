#define QOS_STDIO_NO_MACROS
#include "qos_stdio.h"
#include "syscall.h"

#define QOS_STDIO_PRINTF_BUF 768u

static FILE g_stdin_file = {0};
static FILE g_stdout_file = {1};
static FILE g_stderr_file = {2};

FILE* const qos_stdin = &g_stdin_file;
FILE* const qos_stdout = &g_stdout_file;
FILE* const qos_stderr = &g_stderr_file;

typedef struct qos_fmt_out {
    char* buf;
    size_t cap;
    size_t len;
} qos_fmt_out_t;

static void out_ch(qos_fmt_out_t* out, char c){
    if (!out){
        return;
    }
    if (out->buf && out->cap > 0u && out->len + 1u < out->cap){
        out->buf[out->len] = c;
    }
    out->len++;
}

static void out_repeat(qos_fmt_out_t* out, char c, unsigned int count){
    for (unsigned int i = 0u; i < count; i++){
        out_ch(out, c);
    }
}

static void out_str_n(qos_fmt_out_t* out, const char* s, size_t n){
    if (!s){
        s = "(null)";
        n = 6u;
    }
    for (size_t i = 0u; i < n && s[i]; i++){
        out_ch(out, s[i]);
    }
}

static size_t cstr_len(const char* s){
    size_t n = 0u;
    if (!s){
        return 6u;
    }
    while (s[n]){
        n++;
    }
    return n;
}

static int is_digit(char c){
    return c >= '0' && c <= '9';
}

static unsigned long long signed_abs_to_u64(long long v){
    if (v >= 0){
        return (unsigned long long)v;
    }
    return (unsigned long long)(-(v + 1LL)) + 1ULL;
}

static void out_unsigned(qos_fmt_out_t* out,
                         unsigned long long v,
                         unsigned int base,
                         int uppercase,
                         int negative,
                         int width,
                         int zero_pad,
                         int alt_prefix){
    char tmp[65];
    unsigned int n = 0u;
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    unsigned int prefix_len = 0u;
    char prefix[3];

    if (base < 2u || base > 16u){
        base = 10u;
    }

    if (v == 0ULL){
        tmp[n++] = '0';
    } else {
        while (v != 0ULL && n < (unsigned int)sizeof(tmp)){
            tmp[n++] = digits[v % base];
            v /= base;
        }
    }

    if (negative){
        prefix[prefix_len++] = '-';
    }
    if (alt_prefix && base == 16u){
        prefix[prefix_len++] = '0';
        prefix[prefix_len++] = uppercase ? 'X' : 'x';
    }

    unsigned int total = prefix_len + n;
    unsigned int pad = (width > (int)total) ? (unsigned int)width - total : 0u;
    if (!zero_pad){
        out_repeat(out, ' ', pad);
    }
    for (unsigned int i = 0u; i < prefix_len; i++){
        out_ch(out, prefix[i]);
    }
    if (zero_pad){
        out_repeat(out, '0', pad);
    }
    while (n > 0u){
        out_ch(out, tmp[--n]);
    }
}

static int read_int(const char** p){
    int v = 0;
    while (is_digit(**p)){
        v = (v * 10) + (**p - '0');
        (*p)++;
    }
    return v;
}

int qos_vsnprintf(char* out, size_t cap, const char* fmt, va_list ap){
    qos_fmt_out_t fo;
    const char* p;

    fo.buf = out;
    fo.cap = cap;
    fo.len = 0u;

    if (!fmt){
        fmt = "(null)";
    }

    for (p = fmt; *p; p++){
        if (*p != '%'){
            out_ch(&fo, *p);
            continue;
        }

        p++;
        if (*p == '%'){
            out_ch(&fo, '%');
            continue;
        }

        int left = 0;
        int plus = 0;
        int space = 0;
        int alt = 0;
        int zero = 0;
        int width = 0;
        int precision = -1;
        int length = 0; /* 0=default, 1=h, 2=hh, 3=l, 4=ll, 5=z */

        int parsing_flags = 1;
        while (parsing_flags){
            switch (*p){
                case '-': left = 1; p++; break;
                case '+': plus = 1; p++; break;
                case ' ': space = 1; p++; break;
                case '#': alt = 1; p++; break;
                case '0': zero = 1; p++; break;
                default: parsing_flags = 0; break;
            }
        }

        if (is_digit(*p)){
            width = read_int(&p);
        }
        if (*p == '.'){
            p++;
            precision = read_int(&p);
        }
        if (*p == 'h'){
            p++;
            length = 1;
            if (*p == 'h'){
                p++;
                length = 2;
            }
        } else if (*p == 'l'){
            p++;
            length = 3;
            if (*p == 'l'){
                p++;
                length = 4;
            }
        } else if (*p == 'z'){
            p++;
            length = 5;
        }

        char spec = *p;
        if (!spec){
            break;
        }

        (void)left;
        if (left){
            zero = 0;
        }

        if (spec == 's'){
            const char* s = va_arg(ap, const char*);
            size_t n = cstr_len(s);
            if (precision >= 0 && (size_t)precision < n){
                n = (size_t)precision;
            }
            if (width > (int)n && !left){
                out_repeat(&fo, ' ', (unsigned int)width - (unsigned int)n);
            }
            out_str_n(&fo, s, n);
            if (width > (int)n && left){
                out_repeat(&fo, ' ', (unsigned int)width - (unsigned int)n);
            }
            continue;
        }

        if (spec == 'c'){
            char c = (char)va_arg(ap, int);
            if (width > 1 && !left){
                out_repeat(&fo, ' ', (unsigned int)width - 1u);
            }
            out_ch(&fo, c);
            if (width > 1 && left){
                out_repeat(&fo, ' ', (unsigned int)width - 1u);
            }
            continue;
        }

        if (spec == 'd' || spec == 'i'){
            long long sv;
            if (length == 4){
                sv = va_arg(ap, long long);
            } else if (length == 3){
                sv = (long long)va_arg(ap, long);
            } else if (length == 5){
                sv = (long long)va_arg(ap, size_t);
            } else {
                sv = (long long)va_arg(ap, int);
            }
            int negative = sv < 0;
            if (!negative && (plus || space)){
                out_ch(&fo, plus ? '+' : ' ');
                if (width > 0){
                    width--;
                }
            }
            out_unsigned(&fo, signed_abs_to_u64(sv), 10u, 0, negative, width, zero, 0);
            continue;
        }

        if (spec == 'u' || spec == 'x' || spec == 'X' || spec == 'o'){
            unsigned long long uv;
            if (length == 4){
                uv = va_arg(ap, unsigned long long);
            } else if (length == 3){
                uv = (unsigned long long)va_arg(ap, unsigned long);
            } else if (length == 5){
                uv = (unsigned long long)va_arg(ap, size_t);
            } else {
                uv = (unsigned long long)va_arg(ap, unsigned int);
            }
            unsigned int base = (spec == 'o') ? 8u : ((spec == 'u') ? 10u : 16u);
            out_unsigned(&fo, uv, base, spec == 'X', 0, width, zero, alt);
            continue;
        }

        if (spec == 'p'){
            unsigned long long uv = (unsigned long long)(unsigned long)va_arg(ap, void*);
            out_unsigned(&fo, uv, 16u, 0, 0, width, zero, 1);
            continue;
        }

        if (spec == 'f' || spec == 'F' || spec == 'e' || spec == 'E' || spec == 'g' || spec == 'G'){
            (void)va_arg(ap, double);
            out_str_n(&fo, "<float>", 7u);
            continue;
        }

        out_ch(&fo, '%');
        out_ch(&fo, spec);
    }

    if (out && cap > 0u){
        size_t term = (fo.len < cap) ? fo.len : cap - 1u;
        out[term] = 0;
    }

    return (int)fo.len;
}

int qos_snprintf(char* out, size_t cap, const char* fmt, ...){
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = qos_vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

int qos_vprintf(const char* fmt, va_list ap){
    char buf[QOS_STDIO_PRINTF_BUF];
    int n = qos_vsnprintf(buf, sizeof(buf), fmt, ap);
    qos_puts(buf);
    return n;
}

int qos_printf(const char* fmt, ...){
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = qos_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int qos_vfprintf(FILE* stream, const char* fmt, va_list ap){
    (void)stream;
    return qos_vprintf(fmt, ap);
}

int qos_fprintf(FILE* stream, const char* fmt, ...){
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = qos_vfprintf(stream, fmt, ap);
    va_end(ap);
    return n;
}
