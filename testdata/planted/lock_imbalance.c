void mtx_lock(int *m);
void mtx_unlock(int *m);

int lock_imbalance(int *m, int x) {
    mtx_lock(m);
    if (x < 0) {
        return -1;
    }
    mtx_unlock(m);
    return 0;
}
