// PRISM conformance task memory/mem_struct_self_copy_true.c: expected true (memsafety)
// regression: refinement finding 6: a struct assignment that may be a
// self-assignment is llvm.memcpy(p, p), which LLVM defines (was MEM-OVERLAP)
struct quad { int v[4]; };

int mem_struct_self_copy_true(int c) {
    struct quad s = {{1, 2, 3, 4}}, t = {{5, 6, 7, 8}};
    struct quad *p = &s;
    struct quad *q = c ? &s : &t;
    *p = *q;
    return s.v[0];
}
