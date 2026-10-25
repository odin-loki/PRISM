void pmr_bad(int i) {
    std::pmr::vector<int> v;
    v[i]=1;
}

void pmr_ok(int i) {
    std::pmr::vector<int> v;
    if (i<0||(unsigned)i>=v.size()) return;
    v[i]=1;
}
