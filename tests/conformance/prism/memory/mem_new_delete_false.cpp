// PRISM conformance task memory/mem_new_delete_false.cpp: expected false (memsafety)
int mem_new_delete_false(int i) {
    int *a = new int[4]();
    int r = a[i & 3];
    delete a;
    return r;
}
