auto dangle_bad(void) {
    int x = 1;
    return [&]{ return x; };
}

auto dangle_ok(void) {
    int x = 1;
    return [=]{ return x; };
}

int dangle_ref_ok(void) {
    int x = 1;
    auto f = [&]{ return x; };
    return f();
}
