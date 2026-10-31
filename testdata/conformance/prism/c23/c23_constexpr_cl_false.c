// PRISM conformance task c23/c23_constexpr_cl_false.c: expected false (no-div0)
int c23_constexpr_cl_false(int s) { return (constexpr int){4} / s; }
