// Differential trace of views::take / views::drop over contiguous ranges.
// Compiled against libstdc++ and against PRISM's <ranges> model; the output
// must match. Counts past the size are clamped by both.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <ranges>
#include <vector>

extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::abort();
    }
}

int main() {
    int a[5] = {1, 2, 3, 4, 5};
    std::array<int, 4> b{10, 20, 30, 40};
    std::vector<int> c{7, 8, 9};
    for (int n : {0, 2, 5, 9}) {
        int s = 0;
        for (int v : a | std::views::take(n)) s += v;
        int t = 0;
        for (int v : a | std::views::drop(n)) t += v;
        int u = 0;
        for (int v : b | std::views::take(n)) u += v;
        int w = 0;
        for (int v : c | std::views::drop(n)) w += v;
        std::printf("%d %d %d %d %d\n", n, s, t, u, w);
    }
    return 0;
}
