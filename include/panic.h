#ifndef PANIC_H
#define PANIC_H

__attribute__((noreturn)) void qos_panic(const char* reason, const char* file, unsigned int line);
void qos_stack_canary_init(void);

#define QOS_PANIC(reason_literal) qos_panic((reason_literal), __FILE__, __LINE__)

#define QOS_ASSERT(expr)                          \
    do {                                          \
        if (!(expr)) {                            \
            qos_panic("assert: " #expr, __FILE__, __LINE__); \
        }                                         \
    } while (0)

#define QOS_ASSERT_BOUNDS(index, limit) QOS_ASSERT((index) < (limit))

#endif
