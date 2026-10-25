int try_ok(int x) {
    try {
        return x;
    } catch (...) {
        return 0;
    }
}
