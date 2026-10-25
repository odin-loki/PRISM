void *reallocarray(void *optr, unsigned nmemb, unsigned size);

void reallocarr_bad(void) {
    reallocarray(0, 2, 8);
}

void reallocarr_ok(void) {
    if (reallocarray(0, 2, 8) == 0)
        return;
}
