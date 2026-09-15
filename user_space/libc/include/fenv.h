#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The floating point environment, which on x86-64 lives in TWO places: the
   x87 control and status words and MXCSR. A program that sets a rounding
   mode and gets it applied to its SSE arithmetic but not its long double
   arithmetic - or the other way round - has been told something false, so
   every function here writes both and reads both. */

#define FE_INVALID   0x01
#define FE_DENORMAL  0x02
#define FE_DIVBYZERO 0x04
#define FE_OVERFLOW  0x08
#define FE_UNDERFLOW 0x10
#define FE_INEXACT   0x20

#define FE_ALL_EXCEPT                                                         \
    (FE_INVALID | FE_DENORMAL | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW |   \
     FE_INEXACT)

#define FE_TONEAREST  0x0000
#define FE_DOWNWARD   0x0400
#define FE_UPWARD     0x0800
#define FE_TOWARDZERO 0x0C00

typedef struct {
    unsigned short control_word;
    unsigned short status_word;
    unsigned int mxcsr;
} fenv_t;

typedef unsigned short fexcept_t;

#define FE_DFL_ENV ((const fenv_t *)-1)

int feclearexcept(int excepts);
int fetestexcept(int excepts);
int feraiseexcept(int excepts);
int fegetexceptflag(fexcept_t *out, int excepts);
int fesetexceptflag(const fexcept_t *flags, int excepts);

int fegetround(void);
int fesetround(int mode);

int fegetenv(fenv_t *out);
int fesetenv(const fenv_t *env);
int feholdexcept(fenv_t *out);
int feupdateenv(const fenv_t *env);

#ifdef __cplusplus
}
#endif
