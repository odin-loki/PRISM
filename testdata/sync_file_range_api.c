int sync_file_range(void);

void sfr_bad(void) {
    sync_file_range();
}

void sfr_ok(void) {
    if (sync_file_range()!=-1)
        return;
}
