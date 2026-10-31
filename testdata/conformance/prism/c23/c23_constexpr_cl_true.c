// PRISM conformance task c23/c23_constexpr_cl_true.c: expected true (no-div0)
int c23_constexpr_cl_true(int s) { return s / (constexpr int){4}; }
