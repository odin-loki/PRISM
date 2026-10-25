// PIR: dynamic initialisation before main (docs/CONFORMANCE.md S8). The IR
// initializer of `g` is zeroinitializer; its constructor stores 5 before
// main runs, so the assertion fails in the program. Was PROVED.
#include <cassert>

struct B {
    int i;
    B() : i(5) {}
};

B g;

int main() {
    assert(g.i == 0);
    return 0;
}
