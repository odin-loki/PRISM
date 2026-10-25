int process_mrelease(int pidfd, unsigned flags);

void pmrel_bad(void) {
    process_mrelease(0, 0);
}

void pmrel_ok(void) {
    if (process_mrelease(0, 0)!=-1)
        return;
}
