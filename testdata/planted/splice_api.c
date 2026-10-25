long splice(int fd_in, void *off_in, int fd_out, void *off_out,
            unsigned long len, unsigned flags);

void splice_bad(void) {
    splice(0,0,0,0,0,0);
}

void splice_ok(void) {
    if (splice(0,0,0,0,0,0)<0)
        return;
}
