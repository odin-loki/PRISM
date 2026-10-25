mdspan mdspan_bad(void) {
    int local[4];
    return mdspan<int>(local);
}

int mdspan_ok(mdspan s) {
    return s.size();
}
