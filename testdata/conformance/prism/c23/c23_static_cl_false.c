// PRISM conformance task c23/c23_static_cl_false.c: expected false (memsafety)
static int* table(void) { return (static int[3]){1, 2, 3}; }
int c23_static_cl_false(int s) { return s >= 0 && s < 4 ? table()[s] : 0; }
