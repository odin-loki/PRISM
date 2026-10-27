// std::flat_map operational model (src/prism/pir/models/cxx/flat_map).
// The pir stage puts the model headers first on the include path, so
// <flat_map> below is the model. Each function is one contract. at() on an
// empty container throws; it does not hand back a default mapped value.
// erase of a present key frees that node. erase of a missing key frees
// nothing. insert ranges, try_emplace, and the rest are not part of the
// model.
#include <flat_map>
#include <cassert>

// operator[] inserts one node; at() reads that mapped value.
int flat_map_at_true(int k) {
    std::flat_map<int, int> m;
    m[k] = 7;
    assert(m.size() == 1u);
    assert(m.at(k) == 7);
    assert(m.erase(k) == 1u);
    assert(m.size() == 0u);
    assert(m.erase(k) == 0u);
    assert(m.size() == 0u);
    return 0;
}

// erase of a key that was never inserted frees nothing and leaves the node.
int flat_map_erase_missing_true(int k) {
    std::flat_map<int, int> m;
    m[k] = 4;
    int missing = k == 0 ? 1 : 0;
    assert(m.erase(missing) == 0u);
    assert(m.size() == 1u);
    assert(m.at(k) == 4);
    return 0;
}

// at() on an empty flat_map throws std::out_of_range out of noexcept.
int flat_map_empty_at_false(int k) noexcept {
    std::flat_map<int, int> m;
    return m.at(k);
}

// The reference names the erased node. erase frees that node.
int flat_map_erase_uaf_false(int k) {
    std::flat_map<int, int> m;
    m[k] = 5;
    int& r = m.at(k);
    m.erase(k);
    return r;
}
