// Trace of std::regex pattern ownership. `#include <regex>` is PRISM's
// model when its directory is first on the include path. libstdc++'s
// std::regex does not expose the pattern characters, so this trace is the
// model: size, empty, and data are the owned copy, not a match. flags()
// is the stored unsigned. It is not a match, and 0 is not a match.
//
// regex_match, regex_search, and regex_replace are not called.
//
// argv "null" constructs from a null const char*. The model checks
// FUNC-CONTRACT and must not print a pattern line.
// argv "null-flag" is the same null pointer with a nonzero flag. The
// flag is not stored and no pattern line is printed.
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
    std::printf("%s size=%zu empty=%d flags=%u", label, r.size(), r.empty() ? 1 : 0, r.flags());
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
    if (argc > 1 && std::strcmp(argv[1], "null-flag") == 0) {
        const char* p = nullptr;
        std::regex r(p, 9u);
        std::printf("stored flags=%u\n", r.flags());
        print_pat("null-flag", r);
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
    std::regex def;
    std::regex flagged(p, 4u);
    std::regex flag_empty(z, 7u);
    std::regex flag_empty_copy(flag_empty);
    std::regex flag_zero(p, 0u);
    std::regex star(".*", 3u);
    std::regex flag_copy(flagged);
    const int flag_distinct = flag_copy.data() != nullptr && flag_copy.data() != flagged.data();
    std::regex flag_moved(std::move(flagged));
    std::regex flag_assigned("z", 1u);
    flag_assigned = flag_copy;
    std::regex flag_sink("q", 2u);
    flag_sink = std::move(flag_assigned);

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
    print_pat("def", def);
    print_pat("flag_moved", flag_moved);
    print_pat("flag_src", flagged);
    print_pat("flag_empty", flag_empty);
    print_pat("flag_empty_copy", flag_empty_copy);
    print_pat("flag_zero", flag_zero);
    print_pat("star", star);
    print_pat("flag_copy", flag_copy);
    print_pat("flag_sink", flag_sink);
    print_pat("flag_assign_src", flag_assigned);
    std::printf("flag_distinct=%d\n", flag_distinct);

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
    if (empty.flags() != 0 || from_ptr.flags() != 0 || from_arr.flags() != 0 || ptr_empty.flags() != 0)
        return fail("zero_flags");
    if (moved.flags() != 0 || copy.flags() != 0 || sink.flags() != 0 || lit.flags() != 0) return fail("moved_flags");
    if (written.flags() != 0 || def.flags() != 0) return fail("written_flags");
    if (flag_moved.flags() != 4u || flag_moved.size() != 2 || flag_moved.data()[0] != 'a') return fail("flag_moved");
    if (!flagged.empty() || flagged.flags() != 0 || flagged.data() != nullptr) return fail("flag_src");
    if (!flag_empty.empty() || flag_empty.size() != 0 || flag_empty.data() != nullptr || flag_empty.flags() != 7u)
        return fail("flag_empty");
    if (!flag_empty_copy.empty() || flag_empty_copy.data() != nullptr || flag_empty_copy.flags() != 7u)
        return fail("flag_empty_copy");
    if (flag_zero.flags() != 0 || flag_zero.size() != 2 || flag_zero.data()[0] != 'a') return fail("flag_zero");
    if (star.flags() != 3u || star.size() != 2 || star.data()[0] != '.' || star.data()[1] != '*') return fail("star");
    if (!flag_distinct || flag_copy.flags() != 4u || flag_copy.size() != 2) return fail("flag_copy");
    if (flag_sink.flags() != 4u || flag_sink.size() != 2 || flag_sink.data()[0] != 'a' || flag_sink.data()[1] != 'b')
        return fail("flag_sink");
    if (!flag_assigned.empty() || flag_assigned.flags() != 0 || flag_assigned.data() != nullptr)
        return fail("flag_assign_src");
    return 0;
}
