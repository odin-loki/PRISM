enum E { A, B };
void to_under_bad(E e) {
    auto x = std::to_underlying(e);
    (void)x;
}

void to_under_ok(E e) {
    if (e != A && e != B) return;
    auto x = std::to_underlying(e);
    (void)x;
}
