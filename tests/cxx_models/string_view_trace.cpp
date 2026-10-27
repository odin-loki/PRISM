// Differential trace of std::string_view (tests/cpp/test_cxx_string_view.cpp):
// compiled once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/string_view) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS. The traces must be identical.
// compare, find, substr, and starts_with are not called: the model does not
// declare them.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

// Host stand-ins for the PIR intrinsics the model calls. libstdc++ does not
// reference them. A failed check aborts, which is what empty front() must do.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "check %s: %s\n", cls ? cls : "", msg ? msg : "");
        std::abort();
    }
}

extern "C" unsigned long __prism_obj_size(const void*) {
    // The trace only passes pointers into short NUL-terminated arrays.
    return 1ul << 20;
}

static void owned() {
    char buf[] = {'p', 'q', 'r', 's'};
    const char* raw = buf;
    {
        std::string_view v(buf, 4);
        std::printf("data_eq=%d size=%zu empty=%d idx1=%c front=%c back=%c\n", int(v.data() == raw), v.size(),
                    int(v.empty()), v[1], v.front(), v.back());
        v.remove_prefix(1);
        v.remove_suffix(1);
        std::printf("trimmed size=%zu front=%c back=%c idx0=%c\n", v.size(), v.front(), v.back(), v[0]);
    }
    // The view is gone. The buffer is still the caller's characters.
    std::printf("alive=%c%c%c%c\n", buf[0], buf[1], buf[2], buf[3]);
}

static void from_cstr() {
    const char* s = "view";
    std::string_view v(s);
    std::printf("cstr size=%zu idx0=%c idx3=%c data_eq=%d\n", v.size(), v[0], v[3], int(v.data() == s));
}

static int empty_front() {
    std::string_view v;
    return v.front();
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "front") == 0) return empty_front();
    owned();
    from_cstr();
    return 0;
}
