// PRISM conformance task memory/mem_new_delete_true.cpp: expected true (memsafety)
int mem_new_delete_true(int i) {
    int *a = new int[4]();
    int r = a[i & 3];
    delete[] a;
    return r;
}
