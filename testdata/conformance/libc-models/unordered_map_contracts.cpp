// std::unordered_map / std::unordered_set models (prism_hash_table.h, unordered_map,
// unordered_set) against C++23 [unord.req]. See docs/PIR.md "C++ library models".
#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>

int umap_insert_true(int a, int b) {
    std::unordered_map<int, int> m;
    bool ia = m.insert({a, 1}).second;
    bool ib = m.insert({b, 2}).second;
    assert(ia);
    assert(ib == (b != a));
    assert(m.size() == (a == b ? 1u : 2u));
    assert(m.find(a)->second == 1);
    return 0;
}

int umap_index_true(int a, int b) {
    std::unordered_map<int, int> m;
    m[a] = 5;
    int x = m[b];
    assert(x == (a == b ? 5 : 0));
    assert(m.size() == (a == b ? 1u : 2u));
    assert(m.find(b) != m.end());
    return 0;
}

int umap_erase_true(int v) {
    std::unordered_map<int, int> m;
    m[10] = v;
    m[20] = 2;
    assert(m.erase(20) == 1);
    assert(m.erase(20) == 0);
    assert(m.size() == 1);
    assert(m.find(10)->second == v);
    return 0;
}

int umap_at_true(int k) {
    std::unordered_map<int, int> m;
    m[1] = 10;
    m[2] = 20;
    bool threw = false;
    try {
        (void)m.at(k);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(threw == (k != 1 && k != 2));
    return 0;
}

int umap_erase_uaf_false(int c) {
    std::unordered_map<int, int> m;
    m[1] = 10;
    auto it = m.find(1);
    m.erase(1);
    return it->second + (c & 1);
}

int umap_end_deref_false(int k) {
    std::unordered_map<int, int> m;
    m[1] = 10;
    return m.find(k)->second;
}

int uset_unique_true(int a, int b) {
    std::unordered_set<int> s;
    s.insert(a);
    s.insert(b);
    assert(s.size() == (a == b ? 1u : 2u));
    assert(s.count(a) == 1);
    return 0;
}

int umap_load_factor_true(int n) {
    std::unordered_map<int, int> m;
    for (int i = 0; i < n && i < 5; ++i) m[i] = i;
    assert(m.load_factor() > 0.f || m.empty());
    assert(m.bucket_count() >= 1);
    return 0;
}
