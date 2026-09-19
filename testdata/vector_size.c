int vecsize_unenc_bad(int n) {
    int x __attribute__((vector_size(16)));
    return n;
}

int vecsize_ok(int n) {
    return n;
}
