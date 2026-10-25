int embed_bad(int n) {
#embed user_path
    return n;
}

int embed_ok(int n) {
    unsigned char buf[] = { #embed "x" };
    return (int)sizeof(buf);
}
