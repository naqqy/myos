/*
 * NAQ OS small C library.
 *
 * The parts of the standard C library that ported code needs (the QuickJS
 * engine is the first user): a memory allocator, string functions,
 * printf-style formatting, sorting and the clock.
 *
 * It is built into an archive (libnaqc.a), so an app only gets the parts
 * it actually calls. strlen, memset and memcpy are not here: they are in
 * the runtime every app already links (runtime/string.c).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <assert.h>
#include <errno.h>

/* From the app runtime (see userapps/include/naqos.h). */
extern long long write(int fd, const void* buf, unsigned long long len);
extern unsigned long long get_ticks(void);
extern unsigned long long clock_read(void);

int errno;

/* ------------------------------------------------------------------ */
/* Memory allocator                                                   */
/* ------------------------------------------------------------------ */

/*
 * One contiguous heap, split into blocks. Every block starts with an
 * 8-byte header holding its size (a multiple of 16) and two flags. Free
 * blocks also store their size in their last 8 bytes, so a block being
 * freed can find a free neighbour in front of it and merge with it.
 *
 * Free blocks are kept in lists by size: one list per exact size below
 * 1024 bytes (found instantly through a bitmap), and one per power of two
 * above that.
 */

#define BLOCK_USED       1ull
#define BLOCK_PREV_USED  2ull
#define BLOCK_FLAGS      15ull
#define BLOCK_HEADER     8ull
#define BLOCK_MIN        32ull

#define SMALL_BINS       64
#define TOTAL_BINS       104

typedef struct free_block {
    uint64_t head;
    struct free_block* next;
    struct free_block* prev;
} free_block_t;

static free_block_t* g_bins[TOTAL_BINS];
static uint64_t g_small_map;
static uint8_t* g_heap_start;
static uint8_t* g_heap_end;
static size_t g_heap_used;

static inline uint64_t block_size(const free_block_t* b) {
    return b->head & ~BLOCK_FLAGS;
}

static inline free_block_t* block_next(free_block_t* b) {
    return (free_block_t*)((uint8_t*)b + block_size(b));
}

static inline void block_set_footer(free_block_t* b) {
    uint64_t size = block_size(b);

    *(uint64_t*)((uint8_t*)b + size - 8) = size;
}

static inline unsigned bin_index(uint64_t size) {
    if (size < 1024) {
        return (unsigned)(size >> 4);
    }

    unsigned bin = SMALL_BINS + (63u - (unsigned)__builtin_clzll(size)) - 10u;

    return bin < TOTAL_BINS ? bin : TOTAL_BINS - 1;
}

static void bin_insert(free_block_t* b) {
    unsigned bin = bin_index(block_size(b));

    b->prev = NULL;
    b->next = g_bins[bin];

    if (b->next) {
        b->next->prev = b;
    }

    g_bins[bin] = b;

    if (bin < SMALL_BINS) {
        g_small_map |= 1ull << bin;
    }
}

static void bin_remove(free_block_t* b) {
    unsigned bin = bin_index(block_size(b));

    if (b->prev) {
        b->prev->next = b->next;
    } else {
        g_bins[bin] = b->next;

        if (!b->next && bin < SMALL_BINS) {
            g_small_map &= ~(1ull << bin);
        }
    }

    if (b->next) {
        b->next->prev = b->prev;
    }
}

void naq_heap_init(void* base, size_t size) {
    uintptr_t start = ((uintptr_t)base + 15) & ~(uintptr_t)15;
    uintptr_t end = ((uintptr_t)base + size) & ~(uintptr_t)15;

    for (unsigned i = 0; i < TOTAL_BINS; i++) {
        g_bins[i] = NULL;
    }

    g_small_map = 0;
    g_heap_used = 0;
    g_heap_start = NULL;
    g_heap_end = NULL;

    if (end < start + 4096) {
        return;
    }

    /*
     * Block headers sit 8 bytes before a 16-byte boundary so that the
     * memory handed out is 16-byte aligned. The heap ends with a header
     * marked "used" that stops merging at the end.
     */
    start += 8;
    end -= 8;

    g_heap_start = (uint8_t*)start;
    g_heap_end = (uint8_t*)end;

    free_block_t* first = (free_block_t*)start;

    first->head = (uint64_t)(end - start) | BLOCK_PREV_USED;
    block_set_footer(first);

    ((free_block_t*)end)->head = BLOCK_USED;

    bin_insert(first);
}

size_t naq_heap_used(void) {
    return g_heap_used;
}

/* Take a free block out of its list and cut it down to `need` bytes. */
static void* block_take(free_block_t* b, uint64_t need) {
    uint64_t size = block_size(b);

    bin_remove(b);

    if (size - need >= BLOCK_MIN) {
        free_block_t* rest = (free_block_t*)((uint8_t*)b + need);

        rest->head = (size - need) | BLOCK_PREV_USED;
        block_set_footer(rest);
        bin_insert(rest);

        size = need;
    } else {
        block_next(b)->head |= BLOCK_PREV_USED;
    }

    b->head = size | BLOCK_USED | (b->head & BLOCK_PREV_USED);
    g_heap_used += size;

    return (uint8_t*)b + BLOCK_HEADER;
}

void* malloc(size_t size) {
    if (!g_heap_start || size > (size_t)(g_heap_end - g_heap_start)) {
        return NULL;
    }

    uint64_t need = (size + BLOCK_HEADER + 15) & ~15ull;

    if (need < BLOCK_MIN) {
        need = BLOCK_MIN;
    }

    unsigned bin = bin_index(need);

    if (bin < SMALL_BINS) {
        /* Every block in a small list has exactly that list's size. */
        uint64_t map = g_small_map & ~((1ull << bin) - 1);

        if (map) {
            return block_take(g_bins[__builtin_ctzll(map)], need);
        }

        bin = SMALL_BINS;
    }

    for (; bin < TOTAL_BINS; bin++) {
        for (free_block_t* b = g_bins[bin]; b; b = b->next) {
            if (block_size(b) >= need) {
                return block_take(b, need);
            }
        }
    }

    return NULL;
}

void free(void* ptr) {
    if (!ptr) {
        return;
    }

    free_block_t* b = (free_block_t*)((uint8_t*)ptr - BLOCK_HEADER);
    uint64_t size = block_size(b);
    uint64_t prev_used = b->head & BLOCK_PREV_USED;

    g_heap_used -= size;

    free_block_t* next = (free_block_t*)((uint8_t*)b + size);

    if (!(next->head & BLOCK_USED)) {
        bin_remove(next);
        size += block_size(next);
    }

    if (!prev_used) {
        uint64_t prev_size = *(uint64_t*)((uint8_t*)b - 8);
        free_block_t* prev = (free_block_t*)((uint8_t*)b - prev_size);

        bin_remove(prev);
        size += prev_size;
        b = prev;
        prev_used = BLOCK_PREV_USED;   /* two free blocks never touch */
    }

    b->head = size | prev_used;
    block_set_footer(b);
    block_next(b)->head &= ~BLOCK_PREV_USED;

    bin_insert(b);
}

size_t malloc_usable_size(void* ptr) {
    if (!ptr) {
        return 0;
    }

    free_block_t* b = (free_block_t*)((uint8_t*)ptr - BLOCK_HEADER);

    return (size_t)(block_size(b) - BLOCK_HEADER);
}

void* calloc(size_t count, size_t size) {
    if (size && count > (size_t)-1 / size) {
        return NULL;
    }

    void* p = malloc(count * size);

    if (p) {
        memset(p, 0, count * size);
    }

    return p;
}

/* Give the unused tail of a used block back to the heap. */
static void block_trim(free_block_t* b, uint64_t need) {
    uint64_t size = block_size(b);

    if (size - need < 64) {
        return;
    }

    free_block_t* rest = (free_block_t*)((uint8_t*)b + need);

    b->head = need | (b->head & BLOCK_FLAGS);
    rest->head = (size - need) | BLOCK_USED | BLOCK_PREV_USED;

    free((uint8_t*)rest + BLOCK_HEADER);
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }

    if (size == 0) {
        free(ptr);
        return NULL;
    }

    if (size > (size_t)(g_heap_end - g_heap_start)) {
        return NULL;
    }

    free_block_t* b = (free_block_t*)((uint8_t*)ptr - BLOCK_HEADER);
    uint64_t old = block_size(b);
    uint64_t need = (size + BLOCK_HEADER + 15) & ~15ull;

    if (need < BLOCK_MIN) {
        need = BLOCK_MIN;
    }

    if (need <= old) {
        block_trim(b, need);
        return ptr;
    }

    /* Grow in place when the block behind this one is free and big enough. */
    free_block_t* next = (free_block_t*)((uint8_t*)b + old);

    if (!(next->head & BLOCK_USED) && old + block_size(next) >= need) {
        uint64_t total = old + block_size(next);

        bin_remove(next);
        b->head = total | (b->head & BLOCK_FLAGS);
        block_next(b)->head |= BLOCK_PREV_USED;
        g_heap_used += total - old;

        block_trim(b, need);
        return ptr;
    }

    void* fresh = malloc(size);

    if (!fresh) {
        return NULL;
    }

    memcpy(fresh, ptr, old - BLOCK_HEADER);
    free(ptr);

    return fresh;
}

/* ------------------------------------------------------------------ */
/* Strings                                                            */
/* ------------------------------------------------------------------ */

void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    if (d == s || n == 0) {
        return dst;
    }

    if (d < s || d >= s + n) {
        return memcpy(dst, src, n);
    }

    while (n--) {
        d[n] = s[n];
    }

    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;

    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }

    return 0;
}

void* memchr(const void* s, int c, size_t n) {
    const uint8_t* p = (const uint8_t*)s;

    for (size_t i = 0; i < n; i++) {
        if (p[i] == (uint8_t)c) {
            return (void*)(p + i);
        }
    }

    return NULL;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }

    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) {
            return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
        }
    }

    return 0;
}

char* strcpy(char* dst, const char* src) {
    char* d = dst;

    while ((*d++ = *src++)) {
    }

    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;

    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }

    for (; i < n; i++) {
        dst[i] = 0;
    }

    return dst;
}

char* strcat(char* dst, const char* src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

char* strchr(const char* s, int c) {
    for (;; s++) {
        if (*s == (char)c) {
            return (char*)s;
        }

        if (!*s) {
            return NULL;
        }
    }
}

char* strrchr(const char* s, int c) {
    const char* found = NULL;

    for (;; s++) {
        if (*s == (char)c) {
            found = s;
        }

        if (!*s) {
            return (char*)found;
        }
    }
}

char* strstr(const char* hay, const char* needle) {
    size_t n = strlen(needle);

    if (n == 0) {
        return (char*)hay;
    }

    for (; *hay; hay++) {
        if (*hay == *needle && strncmp(hay, needle, n) == 0) {
            return (char*)hay;
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Numbers and sorting                                                */
/* ------------------------------------------------------------------ */

int abs(int v) {
    return v < 0 ? -v : v;
}

static int digit_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

unsigned long strtoul(const char* s, char** end, int base) {
    const char* p = s;
    int negative = 0;
    unsigned long value = 0;
    int any = 0;

    while (*p == ' ' || (*p >= 9 && *p <= 13)) {
        p++;
    }

    if (*p == '+' || *p == '-') {
        negative = (*p == '-');
        p++;
    }

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        digit_value(p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = (p[0] == '0') ? 8 : 10;
    }

    while (digit_value(*p) < base) {
        value = value * (unsigned long)base + (unsigned long)digit_value(*p);
        p++;
        any = 1;
    }

    if (end) {
        *end = (char*)(any ? p : s);
    }

    return negative ? (unsigned long)(-(long)value) : value;
}

long strtol(const char* s, char** end, int base) {
    return (long)strtoul(s, end, base);
}

int atoi(const char* s) {
    return (int)strtol(s, NULL, 10);
}

double strtod(const char* s, char** end) {
    const char* p = s;
    int negative = 0;
    long double value = 0;
    int exponent = 0;
    int any = 0;

    while (*p == ' ' || (*p >= 9 && *p <= 13)) {
        p++;
    }

    if (*p == '+' || *p == '-') {
        negative = (*p == '-');
        p++;
    }

    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p++ - '0');
        any = 1;
    }

    if (*p == '.') {
        p++;

        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p++ - '0');
            exponent--;
            any = 1;
        }
    }

    if (any && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        int exp_negative = 0;
        int e = 0;

        if (*q == '+' || *q == '-') {
            exp_negative = (*q == '-');
            q++;
        }

        if (*q >= '0' && *q <= '9') {
            while (*q >= '0' && *q <= '9') {
                if (e < 10000) {
                    e = e * 10 + (*q - '0');
                }
                q++;
            }

            exponent += exp_negative ? -e : e;
            p = q;
        }
    }

    if (end) {
        *end = (char*)(any ? p : s);
    }

    long double scale = 10;
    int n = exponent < 0 ? -exponent : exponent;
    long double factor = 1;

    while (n) {
        if (n & 1) {
            factor *= scale;
        }

        scale *= scale;
        n >>= 1;
    }

    value = exponent < 0 ? value / factor : value * factor;

    return (double)(negative ? -value : value);
}

static void swap_bytes(uint8_t* a, uint8_t* b, size_t size) {
    while (size--) {
        uint8_t t = *a;

        *a++ = *b;
        *b++ = t;
    }
}

/* Heap sort: no recursion and no worst case worse than n log n. */
void qsort(void* base, size_t count, size_t size,
           int (*cmp)(const void*, const void*)) {
    uint8_t* a = (uint8_t*)base;

    if (count < 2 || size == 0) {
        return;
    }

    for (size_t start = count / 2, end = count;;) {
        if (start > 0) {
            start--;
        } else {
            end--;

            if (end == 0) {
                break;
            }

            swap_bytes(a, a + end * size, size);
        }

        size_t root = start;

        for (;;) {
            size_t child = root * 2 + 1;

            if (child >= end) {
                break;
            }

            if (child + 1 < end &&
                cmp(a + child * size, a + (child + 1) * size) < 0) {
                child++;
            }

            if (cmp(a + root * size, a + child * size) >= 0) {
                break;
            }

            swap_bytes(a + root * size, a + child * size, size);
            root = child;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Formatted output                                                   */
/* ------------------------------------------------------------------ */

struct naq_file {
    int fd;
};

static struct naq_file g_stdin = { 0 };
static struct naq_file g_stdout = { 1 };
static struct naq_file g_stderr = { 1 };

FILE* stdin = &g_stdin;
FILE* stdout = &g_stdout;
FILE* stderr = &g_stderr;

/*
 * Where formatted text goes: either a caller's buffer (snprintf), or a
 * small buffer that is written to the console each time it fills.
 */
typedef struct sink {
    char* buf;
    size_t cap;
    size_t len;      /* characters produced so far, even if they did not fit */
    size_t fill;     /* console only: characters waiting in buf */
    int console;
} sink_t;

static void sink_put(sink_t* s, char c) {
    if (s->console) {
        s->buf[s->fill++] = c;

        if (s->fill == s->cap) {
            write(1, s->buf, s->fill);
            s->fill = 0;
        }
    } else if (s->len + 1 < s->cap) {
        s->buf[s->len] = c;
    }

    s->len++;
}

static void sink_pad(sink_t* s, char c, int n) {
    while (n-- > 0) {
        sink_put(s, c);
    }
}

#define FMT_LEFT   1
#define FMT_ZERO   2
#define FMT_PLUS   4
#define FMT_SPACE  8
#define FMT_ALT    16
#define FMT_UPPER  32

/* Write `body` with its sign/prefix, padded out to `width`. */
static void emit_field(sink_t* s, const char* prefix, const char* body,
                       int body_len, int zeros, int width, int flags) {
    int prefix_len = (int)strlen(prefix);
    int total = prefix_len + zeros + body_len;
    int pad = width > total ? width - total : 0;

    if (!(flags & (FMT_LEFT | FMT_ZERO))) {
        sink_pad(s, ' ', pad);
    }

    while (*prefix) {
        sink_put(s, *prefix++);
    }

    if ((flags & FMT_ZERO) && !(flags & FMT_LEFT)) {
        sink_pad(s, '0', pad);
    }

    sink_pad(s, '0', zeros);

    for (int i = 0; i < body_len; i++) {
        sink_put(s, body[i]);
    }

    if (flags & FMT_LEFT) {
        sink_pad(s, ' ', pad);
    }
}

static void emit_integer(sink_t* s, unsigned long long value, int negative,
                         unsigned base, int width, int precision, int flags) {
    char digits[32];
    int n = 0;
    const char* set = (flags & FMT_UPPER) ? "0123456789ABCDEF" : "0123456789abcdef";
    const char* prefix = "";

    if (value == 0 && precision == 0) {
        /* "%.0d" of zero prints nothing */
    } else {
        do {
            digits[n++] = set[value % base];
            value /= base;
        } while (value);
    }

    char body[32];

    for (int i = 0; i < n; i++) {
        body[i] = digits[n - 1 - i];
    }

    if (negative) {
        prefix = "-";
    } else if (flags & FMT_PLUS) {
        prefix = "+";
    } else if (flags & FMT_SPACE) {
        prefix = " ";
    } else if ((flags & FMT_ALT) && base == 16 && n > 0) {
        prefix = (flags & FMT_UPPER) ? "0X" : "0x";
    }

    int zeros = precision > n ? precision - n : 0;

    if (precision >= 0) {
        flags &= ~FMT_ZERO;
    }

    emit_field(s, prefix, body, n, zeros, width, flags);
}

/*
 * Floating point: %f, %e and %g. Good to about 17 significant digits,
 * which is all the ported code asks of printf (QuickJS converts numbers
 * to text with its own exact routine).
 */
static void emit_float(sink_t* s, double number, char conv, int width,
                       int precision, int flags) {
    char body[400];
    int n = 0;
    const char* prefix = "";
    long double v = number;

    if (__builtin_signbit(number)) {
        prefix = "-";
        v = -v;
    } else if (flags & FMT_PLUS) {
        prefix = "+";
    } else if (flags & FMT_SPACE) {
        prefix = " ";
    }

    if (number != number || v > 1.7976931348623157e308L) {
        const char* word = (number != number) ? "nan" : "inf";

        emit_field(s, (number != number) ? "" : prefix, word, 3, 0, width,
                   flags & ~FMT_ZERO);
        return;
    }

    if (precision < 0) {
        precision = 6;
    }

    if (precision > 60) {
        precision = 60;
    }

    /* v = mantissa * 10^exp10 with 1 <= mantissa < 10 (or v == 0). */
    int exp10 = 0;

    if (v != 0) {
        while (v >= 1e16L) { v /= 1e16L; exp10 += 16; }
        while (v >= 10) { v /= 10; exp10++; }
        while (v < 1e-15L) { v *= 1e16L; exp10 -= 16; }
        while (v < 1) { v *= 10; exp10--; }
    }

    /* 19 significant digits as an integer: d.dddddddddddddddddd */
    unsigned long long sig = (unsigned long long)(v * 1e18L + 0.5L);

    if (sig >= 10000000000000000000ull) {
        sig /= 10;
        exp10++;
    }

    char dig[24];

    for (int i = 18; i >= 0; i--) {
        dig[i] = (char)('0' + sig % 10);
        sig /= 10;
    }

    int style_e = (conv == 'e');
    int strip = 0;

    if (conv == 'g') {
        if (precision == 0) {
            precision = 1;
        }

        style_e = (exp10 < -4 || exp10 >= precision);
        precision = style_e ? precision - 1 : precision - 1 - exp10;
        strip = !(flags & FMT_ALT);
    }

    /* Number of digits kept: those before the point plus `precision`. */
    int keep = style_e ? 1 + precision : exp10 + 1 + precision;

    if (keep < 0) {
        keep = 0;
    }

    if (keep < 19) {
        /* round half up on the decimal digits */
        int carry = dig[keep] >= '5';

        for (int i = keep - 1; i >= 0 && carry; i--) {
            if (dig[i] == '9') {
                dig[i] = '0';
            } else {
                dig[i]++;
                carry = 0;
            }
        }

        if (carry) {
            /* 9.99 -> 10.0: shift right, one more digit in front */
            for (int i = 18; i > 0; i--) {
                dig[i] = dig[i - 1];
            }

            dig[0] = '1';
            exp10++;

            if (!style_e) {
                keep++;
            }
        }

        for (int i = keep; i < 19; i++) {
            dig[i] = '0';
        }
    }

#define DIGIT(i) (((i) >= 0 && (i) < 19) ? dig[(i)] : '0')

    if (style_e) {
        body[n++] = DIGIT(0);

        if (precision > 0 || (flags & FMT_ALT)) {
            body[n++] = '.';
        }

        for (int i = 1; i <= precision; i++) {
            body[n++] = DIGIT(i);
        }
    } else {
        if (exp10 < 0) {
            body[n++] = '0';
        } else {
            for (int i = 0; i <= exp10 && n < 320; i++) {
                body[n++] = DIGIT(i);
            }
        }

        if (precision > 0 || (flags & FMT_ALT)) {
            body[n++] = '.';
        }

        for (int i = 1; i <= precision; i++) {
            body[n++] = DIGIT(exp10 + i);
        }
    }

#undef DIGIT

    if (strip && precision > 0) {
        while (n > 0 && body[n - 1] == '0') {
            n--;
        }

        if (n > 0 && body[n - 1] == '.') {
            n--;
        }
    }

    if (style_e) {
        int e = exp10 < 0 ? -exp10 : exp10;

        body[n++] = 'e';
        body[n++] = exp10 < 0 ? '-' : '+';

        if (e >= 100) {
            body[n++] = (char)('0' + e / 100);
        }

        body[n++] = (char)('0' + (e / 10) % 10);
        body[n++] = (char)('0' + e % 10);
    }

    emit_field(s, prefix, body, n, 0, width, flags);
}

static void format(sink_t* s, const char* fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            sink_put(s, *fmt);
            continue;
        }

        fmt++;

        int flags = 0;

        for (;; fmt++) {
            if (*fmt == '-') flags |= FMT_LEFT;
            else if (*fmt == '0') flags |= FMT_ZERO;
            else if (*fmt == '+') flags |= FMT_PLUS;
            else if (*fmt == ' ') flags |= FMT_SPACE;
            else if (*fmt == '#') flags |= FMT_ALT;
            else break;
        }

        int width = 0;

        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;

            if (width < 0) {
                flags |= FMT_LEFT;
                width = -width;
            }
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt++ - '0');
            }
        }

        int precision = -1;

        if (*fmt == '.') {
            fmt++;
            precision = 0;

            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt++ - '0');
                }
            }
        }

        /* 0 = int, 1 = long (also size_t, intmax_t, ptrdiff_t), -1 = short, -2 = char */
        int size = 0;

        for (;; fmt++) {
            if (*fmt == 'l' || *fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'q') {
                size = 1;
            } else if (*fmt == 'h') {
                size = (size == -1) ? -2 : -1;
            } else if (*fmt != 'L') {
                break;
            }
        }

        char conv = *fmt;

        if (!conv) {
            break;
        }

        switch (conv) {
            case 'd':
            case 'i': {
                long long v = size > 0 ? va_arg(ap, long) : va_arg(ap, int);

                if (size == -1) v = (short)v;
                if (size == -2) v = (signed char)v;

                emit_integer(s, v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v,
                             v < 0, 10, width, precision, flags);
                break;
            }

            case 'u':
            case 'x':
            case 'X':
            case 'o': {
                unsigned long long v =
                    size > 0 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);

                if (size == -1) v = (unsigned short)v;
                if (size == -2) v = (unsigned char)v;

                if (conv == 'X') {
                    flags |= FMT_UPPER;
                }

                emit_integer(s, v, 0, conv == 'u' ? 10 : (conv == 'o' ? 8 : 16),
                             width, precision, flags & ~(FMT_PLUS | FMT_SPACE));
                break;
            }

            case 'p': {
                unsigned long long v = (unsigned long long)(uintptr_t)va_arg(ap, void*);

                emit_integer(s, v, 0, 16, width, precision,
                             (flags | FMT_ALT) & ~(FMT_PLUS | FMT_SPACE));
                break;
            }

            case 'c': {
                char c = (char)va_arg(ap, int);

                emit_field(s, "", &c, 1, 0, width, flags & ~FMT_ZERO);
                break;
            }

            case 's': {
                const char* text = va_arg(ap, const char*);

                if (!text) {
                    text = "(null)";
                }

                int len = 0;

                while (text[len] && (precision < 0 || len < precision)) {
                    len++;
                }

                emit_field(s, "", text, len, 0, width, flags & ~FMT_ZERO);
                break;
            }

            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G': {
                double v = va_arg(ap, double);
                char lower = (char)(conv | 32);

                emit_float(s, v, lower, width, precision, flags);
                break;
            }

            case '%':
                sink_put(s, '%');
                break;

            default:
                sink_put(s, '%');
                sink_put(s, conv);
                break;
        }
    }
}

int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
    sink_t s = { buf, size, 0, 0, 0 };

    format(&s, fmt, ap);

    if (size > 0) {
        buf[s.len < size ? s.len : size - 1] = 0;
    }

    return (int)s.len;
}

int snprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);

    return n;
}

int sprintf(char* buf, const char* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, (size_t)INT_MAX, fmt, ap);
    va_end(ap);

    return n;
}

int vfprintf(FILE* f, const char* fmt, va_list ap) {
    char chunk[256];
    sink_t s = { chunk, sizeof(chunk), 0, 0, 1 };

    (void)f;
    format(&s, fmt, ap);

    if (s.fill) {
        write(1, chunk, s.fill);
    }

    return (int)s.len;
}

int vprintf(const char* fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int fprintf(FILE* f, const char* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);

    return n;
}

int printf(const char* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);

    return n;
}

int fputc(int c, FILE* f) {
    char ch = (char)c;

    (void)f;
    write(1, &ch, 1);

    return (unsigned char)ch;
}

int fputs(const char* text, FILE* f) {
    (void)f;
    write(1, text, strlen(text));

    return 0;
}

size_t fwrite(const void* data, size_t size, size_t count, FILE* f) {
    (void)f;

    if (size && count) {
        write(1, data, size * count);
    }

    return count;
}

int fflush(FILE* f) {
    (void)f;
    return 0;
}

void abort(void) {
    fputs("abort() called\n", stderr);
    exit(134);

    for (;;) {
    }
}

void naq_assert_fail(const char* expr, const char* file, int line) {
    fprintf(stderr, "assertion failed: %s (%s:%d)\n", expr, file, line);
    abort();
}

/* ------------------------------------------------------------------ */
/* Clock                                                              */
/* ------------------------------------------------------------------ */

/* Days from 1970-01-01 to the given date (proleptic Gregorian). */
static long long days_from_date(long long y, unsigned m, unsigned d) {
    y -= m <= 2;

    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + (long long)doe - 719468;
}

static long long g_boot_ms;     /* wall clock, in ms since 1970, at tick 0 */
static int g_clock_known;

static long long now_ms(void) {
    unsigned long long ticks = get_ticks();   /* 1000 per second */

    if (!g_clock_known) {
        /*
         * clock_read() packs the battery clock as
         * year:month:day:hour:minute:second in 16+8+8+8+8+8 bits.
         */
        unsigned long long c = clock_read();
        long long seconds = 0;

        if (c) {
            long long days = days_from_date((long long)(c >> 40),
                                            (unsigned)((c >> 32) & 255),
                                            (unsigned)((c >> 24) & 255));

            seconds = days * 86400 +
                      (long long)((c >> 16) & 255) * 3600 +
                      (long long)((c >> 8) & 255) * 60 +
                      (long long)(c & 255);
        }

        g_boot_ms = seconds * 1000 - (long long)ticks;
        g_clock_known = 1;
    }

    return g_boot_ms + (long long)ticks;
}

int gettimeofday(struct timeval* tv, void* tz) {
    long long ms = now_ms();

    (void)tz;

    if (tv) {
        tv->tv_sec = (time_t)(ms / 1000);
        tv->tv_usec = (long)(ms % 1000) * 1000;
    }

    return 0;
}

int clock_gettime(clockid_t id, struct timespec* ts) {
    long long ms = (id == CLOCK_MONOTONIC) ? (long long)get_ticks() : now_ms();

    ts->tv_sec = (time_t)(ms / 1000);
    ts->tv_nsec = (long)(ms % 1000) * 1000000;

    return 0;
}

time_t time(time_t* out) {
    time_t t = (time_t)(now_ms() / 1000);

    if (out) {
        *out = t;
    }

    return t;
}

struct tm* gmtime_r(const time_t* t, struct tm* out) {
    long long days = *t / 86400;
    long long rem = *t % 86400;

    if (rem < 0) {
        rem += 86400;
        days--;
    }

    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)((rem % 3600) / 60);
    out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)(((days % 7) + 11) % 7);   /* 1970-01-01 was a Thursday */

    /* Date from day count (the inverse of days_from_date). */
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;

    y += m <= 2;

    out->tm_year = (int)(y - 1900);
    out->tm_mon = (int)m - 1;
    out->tm_mday = (int)d;
    out->tm_yday = (int)(days - days_from_date(y, 1, 1));
    out->tm_isdst = 0;
    out->tm_gmtoff = 0;
    out->tm_zone = "UTC";

    return out;
}

struct tm* localtime_r(const time_t* t, struct tm* out) {
    return gmtime_r(t, out);
}
