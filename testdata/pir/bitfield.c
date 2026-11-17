/* bitfield loads and stores (shifts and masks on the containing integer) */
struct bf {
    unsigned a : 3;
    unsigned b : 5;
    int c : 4;
};

/* stores and loads of unsigned bitfields; s.b is 17, so the division is safe */
int bf_ok(void) {
    struct bf s = {0};
    s.a = 5;
    s.b = 17;
    return 100 / (int)s.b;
}

/* a signed bitfield holding -3 reads back negative (sign extension) */
int bf_signed_ok(void) {
    struct bf s = {0};
    s.a = 0;
    s.b = 1;
    s.c = -3;
    return s.c / 2;
}

/* s.a stored as 0 and divided by: the bitfield read is zero */
int bf_div_bad(void) {
    struct bf s = {0};
    s.a = 0;
    s.b = 1;
    return 10 / (int)s.a;
}
