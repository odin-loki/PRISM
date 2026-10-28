// Differential trace of std::string_view (tests/cpp/test_cxx_string_view.cpp):
// compiled once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/string_view) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS. The traces must be identical.
// compare, find, and substr are not called: the model does not declare them.
// starts_with and ends_with are. An empty needle matches. A longer needle is
// false, and the comparison does not read past the view.
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

static void prefixes() {
    char buf[] = {'p', 'q', 'r', 's'};
    std::string_view v(buf, 4);
    std::string_view empty;
    std::string_view pq(buf, 2);
    char pxb[] = {'p', 'x'};
    std::string_view px(pxb, 2);
    char longer[] = {'p', 'q', 'r', 's', 't'};
    std::string_view longn(longer, 5);
    char rsb[] = {'r', 's'};
    std::string_view rs(rsb, 2);
    char qrb[] = {'q', 'r'};
    std::string_view qr(qrb, 2);
    // exact is the whole object, so a read past the view is an ASan fault.
    char exact[] = {'a', 'b'};
    std::string_view ex(exact, 2);
    char exlong[] = {'a', 'b', 'c'};
    std::string_view exn(exlong, 3);
    char xab[] = {'x', 'a', 'b'};
    std::string_view xabs(xab, 3);

    std::printf("sw_empty=%d sw_both=%d sw_pq=%d sw_px=%d sw_long=%d sw_none=%d sw_self=%d\n",
                int(v.starts_with(empty)), int(empty.starts_with(empty)), int(v.starts_with(pq)),
                int(v.starts_with(px)), int(v.starts_with(longn)), int(empty.starts_with(pq)), int(v.starts_with(v)));
    std::printf("ew_empty=%d ew_both=%d ew_rs=%d ew_qr=%d ew_long=%d ew_none=%d ew_self=%d\n",
                int(v.ends_with(empty)), int(empty.ends_with(empty)), int(v.ends_with(rs)), int(v.ends_with(qr)),
                int(v.ends_with(longn)), int(empty.ends_with(rs)), int(v.ends_with(v)));
    std::printf("ch_sw=%d %d ch_ew=%d %d ch_empty=%d %d\n", int(v.starts_with('p')), int(v.starts_with('q')),
                int(v.ends_with('s')), int(v.ends_with('r')), int(empty.starts_with('a')), int(empty.ends_with('a')));
    std::printf("exact_long=%d %d suf_long=%d\n", int(ex.starts_with(exn)), int(ex.ends_with(exn)),
                int(ex.ends_with(xabs)));
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "front") == 0) return empty_front();
    owned();
    from_cstr();
    prefixes();
    return 0;
}
