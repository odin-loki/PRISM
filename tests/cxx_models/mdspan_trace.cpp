// Host trace of the std::mdspan model (src/prism/pir/models/cxx/mdspan).
// Compiled with that directory on the include path so #include <mdspan> is
// the model. __prism_check is the PIR intrinsic; the doctest links a stub
// that aborts when the precondition fails. This is not a libstdc++
// differential: operator[] and the layout policies are not in the model.
#include <mdspan>

#include <cstddef>
#include <cstdio>
#include <string>

static void in_range() {
    int a[4] = {10, 20, 30, 40};
    std::mdspan<int> m(a, 4);
    m(0) = 11;
    std::printf("in %d %d %d %d\n", m(0), m(1), m(2), m(3));
    std::printf("size %zu %zu\n", m.size(), m.extent(0));
    std::printf("write %d\n", a[0]);
    std::printf("alias %d\n", int(m.data() == a));
}

// The view dies. The array is still the caller's object.
static void not_own() {
    int stack[2] = {7, 8};
    int* p = stack;
    {
        std::mdspan<int> m(p, 2);
        std::printf("inside %d\n", m(1));
    }
    std::printf("after %d\n", p[1]);

    int* heap = new int[2]{9, 4};
    {
        std::mdspan<int> m(heap, 2);
        std::printf("heap %d\n", m(0));
    }
    std::printf("kept %d\n", heap[0]);
    delete[] heap;
}

static void empty() {
    std::mdspan<int> z(static_cast<int*>(nullptr), 0);
    std::printf("empty %zu %zu %d\n", z.size(), z.extent(0), int(z.data() == nullptr));
}

static void rank2() {
    int b[6] = {1, 2, 3, 4, 5, 6};
    std::mdspan<int, 2> m(b, 2, 3);
    std::printf("r2 %zu %zu %zu %d %d\n", m.size(), m.extent(0), m.extent(1), m(0, 0), m(1, 2));
}

static void rank3() {
    int c[24];
    for (int i = 0; i < 24; ++i) c[i] = i + 1;
    std::mdspan<int, 3> m(c, 2, 3, 4);
    std::printf("r3 %zu %zu %zu %zu %d %d\n", m.size(), m.extent(0), m.extent(1), m.extent(2), m(0, 0, 0),
                m(1, 2, 3));
}

// The other two extents would overflow size_t. The zero extent makes the
// product 0, so this is not the overflow check, and size() stays 0.
static void zero3() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, 3> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(0));
    std::printf("z3 %zu\n", m.size());
}

static void empty3() {
    std::mdspan<int, 3> z(static_cast<int*>(nullptr), 0, 0, 0);
    std::printf("e3 %zu %d\n", z.size(), int(z.data() == nullptr));
}

static int oob() {
    int a[4] = {1, 2, 3, 4};
    std::mdspan<int> m(a, 4);
    return m(4);
}

static int oob2() {
    int b[6] = {1, 2, 3, 4, 5, 6};
    std::mdspan<int, 2> m(b, 2, 3);
    return m(1, 3);
}

static int null_ext() {
    std::mdspan<int> m(static_cast<int*>(nullptr), 1);
    return static_cast<int>(m.size());
}

// which 0/1/2 is one past extent 0/1/2. The other indices are in range.
static int oob3(int which) {
    int c[24] = {};
    std::mdspan<int, 3> m(c, 2, 3, 4);
    if (which == 0) return m(2, 0, 0);
    if (which == 1) return m(0, 3, 0);
    return m(0, 0, 4);
}

static int null3() {
    std::mdspan<int, 3> m(static_cast<int*>(nullptr), 0, 0, 1);
    return static_cast<int>(m.size());
}

static int ovf3() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, 3> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2));
    return static_cast<int>(m.size());
}

static int ovf2() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, 2> m(&one, huge, static_cast<std::size_t>(2));
    return static_cast<int>(m.size());
}

int main(int argc, char** argv) {
    const std::string arg = argc > 1 ? argv[1] : "";
    if (arg == "oob") return oob();
    if (arg == "oob2") return oob2();
    if (arg == "oob3") return oob3(0);
    if (arg == "oob3j") return oob3(1);
    if (arg == "oob3k") return oob3(2);
    if (arg == "null") return null_ext();
    if (arg == "null3") return null3();
    if (arg == "ovf3") return ovf3();
    if (arg == "ovf2") return ovf2();
    in_range();
    not_own();
    empty();
    rank2();
    rank3();
    zero3();
    empty3();
    return 0;
}
