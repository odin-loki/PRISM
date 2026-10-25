int inscan_bad(int *in, int *out) {
    std::inclusive_scan(in, in + 4, out);
    return out[9];
}
int inscan_ok(int *in, int *out) {
    std::inclusive_scan(in, in + 4, out);
    return out[0];
}
