int readahead(int fd, long offset, unsigned long count);

void readahead_bad(void) {
    readahead(0, 0, 0);
}

void readahead_ok(void) {
    if (readahead(0, 0, 0) < 0)
        return;
}
