int any_cast_bad(void) {
    any a;
    return any_cast<int>(a);
}

int any_cast_ok(void) {
    any a;
    int *p;
    p = any_cast<int>(&a);
    if (p)
        return *p;
    return 0;
}
