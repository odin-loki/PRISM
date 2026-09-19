int pdfork(int *fdp, int flags);

void pdfork_bad(void) {
    pdfork(0, 0);
}

void pdfork_ok(void) {
    if (pdfork(0, 0) < 0)
        return;
}
