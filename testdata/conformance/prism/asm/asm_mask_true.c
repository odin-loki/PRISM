/* PRISM conformance task asm/asm_mask_true.c: expected true (no-oob) */
int asm_mask_true(int x) {
    int table[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int r;
    // prism: asm ensures r >= 0 && r <= 7
    __asm__("movl %1, %0\n\tandl $7, %0" : "=r"(r) : "r"(x));
    return table[r];
}
