// Trace of std::flat_map. Compiled with PRISM's model header
// (src/prism/pir/models/cxx/flat_map) first on the include path.
// operator[] inserts one node; at() reads it. "miss" calls at() on an
// empty flat_map: the model throws and must not print a default mapped
// value. "uaf" keeps a reference and erases that key (the node is freed).
// "contains" looks up a missing key, then a present key and another
// missing key. Those lookups must not change size.
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
    CountKey(int x) : v(x) {}
    CountKey(const CountKey& o) : v(o.v) { ++copies; }
    bool operator<(const CountKey& o) const { return v < o.v; }
};
int CountKey::copies = 0;

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
    show();
    return 0;
}
