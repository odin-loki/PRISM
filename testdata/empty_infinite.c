void empty_inf_bad(void) {
    for (;;);
}

void empty_inf_while_bad(void) {
    while (1);
}

void empty_inf_ok(int n) {
    while (1) {
        if (n == 0)
            break;
        n--;
    }
}

void empty_inf_for_ok(int n) {
    for (;;) {
        if (n == 0)
            break;
        n--;
    }
}
