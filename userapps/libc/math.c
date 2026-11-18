/*
 * NAQ OS small C library: <math.h>.
 *
 * The work is done by the processor's floating point unit (x87), which
 * has instructions for sine, logarithm, arctangent and so on and computes
 * with 64 bits of precision, 11 more than a double holds. Results are
 * therefore accurate to the last digit or very nearly so.
 */

#include <math.h>

typedef long double ld;

#define LN2      0.693147180559945309417232121458176568L
#define LN2_HIGH 0.693147180559945286226764L            /* ln 2 as a double */
#define LN2_LOW  2.319046813846299558417771e-17L        /* the rest */
#define LOG2_E   1.442695040888963407359924681001892137L
#define LOG10_2  0.301029995663981195213738894724493027L
#define TWO_PI   6.283185307179586476925286766559005768L

/* ---- single instructions ---- */

static inline ld x_sqrt(ld x) {
    __asm__("fsqrt" : "+t"(x));
    return x;
}

static inline ld x_rint(ld x) {
    __asm__("frndint" : "+t"(x));
    return x;
}

/* y * log2(x) */
static inline ld x_ylog2(ld x, ld y) {
    ld r;

    __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

/* y * log2(x + 1), for |x| < 0.29 */
static inline ld x_ylog2p1(ld x, ld y) {
    ld r;

    __asm__("fyl2xp1" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

/* 2^x - 1, for |x| <= 1 */
static inline ld x_2xm1(ld x) {
    __asm__("f2xm1" : "+t"(x));
    return x;
}

/* x * 2^n (n a whole number) */
static inline ld x_scale(ld x, ld n) {
    __asm__("fscale" : "+t"(x) : "u"(n));
    return x;
}

/* atan(y / x), in the right quadrant */
static inline ld x_atan2(ld y, ld x) {
    ld r;

    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

static inline ld x_fmod(ld x, ld y) {
    unsigned short status;

    do {
        __asm__("fprem\n\tfnstsw %%ax" : "+t"(x), "=a"(status) : "u"(y));
    } while (status & 0x400);

    return x;
}

static inline ld x_abs(ld x) {
    return __builtin_fabsl(x);
}

/* ---- basics ---- */

double fabs(double x) {
    return __builtin_fabs(x);
}

double copysign(double x, double y) {
    return __builtin_copysign(x, y);
}

double sqrt(double x) {
    double r;

    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

/* 2^52: at or above this every double is already a whole number. */
#define WHOLE 4503599627370496.0

double trunc(double x) {
    if (!(__builtin_fabs(x) < WHOLE)) {
        return x;
    }

    return __builtin_copysign((double)(long long)x, x);
}

double floor(double x) {
    if (!(__builtin_fabs(x) < WHOLE)) {
        return x;
    }

    double t = (double)(long long)x;

    if (t > x) {
        t -= 1.0;
    }

    return __builtin_copysign(t, x);
}

double ceil(double x) {
    if (!(__builtin_fabs(x) < WHOLE)) {
        return x;
    }

    double t = (double)(long long)x;

    if (t < x) {
        t += 1.0;
    }

    return __builtin_copysign(t, x);
}

double round(double x) {
    if (!(__builtin_fabs(x) < WHOLE)) {
        return x;
    }

    double t = (double)(long long)x;

    if (__builtin_fabs(x - t) >= 0.5) {
        t += __builtin_copysign(1.0, x);
    }

    return __builtin_copysign(t, x);
}

double rint(double x) {
    if (!(__builtin_fabs(x) < WHOLE)) {
        return x;
    }

    return __builtin_copysign((double)x_rint(x), x);
}

long lrint(double x) {
    long r;

    __asm__("cvtsd2si %1, %0" : "=r"(r) : "x"(x));
    return r;
}

double fmod(double x, double y) {
    if (x != x || y != y || __builtin_isinf(x) || y == 0) {
        return __builtin_nan("");
    }

    if (__builtin_isinf(y)) {
        return x;
    }

    return __builtin_copysign((double)x_fmod(x, y), x);
}

double modf(double x, double* ip) {
    double whole = trunc(x);

    *ip = whole;

    if (__builtin_isinf(x)) {
        return __builtin_copysign(0.0, x);
    }

    return __builtin_copysign(x - whole, x);
}

double fmin(double x, double y) {
    if (x != x) return y;
    if (y != y) return x;
    if (x == 0 && y == 0) return __builtin_signbit(x) ? x : y;
    return x < y ? x : y;
}

double fmax(double x, double y) {
    if (x != x) return y;
    if (y != y) return x;
    if (x == 0 && y == 0) return __builtin_signbit(x) ? y : x;
    return x > y ? x : y;
}

double scalbn(double x, int n) {
    return (double)x_scale(x, n);
}

double ldexp(double x, int n) {
    return (double)x_scale(x, n);
}

double frexp(double x, int* e) {
    union { double d; unsigned long long u; } v = { x };
    int biased = (int)((v.u >> 52) & 0x7ff);

    if (biased == 0x7ff || x == 0) {
        *e = 0;
        return x;
    }

    if (biased == 0) {
        /* tiny (denormal): scale up first */
        v.d = x * 18014398509481984.0;   /* 2^54 */
        biased = (int)((v.u >> 52) & 0x7ff) - 54;
    }

    *e = biased - 1022;
    v.u = (v.u & 0x800fffffffffffffull) | (1022ull << 52);

    return v.d;
}

double hypot(double x, double y) {
    if (__builtin_isinf(x) || __builtin_isinf(y)) {
        return __builtin_inf();
    }

    ld a = x;
    ld b = y;

    return (double)x_sqrt(a * a + b * b);
}

/* ---- exponentials and logarithms ---- */

/* 2^t, with the extended range and precision of long double */
static ld exp2_ld(ld t) {
    if (t != t) {
        return t;
    }

    if (t > 16400) {
        return __builtin_infl();
    }

    if (t < -16500) {
        return 0;
    }

    ld n = x_rint(t);

    return x_scale(x_2xm1(t - n) + 1, n);
}

/* e^x */
static ld exp_ld(ld x) {
    if (x != x) {
        return x;
    }

    if (x > 11400) {
        return __builtin_infl();
    }

    if (x < -11400) {
        return 0;
    }

    /*
     * x = n * ln2 + f. Taking n * ln2 off in two parts keeps f accurate,
     * so the result is good to the last bit of a double.
     */
    ld n = x_rint(x * LOG2_E);
    ld f = (x - n * LN2_HIGH) - n * LN2_LOW;

    return x_scale(x_2xm1(f * LOG2_E) + 1, n);
}

static ld log_ld(ld x) {
    return x_ylog2(x, LN2);
}

static ld log1p_ld(ld x) {
    if (x_abs(x) < 0.29L) {
        return x_ylog2p1(x, LN2);
    }

    return x_ylog2(1 + x, LN2);
}

static ld expm1_ld(ld x) {
    if (x_abs(x) < 0.5L) {
        return x_2xm1(x * LOG2_E);
    }

    return exp_ld(x) - 1;
}

double exp(double x) {
    return (double)exp_ld(x);
}

double expm1(double x) {
    if (x == 0) {
        return x;   /* keeps -0 */
    }

    return (double)expm1_ld(x);
}

double log(double x) {
    if (x < 0) {
        return __builtin_nan("");
    }

    return (double)x_ylog2(x, LN2);
}

double log2(double x) {
    if (x < 0) {
        return __builtin_nan("");
    }

    return (double)x_ylog2(x, 1);
}

double log10(double x) {
    if (x < 0) {
        return __builtin_nan("");
    }

    return (double)x_ylog2(x, LOG10_2);
}

double log1p(double x) {
    if (x < -1) {
        return __builtin_nan("");
    }

    if (x == 0) {
        return x;
    }

    return (double)log1p_ld(x);
}

double cbrt(double x) {
    if (x == 0 || x != x || __builtin_isinf(x)) {
        return x;
    }

    ld a = x_abs(x);
    ld r = exp2_ld(x_ylog2(a, 1) / 3);

    /* one correcting step, so that cbrt(27) is exactly 3 */
    r = r - (r * r * r - a) / (3 * r * r);

    return (double)(x < 0 ? -r : r);
}

double pow(double x, double y) {
    if (y == 0 || x == 1) {
        return 1.0;
    }

    if (x != x || y != y) {
        return __builtin_nan("");
    }

    int y_whole = __builtin_fabs(y) >= WHOLE * 2 || y == (double)(long long)y;
    int y_odd = y_whole && __builtin_fabs(y) < WHOLE * 2 && ((long long)y & 1);

    if (__builtin_isinf(y)) {
        double a = __builtin_fabs(x);

        if (a == 1) {
            return 1.0;
        }

        return ((a > 1) == (y > 0)) ? __builtin_inf() : 0.0;
    }

    if (x == 0 || __builtin_isinf(x)) {
        /* result is zero or infinity; its sign follows x only for odd y */
        int big = (x == 0) ? (y < 0) : (y > 0);
        double r = big ? __builtin_inf() : 0.0;

        return (y_odd && __builtin_signbit(x)) ? -r : r;
    }

    if (x < 0 && !y_whole) {
        return __builtin_nan("");
    }

    if (y == 0.5 && x > 0) {
        return sqrt(x);   /* exactly rounded, unlike the general path */
    }

    ld a = x_abs(x);
    ld r;

    if (y_whole && __builtin_fabs(y) <= 1024) {
        /* whole powers by repeated squaring: exact whenever the answer is */
        long long n = (long long)y;
        unsigned long long k = n < 0 ? 0ull - (unsigned long long)n : (unsigned long long)n;
        ld base = a;

        r = 1;

        while (k) {
            if (k & 1) {
                r *= base;
            }

            base *= base;
            k >>= 1;
        }

        if (n < 0) {
            r = 1 / r;
        }
    } else {
        r = exp2_ld(x_ylog2(a, y));
    }

    return (double)((x < 0 && y_odd) ? -r : r);
}

/* ---- trigonometry ---- */

/* The x87 only accepts angles below 2^63; fold larger ones first. */
static ld fold_angle(ld x) {
    if (x_abs(x) >= 4611686018427387904.0L) {
        x = x_fmod(x, TWO_PI);
    }

    return x;
}

double sin(double x) {
    if (x != x || __builtin_isinf(x)) {
        return __builtin_nan("");
    }

    if (x == 0) {
        return x;
    }

    ld v = fold_angle(x);

    __asm__("fsin" : "+t"(v));
    return (double)v;
}

double cos(double x) {
    if (x != x || __builtin_isinf(x)) {
        return __builtin_nan("");
    }

    ld v = fold_angle(x);

    __asm__("fcos" : "+t"(v));
    return (double)v;
}

double tan(double x) {
    if (x != x || __builtin_isinf(x)) {
        return __builtin_nan("");
    }

    if (x == 0) {
        return x;
    }

    ld s = fold_angle(x);
    ld c = s;

    __asm__("fsin" : "+t"(s));
    __asm__("fcos" : "+t"(c));

    return (double)(s / c);
}

double atan2(double y, double x) {
    return (double)x_atan2(y, x);
}

double atan(double x) {
    return (double)x_atan2(x, 1);
}

double asin(double x) {
    if (x == 0) {
        return x;
    }

    ld v = x;

    return (double)x_atan2(v, x_sqrt((1 - v) * (1 + v)));
}

double acos(double x) {
    ld v = x;

    return (double)x_atan2(x_sqrt((1 - v) * (1 + v)), v);
}

/* ---- hyperbolic ---- */

double sinh(double x) {
    if (x == 0 || x != x) {
        return x;
    }

    ld a = x_abs(x);
    ld r;

    if (a < 0.5L) {
        ld e = expm1_ld(a);

        r = 0.5L * (e + e / (e + 1));
    } else {
        ld e = exp_ld(a);

        r = 0.5L * (e - 1 / e);
    }

    return (double)(x < 0 ? -r : r);
}

double cosh(double x) {
    ld e = exp_ld(x_abs(x));

    return (double)(0.5L * (e + 1 / e));
}

double tanh(double x) {
    if (x == 0 || x != x) {
        return x;
    }

    ld a = x_abs(x);
    ld r = 1;

    if (a < 40) {
        ld e = expm1_ld(2 * a);

        r = e / (e + 2);
    }

    return (double)(x < 0 ? -r : r);
}

double asinh(double x) {
    if (x == 0 || x != x || __builtin_isinf(x)) {
        return x;
    }

    ld a = x_abs(x);
    ld r = log1p_ld(a + a * a / (1 + x_sqrt(1 + a * a)));

    return (double)(x < 0 ? -r : r);
}

double acosh(double x) {
    if (x < 1) {
        return __builtin_nan("");
    }

    ld v = x;

    return (double)log_ld(v + x_sqrt(v * v - 1));
}

double atanh(double x) {
    if (x == 0 || x != x) {
        return x;
    }

    ld a = x_abs(x);

    if (a > 1) {
        return __builtin_nan("");
    }

    ld r = 0.5L * log1p_ld(2 * a / (1 - a));

    return (double)(x < 0 ? -r : r);
}
