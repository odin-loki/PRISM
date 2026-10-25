void mtx_lock(int);
void mtx_unlock(int);

int counter;

void atom_bad(void) {
    counter = counter + 1;
}

void atom_ok(void) {
    mtx_lock(0);
    counter = counter + 1;
    mtx_unlock(0);
}
