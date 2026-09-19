struct pad {
    char a;
    int b;
};

void leak_bad(struct pad *out) {
    struct pad s;
    s.a = 1;
    memcpy(out, &s, sizeof(s));
}

void leak_ok(struct pad *out) {
    struct pad s;
    memset(&s, 0, sizeof(s));
    s.a = 1;
    memcpy(out, &s, sizeof(s));
}
