// Trace of std::flat_map. Compiled with PRISM's model header
// (src/prism/pir/models/cxx/flat_map) first on the include path.
// operator[] inserts one node; at() reads it. "miss" calls at() on an
// empty flat_map: the model throws and must not print a default mapped
// value. "uaf" keeps a reference and erases that key (the node is freed).
#include <flat_map>
#include <cstdio>
#include <cstring>
#include <stdexcept>

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
    show();
    return 0;
}
