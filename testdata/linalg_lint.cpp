int linalg_bad(int i, int j) {
    std::linalg::matrix<float> m;
    return m(i, j);
}

int linalg_ok(int i, int j) {
    std::linalg::matrix<float> m;
    if (i < m.extent(0) && j < m.extent(1))
        return m(i, j);
    return 0;
}
