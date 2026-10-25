int uicopy_bad(int *in, int *out) {
    std::uninitialized_copy(in, in + 4, out);
    return out[9];
}
int uicopy_ok(int *in, int *out) {
    std::uninitialized_copy(in, in + 4, out);
    return out[0];
}
