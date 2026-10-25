generator generator_bad(void) {
    int local[4];
    co_yield local[0];
}

generator generator_ok(void) {
    co_yield 1;
}
