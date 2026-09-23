// PRISM conformance task kindmem/kindmem_struct_true.c: expected true (memsafety)
// k-induction with memory: a loop appends to a fixed buffer inside a struct; only part of the buffer is ever written
struct kindmem_buf { unsigned len; int data[8]; };
int kindmem_struct_true(int n, int i) {
    struct kindmem_buf s = {0, {0}};
    if (n < 20 || n > 1000) return 0;
    for (int k = 0; k < n; k++) { s.data[s.len & 3u] = k; s.len++; }
    return s.data[i & 7];
}
