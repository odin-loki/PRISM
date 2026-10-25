void mtx_lock(int *m);
void mtx_unlock(int *m);

void double_lock_bad(int *m) {
    mtx_lock(m);
    mtx_lock(m);
    mtx_unlock(m);
}

void double_lock_ok(int *m) {
    mtx_lock(m);
    mtx_unlock(m);
    mtx_lock(m);
    mtx_unlock(m);
}
