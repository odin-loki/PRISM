// std::mdspan operational model (src/prism/pir/models/cxx/mdspan). The pir
// stage puts the model headers first on the include path, so <mdspan> below
// is the model. Each function is one contract. The view does not own the
// array. An index outside the extent is MEM-OOB-READ. A null pointer with a
// non-zero extent is PTR-NULL-DEREF. submdspan, layouts, and mdarray are
// not part of the model.
#include <mdspan>
#include <cassert>
#include <cstddef>

int mdspan_read_true(int i) {
    int a[4] = {1, 2, 3, 4};
    std::mdspan<int> m(a, 4);
    assert(m.size() == 4);
    assert(m.extent(0) == 4);
    assert(m.data() == a);
    if (i < 0 || i >= 4) return 0;
    auto k = static_cast<std::size_t>(i);
    assert(m(k) == a[k]);
    return m(k);
}

int mdspan_empty_true(int) {
    std::mdspan<int> m(static_cast<int*>(nullptr), 0);
    assert(m.size() == 0);
    assert(m.extent(0) == 0);
    assert(m.data() == nullptr);
    return 0;
}

// The view's lifetime ends. The array element is still there.
int mdspan_nonowning_true(int i) {
    int a[4] = {1, 2, 3, 4};
    int* p = a;
    {
        std::mdspan<int> m(p, 4);
        if (i >= 0 && i < 4) (void)m(static_cast<std::size_t>(i));
    }
    if (i < 0 || i >= 4) return 0;
    return p[i];
}

// operator() at size() (precondition: index < extent).
int mdspan_index_oob_false(int i) {
    if (i < 0 || i > 4) return 0;
    int a[4] = {10, 11, 12, 13};
    std::mdspan<int> m(a, 4);
    return m(static_cast<std::size_t>(i));
}

// A null data pointer with a non-zero extent. n == 0 is the empty view.
int mdspan_null_extent_false(int n) {
    if (n == 0) {
        std::mdspan<int> empty(static_cast<int*>(nullptr), 0);
        return static_cast<int>(empty.size());
    }
    std::mdspan<int> m(static_cast<int*>(nullptr), 1);
    return static_cast<int>(m.extent(0));
}
