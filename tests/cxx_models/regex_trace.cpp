// Trace of std::regex pattern ownership. `#include <regex>` is PRISM's
// model when its directory is first on the include path. libstdc++'s
// std::regex does not expose the pattern characters, so this trace is the
// model: size, empty, and data are the owned copy, not a match. empty()
// is size() == 0. It does not decide a match. mark_count() is 0. It does
// not scan the pattern for a match, and 0 is not a match. flags()
// is the stored unsigned. It is not a match, and 0 is not a match.
// basic_regex is this regex. basic_regex::swap exchanges the pattern
// pointer, the length, and the flags. It does not read a character. Two
// empty patterns stay empty. Self-swap keeps the same block.
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
#include <type_traits>
#include <utility>

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
    std::printf("%s size=%zu empty=%d flags=%u marks=%u", label, r.size(), r.empty() ? 1 : 0, r.flags(),
                r.mark_count());
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

static int query_ok(const char* what, const std::regex& r) {
    if (r.mark_count() != 0) return fail(what);
    if (r.empty() != (r.size() == 0)) return fail(what);
    return 0;
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
    std::regex grouped("(a)");

    static_assert(std::is_same_v<std::regex, std::basic_regex>);
    static_assert(noexcept(std::declval<std::basic_regex&>().swap(std::declval<std::basic_regex&>())));
    static_assert(std::is_void_v<decltype(std::declval<std::basic_regex&>().swap(std::declval<std::basic_regex&>()))>);
    static_assert(noexcept(std::swap(std::declval<std::basic_regex&>(), std::declval<std::basic_regex&>())));

    std::basic_regex swap_a(p, 4u);
    std::regex swap_b(".*", 3u);
    const char* swap_pa = swap_a.data();
    const char* swap_pb = swap_b.data();
    swap_a.swap(swap_b);
    const int swap_exchanged = swap_a.data() == swap_pb && swap_b.data() == swap_pa;
    std::regex swap_e1;
    std::regex swap_e2(z, 7u);
    swap_e1.swap(swap_e2);
    std::regex both_a;
    std::regex both_b;
    both_a.swap(both_b);
    std::regex swap_self(p, 4u);
    const char* self_p = swap_self.data();
    swap_self.swap(swap_self);
    const int self_same = swap_self.data() == self_p;
    std::regex swap_full("ab", 4u);
    std::regex swap_hole;
    const char* full_p = swap_full.data();
    swap_full.swap(swap_hole);
    std::basic_regex std_swap_a("ab", 1u);
    std::regex std_swap_b("cd", 2u);
    const char* std_pa = std_swap_a.data();
    const char* std_pb = std_swap_b.data();
    std::swap(std_swap_a, std_swap_b);
    const int std_exchanged = std_swap_a.data() == std_pb && std_swap_b.data() == std_pa;
    std::regex std_self("zw", 5u);
    const char* std_self_p = std_self.data();
    std::swap(std_self, std_self);
    const int std_self_same = std_self.data() == std_self_p;

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
    print_pat("grouped", grouped);
    std::printf("flag_distinct=%d\n", flag_distinct);
    print_pat("swap_a", swap_a);
    print_pat("swap_b", swap_b);
    std::printf("swap_exchanged=%d\n", swap_exchanged);
    print_pat("swap_empty_a", swap_e1);
    print_pat("swap_empty_b", swap_e2);
    print_pat("swap_both_empty", both_a);
    print_pat("swap_both_empty_b", both_b);
    print_pat("swap_self", swap_self);
    std::printf("self_same=%d\n", self_same);
    print_pat("swap_now_empty", swap_full);
    print_pat("swap_now_full", swap_hole);
    print_pat("std_swap_a", std_swap_a);
    print_pat("std_swap_b", std_swap_b);
    std::printf("std_exchanged=%d\n", std_exchanged);
    print_pat("std_self", std_self);
    std::printf("std_self_same=%d\n", std_self_same);

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
    if (grouped.mark_count() != 0 || grouped.size() != 3 || grouped.empty() || grouped.flags() != 0)
        return fail("grouped");
    if (grouped.data() == nullptr || grouped.data()[0] != '(' || grouped.data()[1] != 'a' || grouped.data()[2] != ')')
        return fail("grouped_data");
    if (query_ok("q_empty", empty) || query_ok("q_ptr", from_ptr) || query_ok("q_written", written) ||
        query_ok("q_star", star) || query_ok("q_grouped", grouped) || query_ok("q_def", def) ||
        query_ok("q_flag_empty", flag_empty) || query_ok("q_moved", moved) || query_ok("q_src", lit))
        return 1;
    if (!swap_exchanged || swap_a.flags() != 3u || swap_b.flags() != 4u || swap_a.size() != 2 || swap_b.size() != 2)
        return fail("swap_exchanged");
    if (swap_a.data()[0] != '.' || swap_a.data()[1] != '*' || swap_b.data()[0] != 'a' || swap_b.data()[1] != 'b')
        return fail("swap_chars");
    if (!swap_e1.empty() || swap_e1.size() != 0 || swap_e1.data() != nullptr || swap_e1.flags() != 7u)
        return fail("swap_empty_a");
    if (!swap_e2.empty() || swap_e2.size() != 0 || swap_e2.data() != nullptr || swap_e2.flags() != 0)
        return fail("swap_empty_b");
    if (!both_a.empty() || both_a.data() != nullptr || both_a.flags() != 0 || !both_b.empty() ||
        both_b.data() != nullptr || both_b.flags() != 0)
        return fail("swap_both_empty");
    if (!self_same || swap_self.flags() != 4u || swap_self.size() != 2 || swap_self.data()[0] != 'a' ||
        swap_self.data()[1] != 'b')
        return fail("swap_self");
    if (!swap_full.empty() || swap_full.data() != nullptr || swap_full.flags() != 0) return fail("swap_now_empty");
    if (swap_hole.data() != full_p || swap_hole.flags() != 4u || swap_hole.size() != 2 || swap_hole.data()[0] != 'a' ||
        swap_hole.data()[1] != 'b')
        return fail("swap_now_full");
    if (!std_exchanged || std_swap_a.flags() != 2u || std_swap_b.flags() != 1u || std_swap_a.data()[0] != 'c' ||
        std_swap_b.data()[0] != 'a')
        return fail("std_swap");
    if (!std_self_same || std_self.flags() != 5u || std_self.size() != 2 || std_self.data()[0] != 'z' ||
        std_self.data()[1] != 'w')
        return fail("std_self");
    if (query_ok("q_swap_a", swap_a) || query_ok("q_swap_empty", swap_e1) || query_ok("q_both", both_a) ||
        query_ok("q_self", swap_self) || query_ok("q_hole", swap_hole) || query_ok("q_std_self", std_self))
        return 1;
    return 0;
}
