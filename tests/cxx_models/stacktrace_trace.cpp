// Trace of std::stacktrace. `#include <stacktrace>` is the model when its
// directory is first on the include path. A default trace has size 0 and
// empty() is true. current() has size 0 or 1, and empty() is that size == 0.
// empty() does not read a frame. Two empty traces compare equal. Different
// sizes compare unequal. The same size compares the stored addresses.
// operator!= is the negation. argv "oob" indexes an empty trace and must
// not print a frame. This is not PROVED and not CLEAN.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stacktrace>

// The model declares this PIR intrinsic and does not define it.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls ? cls : "", msg ? msg : "");
        std::fflush(stderr);
        std::abort();
    }
}

static_assert(std::stacktrace{} == std::stacktrace{});
static_assert(!(std::stacktrace{} != std::stacktrace{}));

// Separate calls so the stored addresses are the return sites of current().
[[gnu::noinline]] std::stacktrace capture_left() { return std::stacktrace::current(); }

[[gnu::noinline]] std::stacktrace capture_right() {
    std::stacktrace st = std::stacktrace::current();
    return st;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "oob") == 0) {
        std::stacktrace empty;
        if (!empty.empty()) {
            std::printf("frame skip\n");
            return 0;
        }
        const void* ip = empty[0].native_handle();
        std::printf("frame %p\n", ip);
        return 0;
    }

    std::stacktrace empty;
    std::stacktrace other;
    const int eeq = (empty == other) ? 1 : 0;
    const int ene = (empty != other) ? 1 : 0;
    std::printf("empty %zu\n", empty.size());
    std::printf("eflag %d\n", empty.empty() ? 1 : 0);
    std::printf("eeq %d\n", eeq);
    std::printf("ene %d\n", ene);

    std::stacktrace cur = std::stacktrace::current();
    std::stacktrace copy = cur;
    const int deq = (empty == cur) ? 1 : 0;
    const int dne = (empty != cur) ? 1 : 0;
    const int seq = (cur == copy) ? 1 : 0;
    const int sne = (cur != copy) ? 1 : 0;
    std::printf("current %zu\n", cur.size());
    std::printf("cflag %d\n", cur.empty() ? 1 : 0);
    std::printf("deq %d\n", deq);
    std::printf("dne %d\n", dne);
    std::printf("seq %d\n", seq);
    std::printf("sne %d\n", sne);
    if (cur.size() == 1) {
        const void* ip = cur[0].native_handle();
        std::printf("frame %d\n", ip != nullptr ? 1 : 0);
    }
    std::stacktrace left = capture_left();
    std::stacktrace right = capture_right();
    const int aeq = (left == right) ? 1 : 0;
    const int ane = (left != right) ? 1 : 0;
    int hsame = 0;
    if (left.size() == 0 && right.size() == 0) {
        hsame = 1;
    } else if (left.size() == 1 && right.size() == 1) {
        hsame = left[0].native_handle() == right[0].native_handle() ? 1 : 0;
    }
    std::printf("aeq %d\n", aeq);
    std::printf("ane %d\n", ane);
    std::printf("hsame %d\n", hsame);
    if (empty.size() != 0) return 1;
    if (!empty.empty()) return 4;
    if (cur.empty() != (cur.size() == 0)) return 5;
    if (cur.size() > 1) return 2;
    if (cur.size() == 1 && cur[0].native_handle() == nullptr) return 3;
    if (eeq != 1 || ene != 0) return 6;
    if (empty.size() == cur.size()) {
        if (deq != 1 || dne != 0) return 7;
    } else {
        if (deq != 0 || dne != 1) return 7;
    }
    if (seq != 1 || sne != 0) return 8;
    if (left.size() != right.size() || left.size() > 1) return 9;
    if (hsame) {
        if (aeq != 1 || ane != 0) return 10;
    } else {
        if (aeq != 0 || ane != 1) return 10;
    }
    return 0;
}
