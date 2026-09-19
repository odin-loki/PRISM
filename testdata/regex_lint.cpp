void regex_bad(void) {
    const char *pat;
    pat = "a+";
    std::regex r(pat);
}

void regex_ok(void) {
    std::regex r("a+");
}
