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

#ifndef PQCLEAN_PREVENT_BRANCH_HACK
/*
 * Some PQClean/PQCode KEM verify paths call this macro to discourage
 * compiler-introduced branching in constant-time code paths.
 * Fallback keeps builds portable when the upstream compatibility layer
 * is not present in this tree.
 */
#define PQCLEAN_PREVENT_BRANCH_HACK(x) do { (void)(x); } while (0)
#endif

#endif
