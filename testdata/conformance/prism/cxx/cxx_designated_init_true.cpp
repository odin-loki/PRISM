// PRISM conformance task cxx/cxx_designated_init_true.cpp: expected true (no-oob)
// C++20 designated initialisers, index stays inside ps[2]
struct P { int x; int y; };
int cxx_designated_init_true(int i) {
    P ps[2] = {{.x = 1, .y = 2}, {.x = 3, .y = 4}};
    return (i >= 0 && i <= 1) ? ps[i].y : 0;
}
