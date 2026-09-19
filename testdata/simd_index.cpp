int simd_index_bad(unsigned n) {
    std::experimental::simd<float> x;
    return x[n];
}

int simd_index_ok(unsigned n) {
    std::experimental::simd<float> x;
    if (n < x.size())
        return x[n];
    return 0;
}
