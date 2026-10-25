void catch_all_bad(void) {
    try {
        throw 1;
    } catch (...) {
    }
}

void catch_all_ok(void) {
    try {
        throw 1;
    } catch (...) {
        throw;
    }
}
