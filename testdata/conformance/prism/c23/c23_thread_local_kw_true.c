// PRISM conformance task c23/c23_thread_local_kw_true.c: expected true (no-overflow)
thread_local static int counter;
int c23_thread_local_kw_true(short x) { counter = x; return counter + 1; }
