int reflection_bad(int n) {
    std::meta::info t = ^^int;
    define_aggregate(t);
    return n;
}

int reflection_ok(int n) {
    std::meta::info t = ^^int;
    return n;
}
