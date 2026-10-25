void *reallocf(void *p, unsigned n);

void reallocf_bad(void) {
    reallocf(0, 8);
}

void reallocf_ok(void) {
    if (reallocf(0, 8) == 0)
        return;
}
