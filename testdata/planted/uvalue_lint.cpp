int uvalue_bad(int *out) {
    std::uninitialized_value_construct(out, out + 4);
    return out[9];
}
int uvalue_ok(int *out) {
    std::uninitialized_value_construct(out, out + 4);
    return out[0];
}
