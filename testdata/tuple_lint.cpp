void tuple_bad(void) {
    std::tuple<int,int> t;
    (void)std::get<1>(t);
}

void tuple_ok(void) {
    std::tuple<int,int> t;
    auto [a,b]=t;
    (void)a;
    (void)b;
}
