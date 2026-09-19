void fstream_bad(void) {
    std::ifstream in("x");
    char c; in.get(c);
}

void fstream_ok(void) {
    std::ifstream in("x");
    if (!in.is_open()) return;
    char c; in.get(c);
}
