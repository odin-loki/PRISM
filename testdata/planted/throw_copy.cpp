struct E {};

void throw_copy_bad(void) {
    try {
        throw E();
    } catch (E &e) {
        throw e;
    }
}

void throw_copy_ok(void) {
    try {
        throw E();
    } catch (E &e) {
        throw;
    }
}
