#ifndef NAQ_LIBC_FENV_H
#define NAQ_LIBC_FENV_H
/* Rounding is always round-to-nearest here. */
#define FE_TONEAREST  0
#define FE_DOWNWARD   0x400
#define FE_UPWARD     0x800
#define FE_TOWARDZERO 0xc00
#endif
