/* Dafny-shaped contract. A proof under requires is PROVED-ASSUMING, never PROVED. */
int inc(int x) {
    // requires: x < 100
    // ensures: result == x+1
    return x + 1;
}

/* Same contract, wrong body — rapid should FAILED. */
int not_inc(int x) {
    // requires: x < 100
    // ensures: result == x+1
    return x;
}

/* Weak ensures: mutating + to - still yields result >= 0 on the domain. */
int loose_add(int x) {
    // requires: x >= 1
    // requires: x < 50
    // ensures: result >= 0
    return x + 1;
}
