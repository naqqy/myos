#ifndef NAQ_LIBC_INTTYPES_H
#define NAQ_LIBC_INTTYPES_H

#include <stdint.h>

/* x86_64: int64_t is long, int32_t is int. */
#define PRId64 "ld"
#define PRIi64 "ld"
#define PRIu64 "lu"
#define PRIx64 "lx"
#define PRIX64 "lX"
#define PRId32 "d"
#define PRIi32 "d"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"

#endif
