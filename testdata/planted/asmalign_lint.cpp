int asmalign_bad(int *p) { return std::assume_aligned<16>(p)[9]; }
int asmalign_ok(int *p) {
    auto *q = std::assume_aligned<16>(p);
    return q[0];
}
