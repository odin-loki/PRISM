struct BoolBad { operator bool() { return 1; } };

void explicit_bool_bad(void) {
    BoolBad f;
    (void)f;
}

struct BoolOk { explicit operator bool() { return 1; } };

void explicit_bool_ok(void) {
    BoolOk f;
    (void)f;
}
