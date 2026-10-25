int cap_getmode(unsigned int *modep);

void capmode_bad(void) {
    cap_getmode(0);
}

void capmode_ok(void) {
    if (cap_getmode(0) != 0)
        return;
}
