// Harness for std::span (tests/cpp/test_cxx_span.cpp): compiled once against
// libstdc++ and once with PRISM's model header (src/prism/pir/models/cxx/span)
// first on the include path. Touches size, empty, and an in-range index.
// The view does not own: `kept` is still the same int after the span dies.
#include <cstdio>
#include <span>

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
    return 0;
}
