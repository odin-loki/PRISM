// Trace of std::stacktrace. `#include <stacktrace>` is the model when its
// directory is first on the include path. A default trace has size 0.
// current() has size 0 or 1. argv "oob" indexes an empty trace and must not
// print a frame.
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

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "oob") == 0) {
        std::stacktrace empty;
        const void* ip = empty[0].native_handle();
        std::printf("frame %p\n", ip);
        return 0;
    }

    std::stacktrace empty;
    std::printf("empty %zu\n", empty.size());

    std::stacktrace cur = std::stacktrace::current();
    std::printf("current %zu\n", cur.size());
    if (cur.size() == 1) {
        const void* ip = cur[0].native_handle();
        std::printf("frame %d\n", ip != nullptr ? 1 : 0);
    }
    if (empty.size() != 0) return 1;
    if (cur.size() > 1) return 2;
    if (cur.size() == 1 && cur[0].native_handle() == nullptr) return 3;
    return 0;
}
