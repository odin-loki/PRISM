// Trace of std::coroutine_handle<void>. `#include <coroutine>` is the model
// when its directory is first on the include path, and libstdc++ otherwise.
// A default handle stores nullptr. address() of that handle is nullptr.
// from_address(nullptr) is a null handle. Equality is address() == address().
// Two default handles compare equal. A default handle is not equal to
// from_address of a non-null pointer. That comparison is not a proof that a
// coroutine ran. The happy path does not call resume, destroy, or done: a
// null resume is a check, not a finished run.
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The model declares this PIR intrinsic and does not define it. libstdc++
// <coroutine> never calls it. A failing check aborts before a caller can
// print a resumed or finished coroutine.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls ? cls : "", msg ? msg : "");
        std::fflush(stderr);
        std::abort();
    }
}

int main(int argc, char** argv) {
    std::coroutine_handle<> h;
    const void* addr = h.address();
    const std::coroutine_handle<> from_null = std::coroutine_handle<>::from_address(nullptr);
    const int live = static_cast<bool>(h) ? 1 : 0;
    std::printf("null %d from %d live %d\n", addr == nullptr ? 1 : 0, from_null.address() == nullptr ? 1 : 0, live);

    int object = 0;
    const std::coroutine_handle<> stored = std::coroutine_handle<>::from_address(&object);
    const std::coroutine_handle<> stored_again = std::coroutine_handle<>::from_address(&object);
    const int eq = (h == from_null && !(h != from_null)) ? 1 : 0;
    const int diff = (h != stored && !(h == stored)) ? 1 : 0;
    const int same =
        (stored.address() == &object && stored == stored_again && !(stored != stored_again)) ? 1 : 0;
    std::printf("eq %d diff %d same %d\n", eq, diff, same);

    if (argc > 1 && std::strcmp(argv[1], "resume") == 0) {
        h.resume();
        std::printf("resumed\n");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "destroy") == 0) {
        h.destroy();
        std::printf("destroyed\n");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "done") == 0) {
        const bool finished = h.done();
        std::printf("done=%d\n", finished ? 1 : 0);
        return finished ? 1 : 0;
    }
    return addr == nullptr && from_null.address() == nullptr && live == 0 && eq == 1 && diff == 1 && same == 1
               ? 0
               : 1;
}
