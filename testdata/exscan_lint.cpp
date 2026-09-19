int exscan_bad(int *in, int *out) {
    std::exclusive_scan(in, in + 4, out, 0);
    return out[9];
}
int exscan_ok(int *in, int *out) {
    std::exclusive_scan(in, in + 4, out, 0);
    return out[0];
}
