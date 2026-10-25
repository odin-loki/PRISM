int move_pages(int pid, unsigned long count, void **pages, const int *nodes, int *status, int flags);

void mvpg_bad(void) {
    move_pages(0,0,0,0,0,0);
}

void mvpg_ok(void) {
    if (move_pages(0,0,0,0,0,0)!=-1)
        return;
}
