// std::flat_set operational model (src/prism/pir/models/cxx/flat_set). The pir
// stage puts the model headers first on the include path, so <flat_set> below
// is the model. Each function is one contract. A missing key is end / false,
// which is defined: those checks are true. The false harnesses are a use of
// the pointer saved before erase, and a wrong claim that a duplicate insert
// grows size.
#include <cassert>
#include <flat_set>

struct FlatKey {
    int id;
    int tag;
    bool operator<(const FlatKey& o) const { return id < o.id; }
};

// One insert. contains is true. An empty container's begin equals end.
int flat_set_contains_true(int k) {
    std::flat_set<int> s;
    assert(s.begin() == s.end());
    assert(s.size() == 0);
    s.insert(k);
    assert(s.size() == 1);
    assert(s.contains(k));
    assert(s.find(k) != s.end());
    return *s.find(k);
}

// A second insert of an equivalent key does not grow size and does not
// replace the key that is already stored (tag stays 10).
int flat_set_duplicate_true(int tag) {
    std::flat_set<FlatKey> s;
    s.insert(FlatKey{1, 10});
    auto ins = s.insert(FlatKey{1, tag});
    assert(ins.second == false);
    assert(s.size() == 1);
    assert(s.contains(FlatKey{1, 0}));
    assert(s.find(FlatKey{1, 0})->tag == 10);
    return s.find(FlatKey{1, 0})->tag;
}

// A missing key is the end iterator, and contains is false. Neither call
// stores a key.
int flat_set_missing_true(int k) {
    std::flat_set<int> s;
    s.insert(1);
    int missing = k == 1 ? 2 : k;
    assert(s.find(missing) == s.end());
    assert(s.contains(missing) == false);
    assert(s.size() == 1);
    return 0;
}

// erase of a missing key frees nothing: the pointer to the stored key is
// still the stored key, and size is unchanged.
int flat_set_erase_missing_true(int k) {
    std::flat_set<int> s;
    s.insert(1);
    const int* p = &*s.begin();
    int missing = k == 1 ? 2 : k;
    assert(s.erase(missing) == 0);
    assert(s.size() == 1);
    assert(*p == 1);
    return *p;
}

// erase frees the node. The pointer saved before that erase is then used.
int flat_set_erase_uaf_false(int c) {
    std::flat_set<int> s;
    s.insert(1);
    const int* p = &*s.begin();
    s.erase(1);
    return *p + (c & 1);
}

// A wrong contract: a second insert of the same key grows size.
int flat_set_dup_size_false(int k) {
    std::flat_set<int> s;
    s.insert(k);
    s.insert(k);
    assert(s.size() == 2);
    return 0;
}
