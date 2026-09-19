int jmp_bad(int x) {
    jmp_buf env;
    if (setjmp(env) == 0)
        longjmp(env, 1);
    return x;
}

int jmp_ok(int x) {
    if (x < 0)
        return 0;
    return x;
}
