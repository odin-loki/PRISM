int nftw(const char *dirpath, void *fn, int nopenfd, int flags);

void nftw_bad(void) {
    nftw(".",0,0,0);
}

void nftw_ok(void) {
    if (nftw(".",0,0,0)!=0)
        return;
}
