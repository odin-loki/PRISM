int uifill_bad(int *out) {
    std::uninitialized_fill(out, out + 4, 0);
    return out[9];
}
int uifill_ok(int *out) {
    std::uninitialized_fill(out, out + 4, 0);
    return out[0];
}
