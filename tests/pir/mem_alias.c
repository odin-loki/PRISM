/* PIR tasks: effective types (C11 6.5p6-7), checked only with
 * --strict-aliasing (opt-in; without it pun_bad is PROVED and the finding
 * says strict aliasing was off). */
int pun_bad(void) {
    int x = 1;
    short *s = (short *)&x;
    *s = 2; /* int object written through a short lvalue */
    return x;
}

int char_ok(void) {
    int x = 1;
    unsigned char *c = (unsigned char *)&x;
    c[0] = 2; /* character types may alias anything */
    return x;
}
