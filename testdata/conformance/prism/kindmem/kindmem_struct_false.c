// PRISM conformance task kindmem/kindmem_struct_false.c: expected false (memsafety)
// k-induction with memory: a loop appends to a fixed buffer inside a struct; only part of the buffer is ever written
struct kindmem_buf { unsigned len; int data[8]; };
int kindmem_struct_false(int n, int i) {
    struct kindmem_buf s;
    s.len = 0;
    if (n < 20 || n > 1000) return 0;
    for (int k = 0; k < n; k++) { s.data[s.len & 3u] = k; s.len++; }
    return s.data[i & 7]; /* data[4..7] never written */
}
