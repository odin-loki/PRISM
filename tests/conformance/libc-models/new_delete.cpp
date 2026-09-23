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
