typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) (*(type *)(ap))
#define va_end(ap) __builtin_va_end(ap)

int va_arg_bad(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int x = va_arg(ap, int);
    va_end(ap);
    return x;
}

int va_arg_ok(int n) {
    return n;
}
