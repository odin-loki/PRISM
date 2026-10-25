void valarray_bad(int i) {
    std::valarray<int> v(4);
    v[i] = 1;
}

void valarray_ok(int i) {
    std::valarray<int> v(4);
    if (i < 0 || (unsigned)i >= v.size()) return;
    v[i] = 1;
}
