// PIR tasks: the C++ library subset (docs/PIR.md "C++ library"). Units are
// compiled with -D_GLIBCXX_ASSERTIONS: libstdc++'s own preconditions become
// reachable __glibcxx_assert_fail calls, i.e. PIR violations.
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

int span_ok(int i) {
    int a[4] = {1, 2, 3, 4};
    std::span<int> s(a);
    return (i >= 0 && i < 4) ? s[i] : 0;
}

int span_bad(int i) {
    int a[4] = {1, 2, 3, 4};
    std::span<int> s(a);
    return (i >= 0 && i <= 4) ? s[i] : 0;  // s[4]
}

int array_ok(int i) {
    std::array<int, 4> a{1, 2, 3, 4};
    return a[i & 3];
}

int array_bad(int i) {
    std::array<int, 4> a{1, 2, 3, 4};
    return a[i & 7];  // up to a[7]
}

static std::optional<int> maybe(int c) {
    if (c) return 3;
    return std::nullopt;
}

int optional_ok(int c) {
    auto o = maybe(c);
    return o ? *o : 0;
}

int optional_bad(int c) {
    auto o = maybe(c);
    return *o;  // empty when c == 0
}

int unique_ok(int c) {
    std::unique_ptr<int> p(new int(c & 7));
    return *p;
}

int unique_bad(int c) {
    std::unique_ptr<int> p(c ? new int(3) : nullptr);
    return *p;  // null when c == 0
}

int vector_ok(int i) {
    std::vector<int> v{1, 2, 3};
    return (i >= 0 && i < 3) ? v[i] : 0;
}

int vector_bad(int i) {
    std::vector<int> v{1, 2, 3};
    return (i >= 0 && i <= 3) ? v[i] : 0;  // v[3]
}

int vector_at_throws(int i) {
    std::vector<int> v{1, 2, 3};
    return v.at(i & 3);  // at(3) throws; the destructor cleanup path is not modelled
}
