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
// and must not overwrite the stored value. "empty" is true when there are
// no nodes and false when there is at least one. Those calls read the
// existing size: they must not allocate, insert, erase, construct a mapped
// value, or call operator[] or at(). "swap" exchanges the node pointer and
// the size. Two empty maps swap to empty and do not load a node. Swap does
// not allocate, free, or construct a key. Self-swap does not free the nodes.
// "less" orders two flat_maps along the node chain, newest pair first. Two
// empty maps compare not-less and do not load a node. An empty map is less
// than a non-empty map, and that path does not load the empty map's node.
// The walk stops at the first deciding pair and does not read past the
// shorter map. It does not return T{}. "greater" is that order with the
// arguments swapped: operator> returns __y < __x and uses the existing
// operator<. Two empty maps compare not-greater and do not load a node. A
// non-empty map is greater than an empty map, and that path does not load
// the empty map. It does not return T{}. CountKey and CountMapped define
// operator< and do not define operator>. "equal" compares two flat_maps
// with == and != (the C++20 rewrite of !(a == b)). Two empty maps compare
// equal and do not load a node. Different sizes compare unequal before any
// key or mapped comparison. The same pairs inserted in different orders
// compare equal: each pair is looked up by key, not paired by link
// position. A missing key or a mismatch returns false. Nothing is
// constructed. The no-argument lines are unchanged. This trace is not
// PROVED and not CLEAN.
#include <flat_map>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <type_traits>

// Copy count for the contains driver. The model allocates a node only by
// constructing a key into it, so a contains that copies nothing did not
// allocate.
struct CountKey {
    int v;
    static int copies;
    static int moves;
    static int eq;
    static int lt;
    static int poison;
    CountKey(int x) : v(x) {}
    CountKey(const CountKey& o) : v(o.v) { ++copies; }
    CountKey(CountKey&& o) noexcept : v(o.v) { ++moves; }
    bool operator<(const CountKey& o) const {
        ++lt;
        if (v < 0 || o.v < 0) ++poison;
        return v < o.v;
    }
    bool operator==(const CountKey& o) const {
        ++eq;
        return v == o.v;
    }
};
int CountKey::copies = 0;
int CountKey::moves = 0;
int CountKey::eq = 0;
int CountKey::lt = 0;
int CountKey::poison = 0;

// Mapped-object counters. from_int is the in-node construction from the
// try_emplace arguments. A temporary mapped object would also move or copy.
struct CountMapped {
    int v;
    static int from_int;
    static int defaults;
    static int copies;
    static int moves;
    static int assigns;
    static int eq;
    static int lt;
    static int poison;
    CountMapped() : v(-1) { ++defaults; }
    explicit CountMapped(int x) : v(x) { ++from_int; }
    CountMapped(const CountMapped& o) : v(o.v) { ++copies; }
    CountMapped(CountMapped&& o) noexcept : v(o.v) { ++moves; }
    bool operator==(const CountMapped& o) const {
        ++eq;
        return v == o.v;
    }
    bool operator<(const CountMapped& o) const {
        ++lt;
        if (v < 0 || o.v < 0) ++poison;
        return v < o.v;
    }
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
int CountMapped::eq = 0;
int CountMapped::lt = 0;
int CountMapped::poison = 0;

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

static void reset_counts() {
    CountKey::copies = 0;
    CountKey::moves = 0;
    CountKey::eq = 0;
    CountKey::lt = 0;
    CountKey::poison = 0;
    CountMapped::from_int = 0;
    CountMapped::defaults = 0;
    CountMapped::copies = 0;
    CountMapped::moves = 0;
    CountMapped::assigns = 0;
    CountMapped::eq = 0;
    CountMapped::lt = 0;
    CountMapped::poison = 0;
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

static void empty_trace() {
    CountKey::copies = 0;
    CountKey::moves = 0;
    CountMapped::from_int = 0;
    CountMapped::defaults = 0;
    CountMapped::copies = 0;
    CountMapped::moves = 0;
    CountMapped::assigns = 0;

    std::flat_map<CountKey, CountMapped> m;
    const int none = m.empty() ? 1 : 0;
    const int none_copies = CountKey::copies;
    const int none_moves = CountKey::moves;
    const int none_mapped = mapped_activity();
    const auto none_size = m.size();

    m[4] = CountMapped(8);
    const int copies_before = CountKey::copies;
    const int moves_before = CountKey::moves;
    const int from_before = CountMapped::from_int;
    const int defaults_before = CountMapped::defaults;
    const int mapped_copies_before = CountMapped::copies;
    const int mapped_moves_before = CountMapped::moves;
    const int assigns_before = CountMapped::assigns;
    const int some = m.empty() ? 1 : 0;
    const int copies_delta = CountKey::copies - copies_before;
    const int moves_delta = CountKey::moves - moves_before;
    const int from_delta = CountMapped::from_int - from_before;
    const int defaults_delta = CountMapped::defaults - defaults_before;
    const int mapped_copies_delta = CountMapped::copies - mapped_copies_before;
    const int mapped_moves_delta = CountMapped::moves - mapped_moves_before;
    const int assigns_delta = CountMapped::assigns - assigns_before;
    const auto some_size = m.size();
    const int value = m.at(4).v;

    const int erased = static_cast<int>(m.erase(4));
    const int after_copies = CountKey::copies;
    const int after_moves = CountKey::moves;
    const int after_mapped = mapped_activity();
    const int again = m.empty() ? 1 : 0;
    const int erase_copies = CountKey::copies - after_copies;
    const int erase_moves = CountKey::moves - after_moves;
    const int erase_mapped = mapped_activity() - after_mapped;
    const auto again_size = m.size();
    const char* missing_at = "present";
    try {
        const int v = m.at(4).v;
        static_cast<void>(v);
    } catch (const std::out_of_range&) {
        missing_at = "threw";
    }

    std::flat_map<int, int> plain;
    const int plain_none = plain.empty() ? 1 : 0;
    plain[1] = 5;
    const std::flat_map<int, int>& held = plain;
    const int plain_some = held.empty() ? 1 : 0;
    plain.erase(1);
    const int plain_again = plain.empty() ? 1 : 0;

    std::printf("none empty=%d key_copies=%d key_moves=%d mapped=%d size=%zu\n", none, none_copies, none_moves,
                none_mapped, none_size);
    std::printf(
        "some empty=%d key_copies=%d key_moves=%d from_int=%d defaults=%d mapped_copies=%d mapped_moves=%d "
        "assigns=%d size=%zu value=%d\n",
        some, copies_delta, moves_delta, from_delta, defaults_delta, mapped_copies_delta, mapped_moves_delta,
        assigns_delta, some_size, value);
    std::printf("erased empty=%d erased=%d key_copies=%d key_moves=%d mapped=%d size=%zu missing_at=%s\n", again,
                erased, erase_copies, erase_moves, erase_mapped, again_size, missing_at);
    std::printf("plain none=%d some=%d again=%d size=%zu\n", plain_none, plain_some, plain_again, plain.size());
}

static void swap_trace() {
    std::flat_map<int, int> e1;
    std::flat_map<int, int> e2;
    static_assert(std::is_void_v<decltype(e1.swap(e2))>);
    static_assert(noexcept(e1.swap(e2)));
    e1.swap(e2);
    std::printf("empty a=%d b=%d asize=%zu bsize=%zu\n", e1.empty() ? 1 : 0, e2.empty() ? 1 : 0, e1.size(),
                e2.size());

    std::flat_map<int, int> left;
    std::flat_map<int, int> right;
    left[1] = 10;
    left[2] = 20;
    right[3] = 30;
    left.swap(right);
    const char* left_old = "present";
    try {
        const int v = left.at(1);
        static_cast<void>(v);
    } catch (const std::out_of_range&) {
        left_old = "threw";
    }
    std::printf("exch left=%d lsize=%zu right1=%d right2=%d rsize=%zu left_old=%s\n", left.at(3), left.size(),
                right.at(1), right.at(2), right.size(), left_old);
    left.swap(right);
    std::printf("back left1=%d left2=%d lsize=%zu right=%d rsize=%zu\n", left.at(1), left.at(2), left.size(),
                right.at(3), right.size());
    left.swap(left);
    std::printf("self left1=%d lsize=%zu empty=%d\n", left.at(1), left.size(), left.empty() ? 1 : 0);

    std::flat_map<int, int> hole;
    left.swap(hole);
    std::printf("hole leftempty=%d lsize=%zu hole1=%d hsize=%zu\n", left.empty() ? 1 : 0, left.size(), hole.at(1),
                hole.size());

    reset_counts();
    std::flat_map<CountKey, CountMapped> k1;
    std::flat_map<CountKey, CountMapped> k2;
    k1.swap(k2);
    const int kempty_copies = CountKey::copies;
    const int kempty_moves = CountKey::moves;
    const int kempty_mapped = mapped_activity();
    const int kempty_eq = CountKey::eq + CountMapped::eq;
    const int kempty_a = k1.empty() ? 1 : 0;
    const int kempty_b = k2.empty() ? 1 : 0;

    k1[CountKey(1)] = CountMapped(8);
    k2[CountKey(2)] = CountMapped(9);
    reset_counts();
    k1.swap(k2);
    const int kex_copies = CountKey::copies;
    const int kex_moves = CountKey::moves;
    const int kex_mapped = mapped_activity();
    const int kex_eq = CountKey::eq + CountMapped::eq;
    const int kex_left = k1.at(CountKey(2)).v;
    const int kex_right = k2.at(CountKey(1)).v;
    const auto kex_lsize = k1.size();
    const auto kex_rsize = k2.size();

    reset_counts();
    k1.swap(k1);
    const int kself_copies = CountKey::copies;
    const int kself_moves = CountKey::moves;
    const int kself_mapped = mapped_activity();
    const int kself_value = k1.at(CountKey(2)).v;
    const auto kself_size = k1.size();

    std::printf("kempty a=%d b=%d copies=%d moves=%d mapped=%d eq=%d\n", kempty_a, kempty_b, kempty_copies,
                kempty_moves, kempty_mapped, kempty_eq);
    std::printf("kex left=%d right=%d lsize=%zu rsize=%zu copies=%d moves=%d mapped=%d eq=%d\n", kex_left, kex_right,
                kex_lsize, kex_rsize, kex_copies, kex_moves, kex_mapped, kex_eq);
    std::printf("kself value=%d size=%zu copies=%d moves=%d mapped=%d\n", kself_value, kself_size, kself_copies,
                kself_moves, kself_mapped);
}

static void show_less(const char* name, int result) {
    std::printf("%s %d key_lt=%d mapped_lt=%d poison=%d copies=%d moves=%d mapped=%d\n", name, result, CountKey::lt,
                CountMapped::lt, CountKey::poison + CountMapped::poison, CountKey::copies + CountMapped::copies,
                CountKey::moves + CountMapped::moves, mapped_activity());
}

static void less_trace() {
    std::flat_map<int, int> e1;
    std::flat_map<int, int> e2;
    const int empty = (e1 < e2) ? 1 : 0;
    std::flat_map<int, int> one;
    one[1] = 10;
    const int el = (e1 < one) ? 1 : 0;
    const int le = (one < e1) ? 1 : 0;
    std::flat_map<int, int> one_b;
    one_b[1] = 10;
    const int oneeq = (one < one_b) ? 1 : 0;
    std::flat_map<int, int> key_a;
    std::flat_map<int, int> key_b;
    key_a[2] = 20;
    key_b[1] = 10;
    const int key_ab = (key_a < key_b) ? 1 : 0;
    const int key_ba = (key_b < key_a) ? 1 : 0;
    std::flat_map<int, int> map_a;
    std::flat_map<int, int> map_b;
    map_a[1] = 10;
    map_b[1] = 20;
    const int map_ab = (map_a < map_b) ? 1 : 0;
    const int map_ba = (map_b < map_a) ? 1 : 0;
    std::flat_map<int, int> pre_s;
    std::flat_map<int, int> pre_l;
    pre_s[1] = 10;
    pre_l[9] = 90;
    pre_l[1] = 10;
    const int pre = (pre_s < pre_l) ? 1 : 0;
    const int revpre = (pre_l < pre_s) ? 1 : 0;
    std::flat_map<int, int> early_a;
    std::flat_map<int, int> early_b;
    early_a[1] = 10;
    early_a[5] = 50;
    early_b[9] = 90;
    early_b[1] = 10;
    early_b[4] = 40;
    const int early = (early_a < early_b) ? 1 : 0;
    const int early_ba = (early_b < early_a) ? 1 : 0;
    std::flat_map<int, int> tail_a;
    std::flat_map<int, int> tail_b;
    tail_a[8] = 80;
    tail_a[1] = 10;
    tail_b[7] = 70;
    tail_b[1] = 10;
    const int tail_ab = (tail_a < tail_b) ? 1 : 0;
    const int tail_ba = (tail_b < tail_a) ? 1 : 0;
    std::printf("empty %d\n", empty);
    std::printf("el %d\n", el);
    std::printf("le %d\n", le);
    std::printf("oneeq %d\n", oneeq);
    std::printf("key %d %d\n", key_ab, key_ba);
    std::printf("mapped %d %d\n", map_ab, map_ba);
    std::printf("prefix %d %d\n", pre, revpre);
    std::printf("early %d %d\n", early, early_ba);
    std::printf("tail %d %d\n", tail_ab, tail_ba);

    reset_counts();
    std::flat_map<CountKey, CountMapped> ke1;
    std::flat_map<CountKey, CountMapped> ke2;
    show_less("kempty", (ke1 < ke2) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> kone;
    (void)kone.try_emplace(CountKey(1), 10);
    reset_counts();
    show_less("kel", (ke1 < kone) ? 1 : 0);
    reset_counts();
    show_less("kle", (kone < ke1) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> kone_b;
    (void)kone_b.try_emplace(CountKey(1), 10);
    reset_counts();
    show_less("ksame", (kone < kone_b) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> ktwo_a;
    std::flat_map<CountKey, CountMapped> ktwo_b;
    (void)ktwo_a.try_emplace(CountKey(1), 10);
    (void)ktwo_a.try_emplace(CountKey(2), 20);
    (void)ktwo_b.try_emplace(CountKey(1), 10);
    (void)ktwo_b.try_emplace(CountKey(2), 20);
    reset_counts();
    show_less("ktwo", (ktwo_a < ktwo_b) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> kkey_a;
    std::flat_map<CountKey, CountMapped> kkey_b;
    (void)kkey_a.try_emplace(CountKey(-1), -1);
    (void)kkey_a.try_emplace(CountKey(5), 50);
    (void)kkey_b.try_emplace(CountKey(-1), -1);
    (void)kkey_b.try_emplace(CountKey(4), 40);
    reset_counts();
    show_less("kkey", (kkey_a < kkey_b) ? 1 : 0);
    reset_counts();
    show_less("kkeyba", (kkey_b < kkey_a) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> kmap_a;
    std::flat_map<CountKey, CountMapped> kmap_b;
    (void)kmap_a.try_emplace(CountKey(-1), -1);
    (void)kmap_a.try_emplace(CountKey(2), 20);
    (void)kmap_b.try_emplace(CountKey(-1), -1);
    (void)kmap_b.try_emplace(CountKey(2), 30);
    reset_counts();
    show_less("kmap", (kmap_a < kmap_b) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> kpre_s;
    std::flat_map<CountKey, CountMapped> kpre_l;
    (void)kpre_s.try_emplace(CountKey(1), 10);
    (void)kpre_l.try_emplace(CountKey(-1), -1);
    (void)kpre_l.try_emplace(CountKey(1), 10);
    reset_counts();
    show_less("kpre", (kpre_s < kpre_l) ? 1 : 0);
    reset_counts();
    show_less("krev", (kpre_l < kpre_s) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> kearly_a;
    std::flat_map<CountKey, CountMapped> kearly_b;
    (void)kearly_a.try_emplace(CountKey(1), 10);
    (void)kearly_a.try_emplace(CountKey(5), 50);
    (void)kearly_b.try_emplace(CountKey(-1), -1);
    (void)kearly_b.try_emplace(CountKey(1), 10);
    (void)kearly_b.try_emplace(CountKey(4), 40);
    reset_counts();
    show_less("kearly", (kearly_a < kearly_b) ? 1 : 0);
    reset_counts();
    show_less("kearlyba", (kearly_b < kearly_a) ? 1 : 0);
}

static void show_greater(const char* name, int result) {
    std::printf("%s %d key_lt=%d mapped_lt=%d poison=%d copies=%d moves=%d mapped=%d\n", name, result, CountKey::lt,
                CountMapped::lt, CountKey::poison + CountMapped::poison, CountKey::copies + CountMapped::copies,
                CountKey::moves + CountMapped::moves, mapped_activity());
}

static void greater_trace() {
    std::flat_map<int, int> e1;
    std::flat_map<int, int> e2;
    const int empty = (e1 > e2) ? 1 : 0;
    std::flat_map<int, int> one;
    one[1] = 10;
    const int eg = (e1 > one) ? 1 : 0;
    const int ge = (one > e1) ? 1 : 0;
    std::flat_map<int, int> one_b;
    one_b[1] = 10;
    const int oneeq = (one > one_b) ? 1 : 0;
    std::flat_map<int, int> key_a;
    std::flat_map<int, int> key_b;
    key_a[2] = 20;
    key_b[1] = 10;
    const int key_ab = (key_a > key_b) ? 1 : 0;
    const int key_ba = (key_b > key_a) ? 1 : 0;
    std::flat_map<int, int> map_a;
    std::flat_map<int, int> map_b;
    map_a[1] = 10;
    map_b[1] = 20;
    const int map_ab = (map_a > map_b) ? 1 : 0;
    const int map_ba = (map_b > map_a) ? 1 : 0;
    std::flat_map<int, int> pre_s;
    std::flat_map<int, int> pre_l;
    pre_s[1] = 10;
    pre_l[9] = 90;
    pre_l[1] = 10;
    const int pre = (pre_s > pre_l) ? 1 : 0;
    const int revpre = (pre_l > pre_s) ? 1 : 0;
    std::flat_map<int, int> early_a;
    std::flat_map<int, int> early_b;
    early_a[1] = 10;
    early_a[5] = 50;
    early_b[9] = 90;
    early_b[1] = 10;
    early_b[4] = 40;
    const int early = (early_a > early_b) ? 1 : 0;
    const int early_ba = (early_b > early_a) ? 1 : 0;
    std::flat_map<int, int> tail_a;
    std::flat_map<int, int> tail_b;
    tail_a[8] = 80;
    tail_a[1] = 10;
    tail_b[7] = 70;
    tail_b[1] = 10;
    const int tail_ab = (tail_a > tail_b) ? 1 : 0;
    const int tail_ba = (tail_b > tail_a) ? 1 : 0;
    std::printf("empty %d\n", empty);
    std::printf("eg %d\n", eg);
    std::printf("ge %d\n", ge);
    std::printf("oneeq %d\n", oneeq);
    std::printf("key %d %d\n", key_ab, key_ba);
    std::printf("mapped %d %d\n", map_ab, map_ba);
    std::printf("prefix %d %d\n", pre, revpre);
    std::printf("early %d %d\n", early, early_ba);
    std::printf("tail %d %d\n", tail_ab, tail_ba);

    reset_counts();
    std::flat_map<CountKey, CountMapped> ke1;
    std::flat_map<CountKey, CountMapped> ke2;
    show_greater("kempty", (ke1 > ke2) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> kone;
    (void)kone.try_emplace(CountKey(1), 10);
    reset_counts();
    show_greater("keg", (ke1 > kone) ? 1 : 0);
    reset_counts();
    show_greater("kge", (kone > ke1) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> kone_b;
    (void)kone_b.try_emplace(CountKey(1), 10);
    reset_counts();
    show_greater("ksame", (kone > kone_b) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> ktwo_a;
    std::flat_map<CountKey, CountMapped> ktwo_b;
    (void)ktwo_a.try_emplace(CountKey(1), 10);
    (void)ktwo_a.try_emplace(CountKey(2), 20);
    (void)ktwo_b.try_emplace(CountKey(1), 10);
    (void)ktwo_b.try_emplace(CountKey(2), 20);
    reset_counts();
    show_greater("ktwo", (ktwo_a > ktwo_b) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> kkey_a;
    std::flat_map<CountKey, CountMapped> kkey_b;
    (void)kkey_a.try_emplace(CountKey(-1), -1);
    (void)kkey_a.try_emplace(CountKey(5), 50);
    (void)kkey_b.try_emplace(CountKey(-1), -1);
    (void)kkey_b.try_emplace(CountKey(4), 40);
    reset_counts();
    show_greater("kkey", (kkey_a > kkey_b) ? 1 : 0);
    reset_counts();
    show_greater("kkeyba", (kkey_b > kkey_a) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> kmap_a;
    std::flat_map<CountKey, CountMapped> kmap_b;
    (void)kmap_a.try_emplace(CountKey(-1), -1);
    (void)kmap_a.try_emplace(CountKey(2), 20);
    (void)kmap_b.try_emplace(CountKey(-1), -1);
    (void)kmap_b.try_emplace(CountKey(2), 30);
    reset_counts();
    show_greater("kmap", (kmap_a > kmap_b) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> kpre_s;
    std::flat_map<CountKey, CountMapped> kpre_l;
    (void)kpre_s.try_emplace(CountKey(1), 10);
    (void)kpre_l.try_emplace(CountKey(-1), -1);
    (void)kpre_l.try_emplace(CountKey(1), 10);
    reset_counts();
    show_greater("kpre", (kpre_s > kpre_l) ? 1 : 0);
    reset_counts();
    show_greater("krev", (kpre_l > kpre_s) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> kearly_a;
    std::flat_map<CountKey, CountMapped> kearly_b;
    (void)kearly_a.try_emplace(CountKey(1), 10);
    (void)kearly_a.try_emplace(CountKey(5), 50);
    (void)kearly_b.try_emplace(CountKey(-1), -1);
    (void)kearly_b.try_emplace(CountKey(1), 10);
    (void)kearly_b.try_emplace(CountKey(4), 40);
    reset_counts();
    show_greater("kearly", (kearly_a > kearly_b) ? 1 : 0);
    reset_counts();
    show_greater("kearlyba", (kearly_b > kearly_a) ? 1 : 0);
}

static void show_equal(const char* tag, int r) {
    std::printf("%s %d key_eq=%d mapped_eq=%d key_lt=%d copies=%d moves=%d mapped=%d\n", tag, r, CountKey::eq,
                CountMapped::eq, CountKey::lt, CountKey::copies + CountMapped::copies,
                CountKey::moves + CountMapped::moves, mapped_activity());
}

static void equal_trace() {
    std::flat_map<int, int> a;
    std::flat_map<int, int> b;
    std::printf("empty %d ne %d\n", (a == b) ? 1 : 0, (a != b) ? 1 : 0);
    a[1] = 10;
    std::printf("short %d ne %d rev %d\n", (a == b) ? 1 : 0, (a != b) ? 1 : 0, (b == a) ? 1 : 0);
    b[1] = 10;
    std::printf("one %d ne %d\n", (a == b) ? 1 : 0, (a != b) ? 1 : 0);
    b[1] = 11;
    std::printf("miss %d ne %d\n", (a == b) ? 1 : 0, (a != b) ? 1 : 0);
    b[1] = 10;
    a[2] = 20;
    b[2] = 20;
    std::printf("same %d ne %d\n", (a == b) ? 1 : 0, (a != b) ? 1 : 0);
    std::flat_map<int, int> rev;
    rev[2] = 20;
    rev[1] = 10;
    std::printf("order %d ne %d rev %d\n", (a == rev) ? 1 : 0, (a != rev) ? 1 : 0, (rev == a) ? 1 : 0);
    std::flat_map<int, int> other;
    other[1] = 10;
    other[3] = 20;
    std::printf("keydiff %d ne %d\n", (a == other) ? 1 : 0, (a != other) ? 1 : 0);
    std::flat_map<int, int> il{{3, 30}, {1, 10}, {2, 20}};
    std::flat_map<int, int> built;
    built[2] = 20;
    built[3] = 30;
    built[1] = 10;
    std::printf("three %d ne %d\n", (il == built) ? 1 : 0, (il != built) ? 1 : 0);
    a[3] = 30;
    std::printf("longer %d ne %d\n", (a == b) ? 1 : 0, (a != b) ? 1 : 0);
    a.erase(3);
    std::printf("erased %d ne %d\n", (a == b) ? 1 : 0, (a != b) ? 1 : 0);

    reset_counts();
    std::flat_map<CountKey, CountMapped> e1;
    std::flat_map<CountKey, CountMapped> e2;
    show_equal("kempty", (e1 == e2) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> one_map;
    std::flat_map<CountKey, CountMapped> two_map;
    (void)one_map.try_emplace(CountKey(1), 10);
    (void)two_map.try_emplace(CountKey(1), 10);
    (void)two_map.try_emplace(CountKey(2), 20);
    reset_counts();
    show_equal("kshort", (two_map == one_map) ? 1 : 0);
    reset_counts();
    show_equal("kshortne", (two_map != one_map) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> left;
    std::flat_map<CountKey, CountMapped> right;
    (void)left.try_emplace(CountKey(1), 10);
    (void)left.try_emplace(CountKey(2), 20);
    (void)right.try_emplace(CountKey(2), 20);
    (void)right.try_emplace(CountKey(1), 10);
    reset_counts();
    show_equal("korder", (left == right) ? 1 : 0);
    reset_counts();
    show_equal("korderne", (left != right) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> stop_a;
    std::flat_map<CountKey, CountMapped> stop_b;
    (void)stop_a.try_emplace(CountKey(1), 10);
    (void)stop_a.try_emplace(CountKey(2), 20);
    (void)stop_a.try_emplace(CountKey(3), 30);
    (void)stop_b.try_emplace(CountKey(1), 10);
    (void)stop_b.try_emplace(CountKey(8), 80);
    (void)stop_b.try_emplace(CountKey(3), 30);
    reset_counts();
    show_equal("kstop", (stop_a == stop_b) ? 1 : 0);

    std::flat_map<CountKey, CountMapped> mstop_a;
    std::flat_map<CountKey, CountMapped> mstop_b;
    (void)mstop_a.try_emplace(CountKey(1), 10);
    (void)mstop_a.try_emplace(CountKey(2), 20);
    (void)mstop_a.try_emplace(CountKey(3), 30);
    (void)mstop_b.try_emplace(CountKey(1), 10);
    (void)mstop_b.try_emplace(CountKey(2), 99);
    (void)mstop_b.try_emplace(CountKey(3), 30);
    reset_counts();
    show_equal("mstop", (mstop_a == mstop_b) ? 1 : 0);
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
    if (argc > 1 && std::strcmp(argv[1], "empty") == 0) {
        empty_trace();
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "swap") == 0) {
        swap_trace();
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "less") == 0) {
        less_trace();
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "greater") == 0) {
        greater_trace();
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "equal") == 0) {
        equal_trace();
        return 0;
    }
    show();
    return 0;
}
