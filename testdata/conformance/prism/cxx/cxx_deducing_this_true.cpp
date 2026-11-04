// PRISM conformance task cxx/cxx_deducing_this_true.cpp: expected true (no-overflow)
// C++23 explicit object parameter, short input: twice fits in int
struct C { int v; int twice(this const C& s) { return s.v * 2; } };
int cxx_deducing_this_true(short x) { C c{x}; return c.twice(); }
