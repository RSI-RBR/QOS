#ifndef PQCLEAN_COMMON_COMPAT_H
#define PQCLEAN_COMMON_COMPAT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Compatibility shim used by newer PQClean/PQCode KEM sources.
 * Some variants include "compat.h" from verify.c/poly helpers.
 * Keep this lightweight and portable for freestanding builds.
 */
#if defined(_MSC_VER)
#define PQCLEAN_ALIGN(N) __declspec(align(N))
#define PQCLEAN_RESTRICT __restrict
#define PQCLEAN_INLINE __inline
#else
#define PQCLEAN_ALIGN(N) __attribute__((aligned(N)))
#define PQCLEAN_RESTRICT __restrict__
#define PQCLEAN_INLINE inline
#endif

#ifndef ALIGN
#define ALIGN(N) PQCLEAN_ALIGN(N)
#endif

#ifndef RESTRICT
#define RESTRICT PQCLEAN_RESTRICT
#endif

#ifndef INLINE
#define INLINE PQCLEAN_INLINE
#endif

#endif
