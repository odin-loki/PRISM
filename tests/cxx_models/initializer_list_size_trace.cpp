// size() and empty() on the initializer_list model
// (tests/cpp/test_cxx_initializer_list.cpp). Compiled with the model
// directory as -isystem so `#include <initializer_list>` is
// src/prism/pir/models/cxx/initializer_list. size() is the stored length.
// empty() is size() == 0. Neither call loads an element. A zero-length
// list has size 0. This trace is not PROVED and not CLEAN.
#include <cstdio>
#include <initializer_list>

struct E {
    int v;
    static int copies;
    static int reads;
    E(int x) : v(x) {}
    E(const E& o) : v(o.v) { ++copies; }
    int get() const {
        ++reads;
        return v;
    }
};

int E::copies = 0;
int E::reads = 0;

constexpr bool zero_length() {
    std::initializer_list<int> z;
    return z.size() == 0 && z.empty();
}

constexpr bool three_length() {
    std::initializer_list<int> xs{1, 2, 3};
    return xs.size() == 3 && !xs.empty();
}

static_assert(zero_length());
static_assert(three_length());

int main() {
    std::initializer_list<int> zero;
    std::initializer_list<int> braced{};
    std::initializer_list<int> three{4, 5, 6};
    std::printf("zero size=%zu empty=%d\n", zero.size(), int(zero.empty()));
    std::printf("braced size=%zu empty=%d\n", braced.size(), int(braced.empty()));
    std::printf("three size=%zu empty=%d\n", three.size(), int(three.empty()));

    E::copies = 0;
    E::reads = 0;
    std::initializer_list<E> none;
    std::printf("none size=%zu empty=%d copies=%d reads=%d\n", none.size(), int(none.empty()), E::copies, E::reads);

    std::initializer_list<E> some{E{1}, E{2}, E{3}};
    E::copies = 0;
    E::reads = 0;
    const auto n = some.size();
    const bool emp = some.empty();
    std::printf("after size=%zu empty=%d copies=%d reads=%d\n", n, int(emp), E::copies, E::reads);
    return (zero.size() == 0 && zero.empty() && braced.size() == 0 && braced.empty() && three.size() == 3 &&
            !three.empty() && none.size() == 0 && none.empty() && n == 3 && !emp && E::copies == 0 && E::reads == 0)
               ? 0
               : 1;
}
