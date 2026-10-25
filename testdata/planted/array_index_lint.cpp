void arr_idx_bad(int i) {
    std::array<int,4> a;
    a[i]=1;
}
void arr_idx_ok(int i) {
    std::array<int,4> a;
    if (i<0 || (unsigned)i>=a.size()) return;
    a[i]=1;
}
