int asm_unenc_bad(int n) {
    asm("nop");
    return n;
}

int asm_unenc_ok(int n) {
    return n;
}
