// C++ allocation models (_Znwm, _Znam, _ZdlPv, _ZdaPv ... in
// src/prism/pir/models/libc/stdlib.c) against [basic.stc.dynamic]. A C++
// unit cannot include the C model (the symbols are mangled names), so this
// harness uses the model the pir stage links for every undefined symbol
// (the same embedded source, docs/PIR.md "Library models").
#include <cassert>
#include <cstddef>

// new T[n] returns n usable elements; delete[] releases them; new never
// returns NULL (the throwing path leaves the function).
int new_array_true(int n, int j) {
    if (n < 1 || n > 4 || j < 0 || j >= n) return 0;
    int *p = new int[n];
    p[j] = 7;
    assert(p[j] == 7);
    delete[] p;
    int *q = new int(3);
    assert(q != nullptr && *q == 3);
    delete q;
    return 0;
}

// Mismatched form: new[] released with delete ([expr.delete]p2).
int new_delete_mismatch_false(int n) {
    if (n < 1 || n > 4) return 0;
    int *p = new int[n];
    delete p;
    return 0;
}

// One element past a new[] object.
int new_array_oob_false(int n) {
    if (n < 1 || n > 4) return 0;
    int *p = new int[n];
    p[n] = 0;
    delete[] p;
    return 0;
}

// Sized deallocation ([new.delete.single]p13, [new.delete.array]p12):
// clang 18 emits it only with -fsized-deallocation, so the harness declares
// and calls the usual deallocation functions explicitly (_ZdlPvm, _ZdaPvm).
void operator delete(void *p, std::size_t n) noexcept;
void operator delete[](void *p, std::size_t n) noexcept;

int sized_delete_true(int c) {
    int *p = new int(c);
    assert(*p == c);
    ::operator delete(p, sizeof(int));
    char *q = new char[4];
    q[3] = 1;
    ::operator delete[](q, 4);
    return 0;
}

// Sized delete twice.
int sized_delete_double_false(int c) {
    int *p = new int(c);
    ::operator delete(p, sizeof(int));
    ::operator delete(p, sizeof(int));
    return 0;
}

// Sized array delete of a single new.
int sized_delete_mismatch_false(int c) {
    int *p = new int(c);
    ::operator delete[](p, sizeof(int));
    return 0;
}
