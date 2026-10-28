// Harness for std::span (tests/cpp/test_cxx_span.cpp): compiled once against
// libstdc++ and once with PRISM's model header (src/prism/pir/models/cxx/span)
// first on the include path. Touches size, empty, an in-range index, last(2),
// and last(0). The view does not own: `kept` is still the same int after the
// span dies. last(0) prints a size and does not read an element.
#include <cstdio>
#include <cstdlib>
#include <span>

// Host stub for the model intrinsic. A passing check returns. libstdc++ does
// not call it. A failing check aborts before a size is printed.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

int main() {
    int a[4] = {10, 20, 30, 40};
    std::span<int> s(a);
    std::printf("size=%zu\n", s.size());
    std::printf("empty=%d\n", s.empty() ? 1 : 0);
    std::printf("at2=%d\n", s[2]);
    std::span<int> none(a, 0);
    std::printf("none_size=%zu\n", none.size());
    std::printf("none_empty=%d\n", none.empty() ? 1 : 0);
    int kept = 5;
    {
        std::span<int> view(&kept, 1);
        std::printf("view=%d\n", view[0]);
    }
    std::printf("kept=%d\n", kept);
    std::span<int> tail = s.last(2);
    std::printf("last2=%zu %d %d %d\n", tail.size(), tail[0], tail[1], tail.data() == s.data() + 2 ? 1 : 0);
    std::span<int> z = s.last(0);
    std::printf("last0=%zu %d\n", z.size(), z.data() == s.data() + s.size() ? 1 : 0);
    std::span<int> ze = none.last(0);
    std::printf("none_last=%zu\n", ze.size());
    return 0;
}
