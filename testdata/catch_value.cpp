struct E {};

void catch_val_bad(void) {
    try {
        throw E();
    } catch (E e) {
        (void)e;
    }
}

void catch_val_ok(void) {
    try {
        throw E();
    } catch (E &e) {
        (void)e;
    }
}

void catch_ptr_ok(void) {
    try {
        throw E();
    } catch (E *e) {
        (void)e;
    }
}
