// Trace of std::regex pattern ownership. `#include <regex>` is PRISM's
// model when its directory is first on the include path. libstdc++'s
// std::regex does not expose the pattern characters, so this trace is the
// model: size, empty, and data are the owned copy, not a match.
//
// regex_match, regex_search, and regex_replace are not called.
//
// argv "null" constructs from a null const char*. The model checks
// FUNC-CONTRACT and must not print a pattern line.
// argv "bare" is a const char* with no terminator inside the object. The
// model checks MEM-OOB-READ and must not print a pattern line.
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>

static const char* g_bound_base = nullptr;
static unsigned long g_bound = 1ul << 20;

// Host stand-ins for the PIR intrinsics. A failed check aborts before the
// caller can print a pattern. A passing check returns.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls ? cls : "", msg ? msg : "");
        std::fflush(stderr);
        std::abort();
    }
}

extern "C" unsigned long __prism_obj_size(const void* p) {
    if (g_bound_base != nullptr && p == g_bound_base) return g_bound;
    return 1ul << 20;
}

static void print_pat(const char* label, const std::regex& r) {
    std::printf("%s size=%zu empty=%d", label, r.size(), r.empty() ? 1 : 0);
    if (r.data() == nullptr) {
        std::printf(" data=null\n");
        return;
    }
    std::printf(" data=");
    for (std::size_t i = 0; i < r.size(); ++i) std::putchar(r.data()[i]);
    std::printf("\n");
}

static int fail(const char* what) {
    std::fprintf(stderr, "trace %s\n", what);
    return 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "null") == 0) {
        const char* p = nullptr;
        std::regex r(p);
        print_pat("null", r);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "bare") == 0) {
        char raw[4] = {'a', 'b', 'c', 'd'};
        g_bound_base = raw;
        g_bound = 4;
        const char* p = raw;
        std::regex r(p);
        print_pat("bare", r);
        return 0;
    }

    std::regex lit("ab");
    std::regex empty("");
    const char* p = "ab";
    const char* z = "";
    std::regex from_ptr(p);
    std::regex ptr_empty(z);
    char arr[3] = {'a', 'b', '\0'};
    std::regex from_arr(arr);
    std::regex copy(lit);
    const int distinct = copy.data() != nullptr && copy.data() != lit.data();
    std::regex moved(std::move(lit));
    std::regex assigned("z");
    assigned = copy;
    const int assign_distinct = assigned.data() != nullptr && assigned.data() != copy.data();
    std::regex sink("q");
    sink = std::move(assigned);
    std::regex written(".*");

    print_pat("empty", empty);
    print_pat("ptr", from_ptr);
    print_pat("ptr_empty", ptr_empty);
    print_pat("arr", from_arr);
    std::printf("distinct=%d\n", distinct);
    print_pat("src_after_move", lit);
    print_pat("moved", moved);
    print_pat("copy_after", copy);
    std::printf("assign_distinct=%d\n", assign_distinct);
    print_pat("sink", sink);
    print_pat("assign_src", assigned);
    print_pat("written", written);

    if (moved.size() != 2 || moved.data() == nullptr || moved.data()[0] != 'a' || moved.data()[1] != 'b')
        return fail("moved");
    if (!empty.empty() || empty.size() != 0 || empty.data() != nullptr) return fail("empty");
    if (from_ptr.size() != 2 || from_ptr.data()[0] != 'a' || from_ptr.data()[1] != 'b') return fail("ptr");
    if (from_arr.size() != 2 || from_arr.data()[0] != 'a' || from_arr.data()[1] != 'b') return fail("arr");
    if (!ptr_empty.empty() || ptr_empty.data() != nullptr) return fail("ptr_empty");
    if (!distinct) return fail("distinct");
    if (!lit.empty() || lit.data() != nullptr) return fail("src_after_move");
    if (copy.size() != 2 || copy.data()[0] != 'a' || copy.data()[1] != 'b') return fail("copy_after");
    if (!assign_distinct) return fail("assign_distinct");
    if (sink.size() != 2 || sink.data()[0] != 'a' || sink.data()[1] != 'b') return fail("sink");
    if (!assigned.empty() || assigned.data() != nullptr) return fail("assign_src");
    if (written.size() != 2 || written.empty() || written.data()[0] != '.' || written.data()[1] != '*')
        return fail("written");
    if (empty.size() == written.size()) return fail("empty_is_written");
    return 0;
}
