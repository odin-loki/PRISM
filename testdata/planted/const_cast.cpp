int cxx_cv_write_bad(void) {
    const int x = 1;
    *const_cast<int*>(&x) = 2;
    return x;
}

int cxx_cv_write_ok(void) {
    int x = 1;
    *const_cast<int*>(&x) = 2;
    return x;
}
