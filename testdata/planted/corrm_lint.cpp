struct A { int x; };
struct B { int x; };
int corrm_bad(int *p) {
    constexpr bool b = std::is_corresponding_member(&A::x, &B::x);
    return p[9];
}
int corrm_ok(int *p) {
    constexpr bool b = std::is_corresponding_member(&A::x, &B::x);
    return p[0];
}
