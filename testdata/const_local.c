int const_local_bad(int n) {
    const int x = n;
    return x;
}

int const_param_ok(const int x) {
    return x;
}

int const_for_bad(int n) {
    int s;
    s = 0;
    for (const int bound = n; s < bound; s++)
        ;
    return s;
}

int const_cast_ok(int x) {
    return (const int)x;
}
