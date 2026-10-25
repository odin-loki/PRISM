// PIR tasks: C++23 through the same pipeline (clang++ -std=c++23).
int cxx_add_bad(int a, int b) { return a + b; }
template <class T> T twice(T x) { return x * 2; }
int use_twice_ok(int x) {
    if (x > 1000 || x < -1000)
        return 0;
    return twice(x);
}
struct Counter {
    int v;
    int get() const { return v; }
};
int use_counter(int x) {
    Counter c{x};
    return c.get();
}
// C++20 and later: signed left shift is defined (modular), unlike C17.
int cxx_shl_ok(int a) {
    if (a < 0)
        return 0;
    return a << 4;
}
constexpr int sq(int x) { return x * x; }
int constexpr_ok(void) { return sq(1000); }
