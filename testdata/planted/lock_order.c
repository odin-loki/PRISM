void mtx_lock(int *m);
void mtx_unlock(int *m);

void lock_ab(int *a, int *b) {
    mtx_lock(a);
    mtx_lock(b);
    mtx_unlock(b);
    mtx_unlock(a);
}

void lock_ba(int *a, int *b) {
    mtx_lock(b);
    mtx_lock(a);
    mtx_unlock(a);
    mtx_unlock(b);
}
