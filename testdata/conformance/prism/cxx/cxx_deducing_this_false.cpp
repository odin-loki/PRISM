// PRISM conformance task cxx/cxx_deducing_this_false.cpp: expected false (no-overflow)
// C++23 explicit object parameter
struct C { int v; int twice(this const C& s) { return s.v * 2; } };
int cxx_deducing_this_false(int x) { C c{x}; return c.twice(); }
