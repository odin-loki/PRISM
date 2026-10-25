int asm_nop(int x) {
    asm("nop");
    return x;
}

int asm_vol(int x) {
    __asm__ volatile ("nop");
    return x;
}
