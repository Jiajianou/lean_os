#include <fenv.h>

#if !defined(__x86_64__)
#error "this floating point environment is the x87's and SSE's, which is x86"
#endif

#define X87_DEFAULT_CONTROL 0x037Fu
#define SSE_DEFAULT_MXCSR   0x1F80u

/* MXCSR keeps the rounding mode thirteen bits up from where the x87 control
   word keeps it, and its exception masks six bits up from its flags. The two
   shifts below are that difference and nothing else. */
#define MXCSR_ROUND_SHIFT 3
#define MXCSR_MASK_SHIFT  7

static unsigned short x87_control(void) {
    unsigned short word;
    __asm__ volatile("fnstcw %0" : "=m"(word));
    return word;
}

static void x87_set_control(unsigned short word) {
    __asm__ volatile("fldcw %0" : : "m"(word));
}

static unsigned short x87_status(void) {
    unsigned short word;
    __asm__ volatile("fnstsw %0" : "=m"(word));
    return word;
}

static unsigned int sse_control(void) {
    unsigned int word;
    __asm__ volatile("stmxcsr %0" : "=m"(word));
    return word;
}

static void sse_set_control(unsigned int word) {
    __asm__ volatile("ldmxcsr %0" : : "m"(word));
}

/* Clearing an x87 exception flag needs the whole environment reloaded -
   there is no instruction that clears one - so this reads it out, edits the
   status word and puts it back. */
static void x87_clear_flags(int excepts) {
    struct {
        unsigned int control_word;
        unsigned int status_word;
        unsigned int tag_word;
        unsigned int instruction_pointer;
        unsigned int instruction_selector;
        unsigned int operand_pointer;
        unsigned int operand_selector;
    } environment;
    __asm__ volatile("fnstenv %0" : "=m"(environment));
    environment.status_word &= ~(unsigned int)(excepts & FE_ALL_EXCEPT);
    __asm__ volatile("fldenv %0" : : "m"(environment));
}

int feclearexcept(int excepts) {
    excepts &= FE_ALL_EXCEPT;
    x87_clear_flags(excepts);
    sse_set_control(sse_control() & ~(unsigned int)excepts);
    return 0;
}

int fetestexcept(int excepts) {
    excepts &= FE_ALL_EXCEPT;
    unsigned int raised = (unsigned int)x87_status() | sse_control();
    return (int)(raised & (unsigned int)excepts);
}

int feraiseexcept(int excepts) {
    /* Setting the flag rather than provoking the operation. A program that
       asked for the flag wants the flag; one that wanted a trap would have
       unmasked the exception, and this does not change the masks. */
    unsigned int mxcsr = sse_control();
    sse_set_control(mxcsr | (unsigned int)(excepts & FE_ALL_EXCEPT));
    return 0;
}

int fegetexceptflag(fexcept_t *out, int excepts) {
    if (!out) {
        return 1;
    }
    *out = (fexcept_t)(fetestexcept(excepts) & FE_ALL_EXCEPT);
    return 0;
}

int fesetexceptflag(const fexcept_t *flags, int excepts) {
    if (!flags) {
        return 1;
    }
    int wanted = (int)*flags & excepts & FE_ALL_EXCEPT;
    feclearexcept(excepts);
    return feraiseexcept(wanted);
}

int fegetround(void) {
    return (int)(x87_control() & FE_TOWARDZERO);
}

int fesetround(int mode) {
    if (mode != FE_TONEAREST && mode != FE_DOWNWARD && mode != FE_UPWARD &&
        mode != FE_TOWARDZERO) {
        return 1;
    }
    unsigned short control = x87_control();
    control = (unsigned short)((control & ~(unsigned short)FE_TOWARDZERO) |
                               (unsigned short)mode);
    x87_set_control(control);

    unsigned int mxcsr = sse_control();
    mxcsr &= ~((unsigned int)FE_TOWARDZERO << MXCSR_ROUND_SHIFT);
    mxcsr |= (unsigned int)mode << MXCSR_ROUND_SHIFT;
    sse_set_control(mxcsr);
    return 0;
}

int fegetenv(fenv_t *out) {
    if (!out) {
        return 1;
    }
    out->control_word = x87_control();
    out->status_word = x87_status();
    out->mxcsr = sse_control();
    return 0;
}

int fesetenv(const fenv_t *env) {
    if (env == FE_DFL_ENV) {
        x87_set_control(X87_DEFAULT_CONTROL);
        x87_clear_flags(FE_ALL_EXCEPT);
        sse_set_control(SSE_DEFAULT_MXCSR);
        return 0;
    }
    if (!env) {
        return 1;
    }
    x87_set_control(env->control_word);
    x87_clear_flags(FE_ALL_EXCEPT);
    sse_set_control(env->mxcsr);
    return 0;
}

int feholdexcept(fenv_t *out) {
    if (fegetenv(out) != 0) {
        return 1;
    }
    feclearexcept(FE_ALL_EXCEPT);
    /* Non-stop mode: every exception masked, so the block this brackets
       cannot trap however it goes. */
    x87_set_control((unsigned short)(x87_control() | FE_ALL_EXCEPT));
    sse_set_control(sse_control() |
                    ((unsigned int)FE_ALL_EXCEPT << MXCSR_MASK_SHIFT));
    return 0;
}

int feupdateenv(const fenv_t *env) {
    int raised = fetestexcept(FE_ALL_EXCEPT);
    if (fesetenv(env) != 0) {
        return 1;
    }
    return feraiseexcept(raised);
}
