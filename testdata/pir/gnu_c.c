// PIR task: GNU C. The Linux kernel's READ_ONCE (include/asm-generic/rwonce.h)
// names the type of its operand with typeof and strips the qualifiers with a
// _Generic selection; the kernel also uses statement expressions, __auto_type
// and the `asm` keyword. None of it compiles as strict -std=c17 (`expected ')'`
// at the first typeof); it compiles as gnu17, which is what the front end
// tries when the strict language level fails (docs/PIR.md "GNU C units").
// True and false variants: *_ok proves, *_bad is a counterexample.

#define __unqual_scalar_typeof(x)                                                       \
    typeof(_Generic((x), char: (char)0, unsigned char: (unsigned char)0,                \
                    signed char: (signed char)0, unsigned short: (unsigned short)0,     \
                    short: (short)0, unsigned int: (unsigned int)0, int: (int)0,        \
                    unsigned long: (unsigned long)0, long: (long)0,                     \
                    unsigned long long: (unsigned long long)0, long long: (long long)0, \
                    default: (x)))
#define __READ_ONCE(x) (*(const volatile __unqual_scalar_typeof(x) *)&(x))
#define READ_ONCE(x) __READ_ONCE(x)

// The divisor a build can choose with -DFIX_DIV=... (a unit's own flags,
// --compile-commands); without it the divisor is the argument.
#ifndef FIX_DIV
#define FIX_DIV n
#endif

int read_once_ok(int n) {
    int v = n;
    int r = READ_ONCE(v);
    return 100 / (r > 0 ? r : 1);
}

int read_once_bad(int n) {
    int v = n;
    int r = READ_ONCE(v);
    return 100 / r;
}

int stmt_expr_ok(int n) {
    int r = ({ int t = n & 7; t + 1; });
    return 100 / r;
}

int auto_type_bad(int n) {
    __auto_type d = n - 3;
    return 100 / d;
}

int asm_barrier_ok(int n) {
    int v = n | 1;
    asm volatile("" ::: "memory");
    return 100 / v;
}

int overflow_bad(int a) { return a + 1; }

int flag_div(int n) { return 100 / (FIX_DIV); }
