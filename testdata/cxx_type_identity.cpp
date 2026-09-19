int typeid_unenc_bad(int n) {
    std::type_identity<int>();
    return n;
}

int typeid_unenc_ok(int n) {
    return n;
}
