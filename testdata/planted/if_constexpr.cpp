int ifcx_unenc_bad(int n) {
    if constexpr (n)
        return 1;
    return 0;
}

int ifcx_ok(int n) {
    if (n)
        return 1;
    return 0;
}
