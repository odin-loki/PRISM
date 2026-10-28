// Trace of std::flat_map. Compiled with PRISM's model header
// (src/prism/pir/models/cxx/flat_map) first on the include path.
// operator[] inserts one node; at() reads it. "miss" calls at() on an
// empty flat_map: the model throws and must not print a default mapped
// value. "uaf" keeps a reference and erases that key (the node is freed).
// "contains" looks up a missing key, then a present key and another
// missing key. Those lookups must not change size. "count" returns 0 for a
// missing key and 1 for a present key. Those calls use find: they must not
// allocate, insert, overwrite the mapped object, or construct a mapped
// value for a missing key. "try_emplace" inserts a missing key by
// constructing the mapped object in the new node, and a second call with
// that key must not allocate, must not construct another mapped object,
// and must not overwrite the stored value.
#include <flat_map>
#include <cstdio>
#include <cstring>
#include <stdexcept>

// Copy count for the contains driver. The model allocates a node only by
// constructing a key into it, so a contains that copies nothing did not
// allocate.
struct CountKey {
    int v;
    static int copies;
    static int moves;
    CountKey(int x) : v(x) {}
    CountKey(const CountKey& o) : v(o.v) { ++copies; }
    CountKey(CountKey&& o) noexcept : v(o.v) { ++moves; }
    bool operator<(const CountKey& o) const { return v < o.v; }
};
int CountKey::copies = 0;
int CountKey::moves = 0;

// Mapped-object counters. from_int is the in-node construction from the
// try_emplace arguments. A temporary mapped object would also move or copy.
struct CountMapped {
    int v;
    static int from_int;
    static int defaults;
    static int copies;
    static int moves;
    static int assigns;
    CountMapped() : v(-1) { ++defaults; }
    explicit CountMapped(int x) : v(x) { ++from_int; }
    CountMapped(const CountMapped& o) : v(o.v) { ++copies; }
    CountMapped(CountMapped&& o) noexcept : v(o.v) { ++moves; }
    CountMapped& operator=(const CountMapped& o) {
        v = o.v;
        ++assigns;
        return *this;
    }
    CountMapped& operator=(CountMapped&& o) noexcept {
        v = o.v;
        ++assigns;
        return *this;
    }
};
int CountMapped::from_int = 0;
int CountMapped::defaults = 0;
int CountMapped::copies = 0;
int CountMapped::moves = 0;
int CountMapped::assigns = 0;

struct ThrowMapped {
    static int throws_left;
    int v;
    explicit ThrowMapped(int x) : v(x) {
        if (throws_left > 0) {
            --throws_left;
            throw std::runtime_error("mapped");
        }
    }
};
int ThrowMapped::throws_left = 0;

static void show() {
    std::flat_map<int, int> m;
    std::printf("empty size=%zu\n", m.size());
    m[3] = 9;
    std::printf("at %d size=%zu\n", m.at(3), m.size());
    m[3] = 4;
    std::printf("replaced %d size=%zu\n", m.at(3), m.size());
    m[8] = 1;
    std::printf("two %d %d size=%zu\n", m.at(3), m.at(8), m.size());
    int present = static_cast<int>(m.erase(3));
    int missing = static_cast<int>(m.erase(3));
    std::printf("erase present=%d missing=%d size=%zu left=%d\n", present, missing, m.size(), m.at(8));
}

static void miss() {
    std::flat_map<int, int> m;
    try {
        int v = m.at(1);
        std::printf("fake=%d\n", v);
    } catch (const std::out_of_range&) {
        std::printf("threw\n");
    }
}

static void lookup() {
    std::flat_map<CountKey, int> m;
    const int base = CountKey::copies;
    const int miss0 = m.contains(4) ? 1 : 0;
    const int copies0 = CountKey::copies - base;
    const auto size0 = m.size();
    m[4] = 8;
    const auto size1 = m.size();
    const int mid = CountKey::copies;
    const int hit = m.contains(4) ? 1 : 0;
    const int miss = m.contains(5) ? 1 : 0;
    const int copies1 = CountKey::copies - mid;
    const auto size2 = m.size();
    std::printf("empty contains=%d copies=%d size=%zu inserted=%zu hit=%d missing=%d copies=%d size=%zu at=%d\n",
                miss0, copies0, size0, size1, hit, miss, copies1, size2, m.at(4));
}

static int mapped_activity() {
    return CountMapped::from_int + CountMapped::defaults + CountMapped::copies + CountMapped::moves +
           CountMapped::assigns;
}

static void count_trace() {
    CountKey::copies = 0;
    CountKey::moves = 0;
    CountMapped::from_int = 0;
    CountMapped::defaults = 0;
    CountMapped::copies = 0;
    CountMapped::moves = 0;
    CountMapped::assigns = 0;

    std::flat_map<CountKey, CountMapped> m;
    const int empty_count = static_cast<int>(m.count(4));
    const int empty_copies = CountKey::copies;
    const int empty_moves = CountKey::moves;
    const int empty_mapped = mapped_activity();
    const auto empty_size = m.size();

    m[4] = CountMapped(8);
    const int index_key_copies = CountKey::copies;
    const int index_key_moves = CountKey::moves;
    const int index_defaults = CountMapped::defaults;
    const int index_from = CountMapped::from_int;
    const int index_mapped_moves = CountMapped::moves;
    const int index_assigns = CountMapped::assigns;
    const auto index_size = m.size();
    const int index_value = m.at(4).v;

    const int copies_before = CountKey::copies;
    const int moves_before = CountKey::moves;
    const int from_before = CountMapped::from_int;
    const int defaults_before = CountMapped::defaults;
    const int mapped_copies_before = CountMapped::copies;
    const int mapped_moves_before = CountMapped::moves;
    const int assigns_before = CountMapped::assigns;
    const int hit = static_cast<int>(m.count(4));
    const int miss = static_cast<int>(m.count(5));
    const int copies_delta = CountKey::copies - copies_before;
    const int moves_delta = CountKey::moves - moves_before;
    const int from_delta = CountMapped::from_int - from_before;
    const int defaults_delta = CountMapped::defaults - defaults_before;
    const int mapped_copies_delta = CountMapped::copies - mapped_copies_before;
    const int mapped_moves_delta = CountMapped::moves - mapped_moves_before;
    const int assigns_delta = CountMapped::assigns - assigns_before;
    const int value_after = m.at(4).v;
    const auto size_after = m.size();
    const char* missing_at = "present";
    try {
        const int v = m.at(5).v;
        static_cast<void>(v);
    } catch (const std::out_of_range&) {
        missing_at = "threw";
    }

    std::printf("empty count=%d key_copies=%d key_moves=%d mapped=%d size=%zu\n", empty_count, empty_copies,
                empty_moves, empty_mapped, empty_size);
    std::printf(
        "index key_copies=%d key_moves=%d defaults=%d from_int=%d mapped_moves=%d assigns=%d size=%zu value=%d\n",
        index_key_copies, index_key_moves, index_defaults, index_from, index_mapped_moves, index_assigns, index_size,
        index_value);
    std::printf(
        "count hit=%d missing=%d key_copies=%d key_moves=%d from_int=%d defaults=%d mapped_copies=%d "
        "mapped_moves=%d assigns=%d value=%d size=%zu missing_at=%s\n",
        hit, miss, copies_delta, moves_delta, from_delta, defaults_delta, mapped_copies_delta, mapped_moves_delta,
        assigns_delta, value_after, size_after, missing_at);
}

static void try_emplace_trace() {
    CountKey::copies = 0;
    CountKey::moves = 0;
    CountMapped::from_int = 0;
    CountMapped::defaults = 0;
    CountMapped::copies = 0;
    CountMapped::moves = 0;
    CountMapped::assigns = 0;

    std::flat_map<CountKey, CountMapped> m;
    CountKey kept(1);
    const int ins = m.try_emplace(kept, 20) ? 1 : 0;
    const int hit = m.try_emplace(kept, 99) ? 1 : 0;
    const int key_copies = CountKey::copies;
    const int key_moves = CountKey::moves;
    const int from_int = CountMapped::from_int;
    const int defaults = CountMapped::defaults;
    const int mapped_copies = CountMapped::copies;
    const int mapped_moves = CountMapped::moves;
    const int assigns = CountMapped::assigns;
    const auto size_hit = m.size();
    const int value = m.at(kept).v;

    const int rv = m.try_emplace(CountKey(2), 7) ? 1 : 0;
    const int rv_dup = m.try_emplace(CountKey(2), 8) ? 1 : 0;
    const int rv_moves = CountKey::moves;
    const int rv_from = CountMapped::from_int;
    const int rv_mapped_moves = CountMapped::moves;
    const int rv_value = m.at(CountKey(2)).v;
    const auto size_rv = m.size();

    const int bare = m.try_emplace(CountKey(3)) ? 1 : 0;
    const int bare_dup = m.try_emplace(CountKey(3), 5) ? 1 : 0;
    const int bare_defaults = CountMapped::defaults;
    const int bare_from = CountMapped::from_int;
    const int bare_value = m.at(CountKey(3)).v;

    CountKey indexed(4);
    const int copies_before_index = CountKey::copies;
    const int defaults_before_index = CountMapped::defaults;
    m[indexed] = CountMapped(4);
    const int index_key_copies = CountKey::copies - copies_before_index;
    const int index_defaults = CountMapped::defaults - defaults_before_index;
    const auto size_index = m.size();

    std::printf(
        "lvalue inserted=%d hit=%d key_copies=%d key_moves=%d from_int=%d defaults=%d "
        "mapped_copies=%d mapped_moves=%d assigns=%d size=%zu value=%d\n",
        ins, hit, key_copies, key_moves, from_int, defaults, mapped_copies, mapped_moves, assigns, size_hit, value);
    std::printf(
        "rvalue inserted=%d dup=%d key_moves=%d from_int=%d mapped_moves=%d size=%zu value=%d\n",
        rv, rv_dup, rv_moves, rv_from, rv_mapped_moves, size_rv, rv_value);
    std::printf("bare inserted=%d dup=%d defaults=%d from_int=%d value=%d\n", bare, bare_dup, bare_defaults, bare_from,
                bare_value);
    std::printf("index key_copies=%d defaults=%d size=%zu at=%d\n", index_key_copies, index_defaults, size_index,
                m.at(indexed).v);

    const int copy_keys_before = CountKey::copies;
    const int copy_mapped_before = CountMapped::copies;
    std::flat_map<CountKey, CountMapped> copied(m);
    std::printf("copy size=%zu key_copies=%d mapped_copies=%d value=%d\n", copied.size(),
                CountKey::copies - copy_keys_before, CountMapped::copies - copy_mapped_before, copied.at(kept).v);

    std::flat_map<int, ThrowMapped> thrown;
    ThrowMapped::throws_left = 1;
    try {
        const int bad = thrown.try_emplace(1, 2) ? 1 : 0;
        std::printf("throw inserted=%d size=%zu\n", bad, thrown.size());
    } catch (const std::runtime_error&) {
        std::printf("throw size=%zu\n", thrown.size());
    }
    const int after = thrown.try_emplace(1, 2) ? 1 : 0;
    std::printf("throw retry inserted=%d size=%zu value=%d\n", after, thrown.size(), thrown.at(1).v);

    std::flat_map<int, int> plain;
    plain[3] = 9;
    const int plain_hit = plain.try_emplace(3, 1) ? 1 : 0;
    const int plain_new = plain.try_emplace(4, 8) ? 1 : 0;
    const int erased_missing = static_cast<int>(plain.erase(5));
    const int erased_present = static_cast<int>(plain.erase(4));
    int saw = 0;
    try {
        saw = plain.at(6);
    } catch (const std::out_of_range&) {
        saw = -1;
    }
    std::printf("plain hit=%d inserted=%d at=%d erase_missing=%d erase_present=%d missing_at=%d size=%zu\n", plain_hit,
                plain_new, plain.at(3), erased_missing, erased_present, saw, plain.size());
}

static int uaf() {
    std::flat_map<int, int> m;
    m[1] = 5;
    int& r = m.at(1);
    m.erase(1);
    return r;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "miss") == 0) {
        miss();
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "uaf") == 0) return uaf();
    if (argc > 1 && std::strcmp(argv[1], "contains") == 0) {
        lookup();
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "count") == 0) {
        count_trace();
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "try_emplace") == 0) {
        try_emplace_trace();
        return 0;
    }
    show();
    return 0;
}
