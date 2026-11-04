// PRISM conformance task cxx/cxx_import_std_false.cpp: expected false (noassert)
// `import std;` resolves to the modelled std module (models/cxx/std.cppm); the sum is 6, so the trap fires
import std;
int cxx_import_std_false(int c) {
    std::vector<int> v{1, 2, 3};
    int s = 0;
    for (int x : v) s += x;
    if (s == 6) __builtin_trap();
    return s + (c & 0);
}
