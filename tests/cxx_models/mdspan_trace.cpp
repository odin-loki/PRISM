// Host trace of the std::mdspan model (src/prism/pir/models/cxx/mdspan).
// Compiled with that directory on the include path so #include <mdspan> is
// the model. __prism_check is the PIR intrinsic; the doctest links a stub
// that aborts when the precondition fails. This is not a library
// differential: it calls operator() and data(), which the model declares
// and the standard mdspan does not.
#include <mdspan>

#include <cstddef>
#include <cstdio>
#include <string>
#include <type_traits>

static void in_range() {
    int a[4] = {10, 20, 30, 40};
    std::mdspan<int, std::dextents<std::size_t, 1>> m(a, 4);
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
        std::mdspan<int, std::dextents<std::size_t, 1>> m(p, 2);
        std::printf("inside %d\n", m(1));
    }
    std::printf("after %d\n", p[1]);

    int* heap = new int[2]{9, 4};
    {
        std::mdspan<int, std::dextents<std::size_t, 1>> m(heap, 2);
        std::printf("heap %d\n", m(0));
    }
    std::printf("kept %d\n", heap[0]);
    delete[] heap;
}

static void empty() {
    std::mdspan<int, std::dextents<std::size_t, 1>> z(static_cast<int*>(nullptr), 0);
    std::printf("empty %zu %zu %d\n", z.size(), z.extent(0), int(z.data() == nullptr));
}

static void rank2() {
    int b[6] = {1, 2, 3, 4, 5, 6};
    std::mdspan<int, std::dextents<std::size_t, 2>> m(b, 2, 3);
    std::printf("r2 %zu %zu %zu %d %d\n", m.size(), m.extent(0), m.extent(1), m(0, 0), m(1, 2));
}

static void rank3() {
    int c[24];
    for (int i = 0; i < 24; ++i) c[i] = i + 1;
    std::mdspan<int, std::dextents<std::size_t, 3>> m(c, 2, 3, 4);
    std::printf("r3 %zu %zu %zu %zu %d %d\n", m.size(), m.extent(0), m.extent(1), m.extent(2), m(0, 0, 0),
                m(1, 2, 3));
}

// The other two extents would overflow size_t. The zero extent makes the
// product 0, so this is not the overflow check, and size() stays 0.
static void zero3() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 3>> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(0));
    std::printf("z3 %zu\n", m.size());
}

static void empty3() {
    std::mdspan<int, std::dextents<std::size_t, 3>> z(static_cast<int*>(nullptr), 0, 0, 0);
    std::printf("e3 %zu %d\n", z.size(), int(z.data() == nullptr));
}

static void rank4() {
    int d[120];
    for (int i = 0; i < 120; ++i) d[i] = i + 1;
    std::mdspan<int, std::dextents<std::size_t, 4>> m(d, 2, 3, 4, 5);
    std::printf("r4 %zu %zu %zu %zu %zu %d %d\n", m.size(), m.extent(0), m.extent(1), m.extent(2), m.extent(3),
                m(0, 0, 0, 0), m(1, 2, 3, 4));
}

// The other three extents would overflow size_t. The zero extent makes the
// product 0, so this is not the overflow check, and size() stays 0.
static void zero4() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 4>> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(0));
    std::printf("z4 %zu\n", m.size());
}

static void empty4() {
    std::mdspan<int, std::dextents<std::size_t, 4>> z(static_cast<int*>(nullptr), 0, 0, 0, 0);
    std::printf("e4 %zu %d\n", z.size(), int(z.data() == nullptr));
}

static void rank5() {
    int e[720];
    for (int i = 0; i < 720; ++i) e[i] = i + 1;
    std::mdspan<int, std::dextents<std::size_t, 5>> m(e, 2, 3, 4, 5, 6);
    std::printf("r5 %zu %zu %zu %zu %zu %zu %d %d\n", m.size(), m.extent(0), m.extent(1), m.extent(2), m.extent(3),
                m.extent(4), m(0, 0, 0, 0, 0), m(1, 2, 3, 4, 5));
}

// The other four extents would overflow size_t. The zero extent makes the
// product 0, so this is not the overflow check, and size() stays 0.
static void zero5() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 5>> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(2), static_cast<std::size_t>(0));
    std::printf("z5 %zu\n", m.size());
}

static void empty5() {
    std::mdspan<int, std::dextents<std::size_t, 5>> z(static_cast<int*>(nullptr), 0, 0, 0, 0, 0);
    std::printf("e5 %zu %d\n", z.size(), int(z.data() == nullptr));
}

static void rank6() {
    int f[5040];
    for (int i = 0; i < 5040; ++i) f[i] = i + 1;
    std::mdspan<int, std::dextents<std::size_t, 6>> m(f, 2, 3, 4, 5, 6, 7);
    std::printf("r6 %zu %zu %zu %zu %zu %zu %zu %d %d\n", m.size(), m.extent(0), m.extent(1), m.extent(2),
                m.extent(3), m.extent(4), m.extent(5), m(0, 0, 0, 0, 0, 0), m(1, 2, 3, 4, 5, 6));
}

// The other five extents would overflow size_t. The zero extent makes the
// product 0, so this is not the overflow check, and size() stays 0.
static void zero6() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 6>> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(2), static_cast<std::size_t>(2), static_cast<std::size_t>(0));
    std::printf("z6 %zu\n", m.size());
}

static void empty6() {
    std::mdspan<int, std::dextents<std::size_t, 6>> z(static_cast<int*>(nullptr), 0, 0, 0, 0, 0, 0);
    std::printf("e6 %zu %d\n", z.size(), int(z.data() == nullptr));
}

static void rank7() {
    int g[40320];
    for (int i = 0; i < 40320; ++i) g[i] = i + 1;
    std::mdspan<int, std::dextents<std::size_t, 7>> m(g, 2, 3, 4, 5, 6, 7, 8);
    std::printf("r7 %zu %zu %zu %zu %zu %zu %zu %zu %d %d\n", m.size(), m.extent(0), m.extent(1), m.extent(2),
                m.extent(3), m.extent(4), m.extent(5), m.extent(6), m(0, 0, 0, 0, 0, 0, 0), m(1, 2, 3, 4, 5, 6, 7));
}

// The other six extents would overflow size_t. The zero extent makes the
// product 0, so this is not the overflow check, and size() stays 0.
static void zero7() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 7>> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(2), static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(0));
    std::printf("z7 %zu\n", m.size());
}

static void empty7() {
    std::mdspan<int, std::dextents<std::size_t, 7>> z(static_cast<int*>(nullptr), 0, 0, 0, 0, 0, 0, 0);
    std::printf("e7 %zu %d\n", z.size(), int(z.data() == nullptr));
}

static int oob() {
    int a[4] = {1, 2, 3, 4};
    std::mdspan<int, std::dextents<std::size_t, 1>> m(a, 4);
    return m(4);
}

static int oob2() {
    int b[6] = {1, 2, 3, 4, 5, 6};
    std::mdspan<int, std::dextents<std::size_t, 2>> m(b, 2, 3);
    return m(1, 3);
}

static int null_ext() {
    std::mdspan<int, std::dextents<std::size_t, 1>> m(static_cast<int*>(nullptr), 1);
    return static_cast<int>(m.size());
}

// which 0/1/2 is one past extent 0/1/2. The other indices are in range.
static int oob3(int which) {
    int c[24] = {};
    std::mdspan<int, std::dextents<std::size_t, 3>> m(c, 2, 3, 4);
    if (which == 0) return m(2, 0, 0);
    if (which == 1) return m(0, 3, 0);
    return m(0, 0, 4);
}

static int null3() {
    std::mdspan<int, std::dextents<std::size_t, 3>> m(static_cast<int*>(nullptr), 0, 0, 1);
    return static_cast<int>(m.size());
}

static int ovf3() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 3>> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2));
    return static_cast<int>(m.size());
}

static int ovf2() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 2>> m(&one, huge, static_cast<std::size_t>(2));
    return static_cast<int>(m.size());
}

// which 0/1/2/3 is one past extent 0/1/2/3. The other indices are in range.
static int oob4(int which) {
    int d[120] = {};
    std::mdspan<int, std::dextents<std::size_t, 4>> m(d, 2, 3, 4, 5);
    if (which == 0) return m(2, 0, 0, 0);
    if (which == 1) return m(0, 3, 0, 0);
    if (which == 2) return m(0, 0, 4, 0);
    return m(0, 0, 0, 5);
}

static int null4() {
    std::mdspan<int, std::dextents<std::size_t, 4>> m(static_cast<int*>(nullptr), 0, 0, 0, 1);
    return static_cast<int>(m.size());
}

static int ovf4() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 4>> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(2));
    return static_cast<int>(m.size());
}

// which 0/1/2/3/4 is one past extent 0/1/2/3/4. The other indices are in range.
static int oob5(int which) {
    int e[720] = {};
    std::mdspan<int, std::dextents<std::size_t, 5>> m(e, 2, 3, 4, 5, 6);
    if (which == 0) return m(2, 0, 0, 0, 0);
    if (which == 1) return m(0, 3, 0, 0, 0);
    if (which == 2) return m(0, 0, 4, 0, 0);
    if (which == 3) return m(0, 0, 0, 5, 0);
    return m(0, 0, 0, 0, 6);
}

static int null5() {
    std::mdspan<int, std::dextents<std::size_t, 5>> m(static_cast<int*>(nullptr), 0, 0, 0, 0, 1);
    return static_cast<int>(m.size());
}

static int ovf5() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 5>> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(2), static_cast<std::size_t>(2));
    return static_cast<int>(m.size());
}

// which 0/1/2/3/4/5 is one past extent 0/1/2/3/4/5. The other indices are in range.
static int oob6(int which) {
    int f[5040] = {};
    std::mdspan<int, std::dextents<std::size_t, 6>> m(f, 2, 3, 4, 5, 6, 7);
    if (which == 0) return m(2, 0, 0, 0, 0, 0);
    if (which == 1) return m(0, 3, 0, 0, 0, 0);
    if (which == 2) return m(0, 0, 4, 0, 0, 0);
    if (which == 3) return m(0, 0, 0, 5, 0, 0);
    if (which == 4) return m(0, 0, 0, 0, 6, 0);
    return m(0, 0, 0, 0, 0, 7);
}

static int null6() {
    std::mdspan<int, std::dextents<std::size_t, 6>> m(static_cast<int*>(nullptr), 0, 0, 0, 0, 0, 1);
    return static_cast<int>(m.size());
}

static int ovf6() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 6>> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(2), static_cast<std::size_t>(2), static_cast<std::size_t>(2));
    return static_cast<int>(m.size());
}

// which 0/1/2/3/4/5/6 is one past extent 0/1/2/3/4/5/6. The other indices are in range.
static int oob7(int which) {
    int g[40320] = {};
    std::mdspan<int, std::dextents<std::size_t, 7>> m(g, 2, 3, 4, 5, 6, 7, 8);
    if (which == 0) return m(2, 0, 0, 0, 0, 0, 0);
    if (which == 1) return m(0, 3, 0, 0, 0, 0, 0);
    if (which == 2) return m(0, 0, 4, 0, 0, 0, 0);
    if (which == 3) return m(0, 0, 0, 5, 0, 0, 0);
    if (which == 4) return m(0, 0, 0, 0, 6, 0, 0);
    if (which == 5) return m(0, 0, 0, 0, 0, 7, 0);
    return m(0, 0, 0, 0, 0, 0, 8);
}

static int null7() {
    std::mdspan<int, std::dextents<std::size_t, 7>> m(static_cast<int*>(nullptr), 0, 0, 0, 0, 0, 0, 1);
    return static_cast<int>(m.size());
}

static int ovf7() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 7>> m(&one, huge, static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(2), static_cast<std::size_t>(2), static_cast<std::size_t>(2),
                          static_cast<std::size_t>(2));
    return static_cast<int>(m.size());
}

// Extents alternate 2 and 3 so the array stays small and every stride is
// still a product of several extents. (1, 0, 1, 0, 1, 0, 1, 0) is the
// element at offset 777, not the first or the last.
static void rank8() {
    static int g8[1296];
    for (int i = 0; i < 1296; ++i) g8[i] = i + 1;
    std::mdspan<int, std::dextents<std::size_t, 8>> m(g8, 2, 3, 2, 3, 2, 3, 2, 3);
    std::printf("r8 %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %d %d %d\n", m.rank(), m.size(), m.extent(0), m.extent(1),
                m.extent(2), m.extent(3), m.extent(4), m.extent(5), m.extent(6), m.extent(7),
                m(0, 0, 0, 0, 0, 0, 0, 0), m(1, 2, 1, 2, 1, 2, 1, 2), m(1, 0, 1, 0, 1, 0, 1, 0));
}

// The other seven extents would overflow size_t. The zero extent makes the
// product 0, so this is not the overflow check, and size() stays 0.
static void zero8() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 8>> m(&one, huge, 2, 2, 2, 2, 2, 2, 0);
    std::printf("z8 %zu\n", m.size());
}

static void empty8() {
    std::mdspan<int, std::dextents<std::size_t, 8>> z(static_cast<int*>(nullptr), 0, 0, 0, 0, 0, 0, 0, 0);
    std::printf("e8 %zu %d\n", z.size(), int(z.data() == nullptr));
}

// The deduction guide with eight extents is dextents<size_t, 8>, and
// operator[] (the standard spelling) reads the same element as operator().
static void deduce8() {
    static int h8[1296];
    for (int i = 0; i < 1296; ++i) h8[i] = i + 1;
    std::mdspan m(h8, std::size_t(2), std::size_t(3), std::size_t(2), std::size_t(3), std::size_t(2), std::size_t(3),
                  std::size_t(2), std::size_t(3));
    const bool same = std::is_same_v<decltype(m), std::mdspan<int, std::dextents<std::size_t, 8>>>;
    std::printf("d8 %d %d %d\n", int(same), m[1, 0, 1, 0, 1, 0, 1, 0],
                int(&m[1, 2, 1, 2, 1, 2, 1, 2] == &m(1, 2, 1, 2, 1, 2, 1, 2)));
}

// which 0..7 is one past extent 0..7. The other indices are in range.
// which 8 is the same past-the-end index through operator[].
static int oob8(int which) {
    static int g[1296] = {};
    std::mdspan<int, std::dextents<std::size_t, 8>> m(g, 2, 3, 2, 3, 2, 3, 2, 3);
    if (which == 0) return m(2, 0, 0, 0, 0, 0, 0, 0);
    if (which == 1) return m(0, 3, 0, 0, 0, 0, 0, 0);
    if (which == 2) return m(0, 0, 2, 0, 0, 0, 0, 0);
    if (which == 3) return m(0, 0, 0, 3, 0, 0, 0, 0);
    if (which == 4) return m(0, 0, 0, 0, 2, 0, 0, 0);
    if (which == 5) return m(0, 0, 0, 0, 0, 3, 0, 0);
    if (which == 6) return m(0, 0, 0, 0, 0, 0, 2, 0);
    if (which == 7) return m(0, 0, 0, 0, 0, 0, 0, 3);
    return m[0, 0, 0, 0, 0, 0, 0, 3];
}

static int null8() {
    std::mdspan<int, std::dextents<std::size_t, 8>> m(static_cast<int*>(nullptr), 0, 0, 0, 0, 0, 0, 0, 1);
    return static_cast<int>(m.size());
}

static int ovf8() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 8>> m(&one, huge, 2, 2, 2, 2, 2, 2, 2);
    return static_cast<int>(m.size());
}

// Extents alternate 2 and 3 so the array stays small and every stride is
// still a product of several extents. (1, 0, 1, 0, 1, 0, 1, 0, 1) is the
// element at offset 1555, not the first or the last.
static void rank9() {
    static int g9[2592];
    for (int i = 0; i < 2592; ++i) g9[i] = i + 1;
    std::mdspan<int, std::dextents<std::size_t, 9>> m(g9, 2, 3, 2, 3, 2, 3, 2, 3, 2);
    std::printf("r9 %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %d %d %d\n", m.rank(), m.size(), m.extent(0),
                m.extent(1), m.extent(2), m.extent(3), m.extent(4), m.extent(5), m.extent(6), m.extent(7),
                m.extent(8), m(0, 0, 0, 0, 0, 0, 0, 0, 0), m(1, 2, 1, 2, 1, 2, 1, 2, 1),
                m(1, 0, 1, 0, 1, 0, 1, 0, 1));
}

// The other eight extents would overflow size_t. The zero extent makes the
// product 0, so this is not the overflow check, and size() stays 0.
static void zero9() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 9>> m(&one, huge, 2, 2, 2, 2, 2, 2, 2, 0);
    std::printf("z9 %zu\n", m.size());
}

static void empty9() {
    std::mdspan<int, std::dextents<std::size_t, 9>> z(static_cast<int*>(nullptr), 0, 0, 0, 0, 0, 0, 0, 0, 0);
    std::printf("e9 %zu %d\n", z.size(), int(z.data() == nullptr));
}

// The deduction guide with nine extents is dextents<size_t, 9>, and
// operator[] (the standard spelling) reads the same element as operator().
static void deduce9() {
    static int h9[2592];
    for (int i = 0; i < 2592; ++i) h9[i] = i + 1;
    std::mdspan m(h9, std::size_t(2), std::size_t(3), std::size_t(2), std::size_t(3), std::size_t(2), std::size_t(3),
                  std::size_t(2), std::size_t(3), std::size_t(2));
    const bool same = std::is_same_v<decltype(m), std::mdspan<int, std::dextents<std::size_t, 9>>>;
    std::printf("d9 %d %d %d\n", int(same), m[1, 0, 1, 0, 1, 0, 1, 0, 1],
                int(&m[1, 2, 1, 2, 1, 2, 1, 2, 1] == &m(1, 2, 1, 2, 1, 2, 1, 2, 1)));
}

// which 0..8 is one past extent 0..8. The other indices are in range.
// which 9 is the same past-the-end index through operator[].
static int oob9(int which) {
    static int g[2592] = {};
    std::mdspan<int, std::dextents<std::size_t, 9>> m(g, 2, 3, 2, 3, 2, 3, 2, 3, 2);
    if (which == 0) return m(2, 0, 0, 0, 0, 0, 0, 0, 0);
    if (which == 1) return m(0, 3, 0, 0, 0, 0, 0, 0, 0);
    if (which == 2) return m(0, 0, 2, 0, 0, 0, 0, 0, 0);
    if (which == 3) return m(0, 0, 0, 3, 0, 0, 0, 0, 0);
    if (which == 4) return m(0, 0, 0, 0, 2, 0, 0, 0, 0);
    if (which == 5) return m(0, 0, 0, 0, 0, 3, 0, 0, 0);
    if (which == 6) return m(0, 0, 0, 0, 0, 0, 2, 0, 0);
    if (which == 7) return m(0, 0, 0, 0, 0, 0, 0, 3, 0);
    if (which == 8) return m(0, 0, 0, 0, 0, 0, 0, 0, 2);
    return m[0, 0, 0, 0, 0, 0, 0, 0, 2];
}

static int null9() {
    std::mdspan<int, std::dextents<std::size_t, 9>> m(static_cast<int*>(nullptr), 0, 0, 0, 0, 0, 0, 0, 0, 1);
    return static_cast<int>(m.size());
}

static int ovf9() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 9>> m(&one, huge, 2, 2, 2, 2, 2, 2, 2, 2);
    return static_cast<int>(m.size());
}

// Extents alternate 2 and 3 so the array stays small and every stride is
// still a product of several extents. (1, 0, 1, 0, 1, 0, 1, 0, 1, 0) is the
// element at offset 4665, not the first or the last.
static void rank10() {
    static int g10[7776];
    for (int i = 0; i < 7776; ++i) g10[i] = i + 1;
    std::mdspan<int, std::dextents<std::size_t, 10>> m(g10, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3);
    std::printf("r10 %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %d %d %d\n", m.rank(), m.size(), m.extent(0),
                m.extent(1), m.extent(2), m.extent(3), m.extent(4), m.extent(5), m.extent(6), m.extent(7),
                m.extent(8), m.extent(9), m(0, 0, 0, 0, 0, 0, 0, 0, 0, 0), m(1, 2, 1, 2, 1, 2, 1, 2, 1, 2),
                m(1, 0, 1, 0, 1, 0, 1, 0, 1, 0));
}

// The other nine extents would overflow size_t. The zero extent makes the
// product 0, so this is not the overflow check, and size() stays 0.
static void zero10() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 10>> m(&one, huge, 2, 2, 2, 2, 2, 2, 2, 2, 0);
    std::printf("z10 %zu\n", m.size());
}

static void empty10() {
    std::mdspan<int, std::dextents<std::size_t, 10>> z(static_cast<int*>(nullptr), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    std::printf("e10 %zu %d\n", z.size(), int(z.data() == nullptr));
}

// The deduction guide with ten extents is dextents<size_t, 10>, and
// operator[] (the standard spelling) reads the same element as operator().
static void deduce10() {
    static int h10[7776];
    for (int i = 0; i < 7776; ++i) h10[i] = i + 1;
    std::mdspan m(h10, std::size_t(2), std::size_t(3), std::size_t(2), std::size_t(3), std::size_t(2), std::size_t(3),
                  std::size_t(2), std::size_t(3), std::size_t(2), std::size_t(3));
    const bool same = std::is_same_v<decltype(m), std::mdspan<int, std::dextents<std::size_t, 10>>>;
    std::printf("d10 %d %d %d\n", int(same), m[1, 0, 1, 0, 1, 0, 1, 0, 1, 0],
                int(&m[1, 2, 1, 2, 1, 2, 1, 2, 1, 2] == &m(1, 2, 1, 2, 1, 2, 1, 2, 1, 2)));
}

// which 0..9 is one past extent 0..9. The other indices are in range.
// which 10 is the same past-the-end index through operator[].
static int oob10(int which) {
    static int g[7776] = {};
    std::mdspan<int, std::dextents<std::size_t, 10>> m(g, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3);
    if (which == 0) return m(2, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (which == 1) return m(0, 3, 0, 0, 0, 0, 0, 0, 0, 0);
    if (which == 2) return m(0, 0, 2, 0, 0, 0, 0, 0, 0, 0);
    if (which == 3) return m(0, 0, 0, 3, 0, 0, 0, 0, 0, 0);
    if (which == 4) return m(0, 0, 0, 0, 2, 0, 0, 0, 0, 0);
    if (which == 5) return m(0, 0, 0, 0, 0, 3, 0, 0, 0, 0);
    if (which == 6) return m(0, 0, 0, 0, 0, 0, 2, 0, 0, 0);
    if (which == 7) return m(0, 0, 0, 0, 0, 0, 0, 3, 0, 0);
    if (which == 8) return m(0, 0, 0, 0, 0, 0, 0, 0, 2, 0);
    if (which == 9) return m(0, 0, 0, 0, 0, 0, 0, 0, 0, 3);
    return m[0, 0, 0, 0, 0, 0, 0, 0, 0, 3];
}

static int null10() {
    std::mdspan<int, std::dextents<std::size_t, 10>> m(static_cast<int*>(nullptr), 0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
    return static_cast<int>(m.size());
}

static int ovf10() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 10>> m(&one, huge, 2, 2, 2, 2, 2, 2, 2, 2, 2);
    return static_cast<int>(m.size());
}

// Extents alternate 2 and 3 so the array stays small and every stride is
// still a product of several extents. (1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1) is the
// element at offset 9331, not the first or the last.
static void rank11() {
    static int g11[15552];
    for (int i = 0; i < 15552; ++i) g11[i] = i + 1;
    std::mdspan<int, std::dextents<std::size_t, 11>> m(g11, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2);
    std::printf("r11 %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %d %d %d\n", m.rank(), m.size(), m.extent(0),
                m.extent(1), m.extent(2), m.extent(3), m.extent(4), m.extent(5), m.extent(6), m.extent(7),
                m.extent(8), m.extent(9), m.extent(10), m(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
                m(1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1), m(1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1));
}

// The other ten extents would overflow size_t. The zero extent makes the
// product 0, so this is not the overflow check, and size() stays 0.
static void zero11() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 11>> m(&one, huge, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0);
    std::printf("z11 %zu\n", m.size());
}

static void empty11() {
    std::mdspan<int, std::dextents<std::size_t, 11>> z(static_cast<int*>(nullptr), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    std::printf("e11 %zu %d\n", z.size(), int(z.data() == nullptr));
}

// The deduction guide with eleven extents is dextents<size_t, 11>, and
// operator[] (the standard spelling) reads the same element as operator().
static void deduce11() {
    static int h11[15552];
    for (int i = 0; i < 15552; ++i) h11[i] = i + 1;
    std::mdspan m(h11, std::size_t(2), std::size_t(3), std::size_t(2), std::size_t(3), std::size_t(2), std::size_t(3),
                  std::size_t(2), std::size_t(3), std::size_t(2), std::size_t(3), std::size_t(2));
    const bool same = std::is_same_v<decltype(m), std::mdspan<int, std::dextents<std::size_t, 11>>>;
    std::printf("d11 %d %d %d\n", int(same), m[1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1],
                int(&m[1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1] == &m(1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1)));
}

// which 0..10 is one past extent 0..10. The other indices are in range.
// which 11 is the same past-the-end index through operator[].
static int oob11(int which) {
    static int g[15552] = {};
    std::mdspan<int, std::dextents<std::size_t, 11>> m(g, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2);
    if (which == 0) return m(2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (which == 1) return m(0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (which == 2) return m(0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0);
    if (which == 3) return m(0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0);
    if (which == 4) return m(0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0);
    if (which == 5) return m(0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0);
    if (which == 6) return m(0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0);
    if (which == 7) return m(0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0);
    if (which == 8) return m(0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0);
    if (which == 9) return m(0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0);
    if (which == 10) return m(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2);
    return m[0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2];
}

static int null11() {
    std::mdspan<int, std::dextents<std::size_t, 11>> m(static_cast<int*>(nullptr), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
    return static_cast<int>(m.size());
}

static int ovf11() {
    int one = 1;
    const std::size_t huge = static_cast<std::size_t>(-1);
    std::mdspan<int, std::dextents<std::size_t, 11>> m(&one, huge, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2);
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
    if (arg == "oob8") return oob8(0);
    if (arg == "oob8j") return oob8(1);
    if (arg == "oob8k") return oob8(2);
    if (arg == "oob8l") return oob8(3);
    if (arg == "oob8m") return oob8(4);
    if (arg == "oob8n") return oob8(5);
    if (arg == "oob8p") return oob8(6);
    if (arg == "oob8q") return oob8(7);
    if (arg == "oob8b") return oob8(8);
    if (arg == "null8") return null8();
    if (arg == "ovf8") return ovf8();
    if (arg == "oob9") return oob9(0);
    if (arg == "oob9j") return oob9(1);
    if (arg == "oob9k") return oob9(2);
    if (arg == "oob9l") return oob9(3);
    if (arg == "oob9m") return oob9(4);
    if (arg == "oob9n") return oob9(5);
    if (arg == "oob9p") return oob9(6);
    if (arg == "oob9q") return oob9(7);
    if (arg == "oob9r") return oob9(8);
    if (arg == "oob9b") return oob9(9);
    if (arg == "null9") return null9();
    if (arg == "ovf9") return ovf9();
    if (arg == "oob10") return oob10(0);
    if (arg == "oob10j") return oob10(1);
    if (arg == "oob10k") return oob10(2);
    if (arg == "oob10l") return oob10(3);
    if (arg == "oob10m") return oob10(4);
    if (arg == "oob10n") return oob10(5);
    if (arg == "oob10p") return oob10(6);
    if (arg == "oob10q") return oob10(7);
    if (arg == "oob10r") return oob10(8);
    if (arg == "oob10s") return oob10(9);
    if (arg == "oob10b") return oob10(10);
    if (arg == "null10") return null10();
    if (arg == "ovf10") return ovf10();
    if (arg == "oob11") return oob11(0);
    if (arg == "oob11j") return oob11(1);
    if (arg == "oob11k") return oob11(2);
    if (arg == "oob11l") return oob11(3);
    if (arg == "oob11m") return oob11(4);
    if (arg == "oob11n") return oob11(5);
    if (arg == "oob11p") return oob11(6);
    if (arg == "oob11q") return oob11(7);
    if (arg == "oob11r") return oob11(8);
    if (arg == "oob11s") return oob11(9);
    if (arg == "oob11t") return oob11(10);
    if (arg == "oob11b") return oob11(11);
    if (arg == "null11") return null11();
    if (arg == "ovf11") return ovf11();
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
    rank8();
    zero8();
    empty8();
    deduce8();
    rank9();
    zero9();
    empty9();
    deduce9();
    rank10();
    zero10();
    empty10();
    deduce10();
    rank11();
    zero11();
    empty11();
    deduce11();
    return 0;
}
