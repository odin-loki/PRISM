void synccas_unenc_bad(void) {
    __sync_bool_compare_and_swap();
}

int synccas_unenc_ok(int n) {
    return n;
}
