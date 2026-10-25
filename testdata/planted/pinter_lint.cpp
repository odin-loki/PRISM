struct S { int m; };
int pinter_bad(int *p) {
    constexpr bool b = std::is_pointer_interconvertible_with_class(&S::m);
    return p[9];
}
int pinter_ok(int *p) {
    constexpr bool b = std::is_pointer_interconvertible_with_class(&S::m);
    return p[0];
}
