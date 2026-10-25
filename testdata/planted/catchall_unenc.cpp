int catchall_unenc_bad(int n) {
    try {
        throw 1;
    } catch (...) {
    }
    return n;
}

int catchall_unenc_ok(int n) {
    return n;
}
