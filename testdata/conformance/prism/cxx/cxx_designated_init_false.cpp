// PRISM conformance task cxx/cxx_designated_init_false.cpp: expected false (no-oob)
// C++20 designated initialisers, index 2 is out of bounds
struct P { int x; int y; };
int cxx_designated_init_false(int i) {
    P ps[2] = {{.x = 1, .y = 2}, {.x = 3, .y = 4}};
    return (i >= 0 && i <= 2) ? ps[i].y : 0;
}
