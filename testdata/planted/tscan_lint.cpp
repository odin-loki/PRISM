int tscan_bad(int *in, int *out) {
    std::transform_inclusive_scan(in, in + 4, out, std::plus<>{});
    return out[9];
}
int tscan_ok(int *in, int *out) {
    std::transform_inclusive_scan(in, in + 4, out, std::plus<>{});
    return out[0];
}
