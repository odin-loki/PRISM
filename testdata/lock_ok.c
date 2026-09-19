void mtx_lock(int *m);
void mtx_unlock(int *m);

void lock_ab_ok(int *a, int *b) {
    mtx_lock(a);
    mtx_lock(b);
    mtx_unlock(b);
    mtx_unlock(a);
}

void lock_ab_ok2(int *a, int *b) {
    mtx_lock(a);
    mtx_lock(b);
    mtx_unlock(b);
    mtx_unlock(a);
}
