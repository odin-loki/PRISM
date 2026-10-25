int process_madvise(int pidfd, void *iov, unsigned long vlen, int advice,
                    unsigned flags);

void pmadvise_bad(void) {
    process_madvise(0,0,0,0,0);
}

void pmadvise_ok(void) {
    if (process_madvise(0,0,0,0,0)<0)
        return;
}
