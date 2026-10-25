void text_enc_bad(void) {
    const char *name;
    name = "UTF-8";
    std::text_encoding te(name);
}

void text_enc_ok(void) {
    std::text_encoding te("UTF-8");
    (void)text_encoding::utf8();
}
