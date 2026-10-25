// PIR tasks: C++ new/delete through the operator new/delete models.
int new_delete_ok(int v) {
    int* p = new int(v);
    int r = *p > 0;
    delete p;
    return r;
}

int new_array_ok(int i) {
    int* a = new int[4]();
    int r = a[i & 3];
    delete[] a;
    return r;
}

int mismatch_bad(void) {
    int* a = new int[4]();
    delete a;  // new[] released with delete
    return 0;
}

int new_oob_bad(int i) {
    int* a = new int[4]();
    int r = (i >= 0 && i <= 4) ? a[i] : 0;  // a[4]
    delete[] a;
    return r;
}

int delete_twice_bad(int c) {
    int* p = new int(1);
    delete p;
    if (c) delete p;
    return 0;
}
