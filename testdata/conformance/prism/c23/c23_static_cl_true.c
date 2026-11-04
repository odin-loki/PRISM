// PRISM conformance task c23/c23_static_cl_true.c: expected true (memsafety)
// C23 N3038: a static compound literal outlives the call, so returning it is safe.
// clang (through 23.1.4) rejects this; PRISM's pir stage rewrites it (stage.cpp).
static int* table(void) { return (static int[3]){1, 2, 3}; }
int c23_static_cl_true(int s) { return table()[s & 1]; }
