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

static void rank4() {
    int d[120];
    for (int i = 0; i < 120; ++i) d[i] = i + 1;
    std::mdspan<int, 4> m(d, 2, 3, 4, 5);
    std::printf("r4 %zu %zu %zu %zu %zu %d %d\n", m.size(), m.extent(0), m.extent(1), m.extent(2), m.extent(3),
                m(0, 0, 0, 0), m(1, 2, 3, 4));
}

// The other three extents would overflow size_t. The zero extent makes the
// product 0, so this is not the overflow check, and size() stays 0.
static void zero4() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, 4> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(0));
    std::printf("z4 %zu\n", m.size());
}

static void empty4() {
    std::mdspan<int, 4> z(static_cast<int*>(nullptr), 0, 0, 0, 0);
    std::printf("e4 %zu %d\n", z.size(), int(z.data() == nullptr));
}

static void rank5() {
    int e[720];
    for (int i = 0; i < 720; ++i) e[i] = i + 1;
    std::mdspan<int, 5> m(e, 2, 3, 4, 5, 6);
    std::printf("r5 %zu %zu %zu %zu %zu %zu %d %d\n", m.size(), m.extent(0), m.extent(1), m.extent(2), m.extent(3),
                m.extent(4), m(0, 0, 0, 0, 0), m(1, 2, 3, 4, 5));
}

// The other four extents would overflow size_t. The zero extent makes the
// product 0, so this is not the overflow check, and size() stays 0.
static void zero5() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, 5> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(2), static_cast<std::size_t>(0));
    std::printf("z5 %zu\n", m.size());
}

static void empty5() {
    std::mdspan<int, 5> z(static_cast<int*>(nullptr), 0, 0, 0, 0, 0);
    std::printf("e5 %zu %d\n", z.size(), int(z.data() == nullptr));
}

static void rank6() {
    int f[5040];
    for (int i = 0; i < 5040; ++i) f[i] = i + 1;
    std::mdspan<int, 6> m(f, 2, 3, 4, 5, 6, 7);
    std::printf("r6 %zu %zu %zu %zu %zu %zu %zu %d %d\n", m.size(), m.extent(0), m.extent(1), m.extent(2),
                m.extent(3), m.extent(4), m.extent(5), m(0, 0, 0, 0, 0, 0), m(1, 2, 3, 4, 5, 6));
}

// The other five extents would overflow size_t. The zero extent makes the
// product 0, so this is not the overflow check, and size() stays 0.
static void zero6() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, 6> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(2), static_cast<std::size_t>(2), static_cast<std::size_t>(0));
    std::printf("z6 %zu\n", m.size());
}

static void empty6() {
    std::mdspan<int, 6> z(static_cast<int*>(nullptr), 0, 0, 0, 0, 0, 0);
    std::printf("e6 %zu %d\n", z.size(), int(z.data() == nullptr));
}

static void rank7() {
    int g[40320];
    for (int i = 0; i < 40320; ++i) g[i] = i + 1;
    std::mdspan<int, 7> m(g, 2, 3, 4, 5, 6, 7, 8);
    std::printf("r7 %zu %zu %zu %zu %zu %zu %zu %zu %d %d\n", m.size(), m.extent(0), m.extent(1), m.extent(2),
                m.extent(3), m.extent(4), m.extent(5), m.extent(6), m(0, 0, 0, 0, 0, 0, 0), m(1, 2, 3, 4, 5, 6, 7));
}

// The other six extents would overflow size_t. The zero extent makes the
// product 0, so this is not the overflow check, and size() stays 0.
static void zero7() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, 7> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(2), static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(0));
    std::printf("z7 %zu\n", m.size());
}

static void empty7() {
    std::mdspan<int, 7> z(static_cast<int*>(nullptr), 0, 0, 0, 0, 0, 0, 0);
    std::printf("e7 %zu %d\n", z.size(), int(z.data() == nullptr));
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

// which 0/1/2/3 is one past extent 0/1/2/3. The other indices are in range.
static int oob4(int which) {
    int d[120] = {};
    std::mdspan<int, 4> m(d, 2, 3, 4, 5);
    if (which == 0) return m(2, 0, 0, 0);
    if (which == 1) return m(0, 3, 0, 0);
    if (which == 2) return m(0, 0, 4, 0);
    return m(0, 0, 0, 5);
}

static int null4() {
    std::mdspan<int, 4> m(static_cast<int*>(nullptr), 0, 0, 0, 1);
    return static_cast<int>(m.size());
}

static int ovf4() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, 4> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(2));
    return static_cast<int>(m.size());
}

// which 0/1/2/3/4 is one past extent 0/1/2/3/4. The other indices are in range.
static int oob5(int which) {
    int e[720] = {};
    std::mdspan<int, 5> m(e, 2, 3, 4, 5, 6);
    if (which == 0) return m(2, 0, 0, 0, 0);
    if (which == 1) return m(0, 3, 0, 0, 0);
    if (which == 2) return m(0, 0, 4, 0, 0);
    if (which == 3) return m(0, 0, 0, 5, 0);
    return m(0, 0, 0, 0, 6);
}

static int null5() {
    std::mdspan<int, 5> m(static_cast<int*>(nullptr), 0, 0, 0, 0, 1);
    return static_cast<int>(m.size());
}

static int ovf5() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, 5> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(2), static_cast<std::size_t>(2));
    return static_cast<int>(m.size());
}

// which 0/1/2/3/4/5 is one past extent 0/1/2/3/4/5. The other indices are in range.
static int oob6(int which) {
    int f[5040] = {};
    std::mdspan<int, 6> m(f, 2, 3, 4, 5, 6, 7);
    if (which == 0) return m(2, 0, 0, 0, 0, 0);
    if (which == 1) return m(0, 3, 0, 0, 0, 0);
    if (which == 2) return m(0, 0, 4, 0, 0, 0);
    if (which == 3) return m(0, 0, 0, 5, 0, 0);
    if (which == 4) return m(0, 0, 0, 0, 6, 0);
    return m(0, 0, 0, 0, 0, 7);
}

static int null6() {
    std::mdspan<int, 6> m(static_cast<int*>(nullptr), 0, 0, 0, 0, 0, 1);
    return static_cast<int>(m.size());
}

static int ovf6() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, 6> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(2), static_cast<std::size_t>(2), static_cast<std::size_t>(2));
    return static_cast<int>(m.size());
}

// which 0/1/2/3/4/5/6 is one past extent 0/1/2/3/4/5/6. The other indices are in range.
static int oob7(int which) {
    int g[40320] = {};
    std::mdspan<int, 7> m(g, 2, 3, 4, 5, 6, 7, 8);
    if (which == 0) return m(2, 0, 0, 0, 0, 0, 0);
    if (which == 1) return m(0, 3, 0, 0, 0, 0, 0);
    if (which == 2) return m(0, 0, 4, 0, 0, 0, 0);
    if (which == 3) return m(0, 0, 0, 5, 0, 0, 0);
    if (which == 4) return m(0, 0, 0, 0, 6, 0, 0);
    if (which == 5) return m(0, 0, 0, 0, 0, 7, 0);
    return m(0, 0, 0, 0, 0, 0, 8);
}

static int null7() {
    std::mdspan<int, 7> m(static_cast<int*>(nullptr), 0, 0, 0, 0, 0, 0, 1);
    return static_cast<int>(m.size());
}

static int ovf7() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, 7> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(2), static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(2));
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
    if (arg == "oob4") return oob4(0);
    if (arg == "oob4j") return oob4(1);
    if (arg == "oob4k") return oob4(2);
    if (arg == "oob4l") return oob4(3);
    if (arg == "null4") return null4();
    if (arg == "ovf4") return ovf4();
    if (arg == "oob5") return oob5(0);
    if (arg == "oob5j") return oob5(1);
    if (arg == "oob5k") return oob5(2);
    if (arg == "oob5l") return oob5(3);
    if (arg == "oob5m") return oob5(4);
    if (arg == "null5") return null5();
    if (arg == "ovf5") return ovf5();
    if (arg == "oob6") return oob6(0);
    if (arg == "oob6j") return oob6(1);
    if (arg == "oob6k") return oob6(2);
    if (arg == "oob6l") return oob6(3);
    if (arg == "oob6m") return oob6(4);
    if (arg == "oob6n") return oob6(5);
    if (arg == "null6") return null6();
    if (arg == "ovf6") return ovf6();
    if (arg == "oob7") return oob7(0);
    if (arg == "oob7j") return oob7(1);
    if (arg == "oob7k") return oob7(2);
    if (arg == "oob7l") return oob7(3);
    if (arg == "oob7m") return oob7(4);
    if (arg == "oob7n") return oob7(5);
    if (arg == "oob7p") return oob7(6);
    if (arg == "null7") return null7();
    if (arg == "ovf7") return ovf7();
    in_range();
    not_own();
    empty();
    rank2();
    rank3();
    zero3();
    empty3();
    rank4();
    zero4();
    empty4();
    rank5();
    zero5();
    empty5();
    rank6();
    zero6();
    empty6();
    rank7();
    zero7();
    empty7();
    return 0;
}
