// Trace of std::valarray. `#include <valarray>` is the model when its
// directory is first on the include path, and libstdc++ otherwise.
// A default valarray has size 0. One stored value is read back at [0].
// resize to a new length replaces the previous buffer. An empty [0] is not
// traced: libstdc++ would use a null pointer, and the model reports a check.
#include <cstdio>
#include <cstdlib>
#include <valarray>

// The model declares this PIR intrinsic and does not define it. libstdc++
// <valarray> never calls it.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls ? cls : "", msg ? msg : "");
        std::fflush(stderr);
        std::abort();
    }
}

int main() {
    std::valarray<int> empty;
    std::printf("empty %zu\n", empty.size());

    std::valarray<int> one;
    one.resize(1, 7);
    std::printf("one %zu %d\n", one.size(), one[0]);

    one.resize(3, 4);
    std::printf("grow %zu %d\n", one.size(), one[0]);

    one.resize(1, 9);
    std::printf("shrink %zu %d\n", one.size(), one[0]);

    one.resize(0);
    std::printf("zero %zu\n", one.size());
    return empty.size() == 0 && one.size() == 0 ? 0 : 1;
}
