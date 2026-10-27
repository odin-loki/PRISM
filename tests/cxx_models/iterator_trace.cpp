// Trace of std::begin / std::end on an array. `#include <iterator>` is the
// model when its directory is first on the include path, and libstdc++
// otherwise. begin is a pointer to the first element. end is one past the
// last. An empty array compares those pointers and does not dereference
// them. A pointer argument is not called: it is not an overload of these
// functions. This is not a proof that an iterator stays in range.
#include <cstdio>
#include <iterator>

template <class T>
constexpr bool prism_accepts_begin = requires(T& r) { std::begin(r); };

static_assert(prism_accepts_begin<int[3]>);
static_assert(!prism_accepts_begin<int*>);
static_assert(!prism_accepts_begin<const int*>);

int main() {
    int a[3] = {4, 5, 6};
    int* b = std::begin(a);
    int* e = std::end(a);
    std::printf("len %td first %d last %d\n", e - b, *b, *(e - 1));

    int none[0] = {};
    int* nb = std::begin(none);
    int* ne = std::end(none);
    std::printf("empty %d\n", nb == ne ? 1 : 0);

    const int c[2] = {8, 9};
    const int* cb = std::begin(c);
    const int* ce = std::end(c);
    std::printf("const %td %d\n", ce - cb, *cb);

    int x = 1;
    int y = 2;
    int* ps[2] = {&x, &y};
    int** pb = std::begin(ps);
    int** pe = std::end(ps);
    std::printf("ptrs %td %d\n", pe - pb, **pb);

    const bool ok = (e - b == 3) && (*b == 4) && (*(e - 1) == 6) && (nb == ne) && (ce - cb == 2) &&
                    (*cb == 8) && (pe - pb == 2) && (**pb == 1);
    return ok ? 0 : 1;
}
