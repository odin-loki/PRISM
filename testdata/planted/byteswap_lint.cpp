void bswap_bad(unsigned n) {
    int a[4];
    a[std::byteswap(n)]=1;
}

void bswap_ok(unsigned n) {
    int a[4];
    unsigned i=std::byteswap((unsigned)n);
    if (i>=4) return;
    a[i]=1;
}
