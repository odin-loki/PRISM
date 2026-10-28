// Differential trace of std::string_view (tests/cpp/test_cxx_string_view.cpp):
// compiled once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/string_view) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS. The traces must be identical.
// compare(basic_string_view) is called. find is not: the model does not
// declare it. starts_with, ends_with, and substr are. An empty needle matches.
// A longer needle is false, and the comparison does not read past the view.
// substr past size() is a failed check. A count of npos, or a count that would
// pass size(), stops at size(). substr(0, 0) does not read a character.
// Two empty views compare equal. The printed compare values are signs, so a
// libstdc++ magnitude other than -1 or 1 still matches. A high byte is ordered
// as unsigned char. A proper prefix is less. A zero-length view is not read.
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

static void slices() {
    char buf[] = {'p', 'q', 'r', 's'};
    std::string_view v(buf, 4);
    std::string_view empty;
    const auto z = v.substr(0, 0);
    const auto mid = v.substr(1, 2);
    const auto tail = v.substr(1, std::string_view::npos);
    const auto one = v.substr(1);
    const auto over = v.substr(2, static_cast<std::size_t>(-2));
    const auto at_end = v.substr(4, 1);
    const auto gap = v.substr(2, 0);
    const auto whole = v.substr(0, std::string_view::npos);
    const auto ez = empty.substr(0, 0);
    const auto en = empty.substr(0, std::string_view::npos);
    // Zero-length results are not indexed. That includes substr(0, 0).
    std::printf("sub0=%zu %d %d\n", z.size(), int(z.empty()), int(z.data() == v.data()));
    std::printf("submid=%zu %c%c %d\n", mid.size(), mid[0], mid[1], int(mid.data() == buf + 1));
    std::printf("subnpos=%zu %zu %d\n", tail.size(), one.size(), int(tail.data() == buf + 1));
    std::printf("subover=%zu %c%c %d\n", over.size(), over[0], over[1], int(over.data() == buf + 2));
    std::printf("subend=%zu %d %d\n", at_end.size(), int(at_end.empty()), int(at_end.data() == buf + 4));
    std::printf("subz=%zu %d %d\n", gap.size(), int(gap.empty()), int(gap.data() == buf + 2));
    std::printf("suball=%zu %d %d\n", whole.size(), int(whole.data() == v.data()), int(whole.data() == buf));
    std::printf("subempty=%zu %d %d %zu %d\n", ez.size(), int(ez.empty()), int(ez.data() == nullptr), en.size(),
                int(en.data() == nullptr));
    std::printf("subalive=%c%c%c%c\n", buf[0], buf[1], buf[2], buf[3]);
}

static int substr_oob() {
    char buf[] = {'p', 'q', 'r', 's'};
    std::string_view v(buf, 4);
    const auto s = v.substr(5, 1);
    return static_cast<int>(s.size());
}

static int sgn(int v) { return (v > 0) - (v < 0); }

static void orders() {
    std::string_view empty;
    std::string_view empty2;
    // Exact objects. A read past size() is an ASan fault.
    char a[] = {'a'};
    char b[] = {'b'};
    char ab[] = {'a', 'b'};
    char ac[] = {'a', 'c'};
    char abc[] = {'a', 'b', 'c'};
    char hi[] = {static_cast<char>(0x80)};
    std::string_view va(a, 1);
    std::string_view vb(b, 1);
    std::string_view vab(ab, 2);
    std::string_view vac(ac, 2);
    std::string_view vabc(abc, 3);
    std::string_view vhi(hi, 1);
    // Length 0, different characters behind the pointers. A read would order
    // 'a' against 'b' instead of returning 0.
    const auto z = va.substr(0, 0);
    const auto z2 = vb.substr(0, 0);
    // Length 0, one past the last character. A read is past the object.
    const auto end = vab.substr(2, 0);
    const auto end2 = vab.substr(2, 0);

    std::printf("cmp_empty=%d cmp_empty_a=%d cmp_a_empty=%d cmp_aa=%d cmp_ab=%d cmp_ba=%d\n",
                sgn(empty.compare(empty2)), sgn(empty.compare(va)), sgn(va.compare(empty)), sgn(va.compare(va)),
                sgn(va.compare(vb)), sgn(vb.compare(va)));
    std::printf("cmp_prefix=%d cmp_longer=%d cmp_mid=%d cmp_pref3=%d cmp_high=%d cmp_zz=%d cmp_end=%d\n",
                sgn(va.compare(vab)), sgn(vab.compare(va)), sgn(vab.compare(vac)), sgn(va.compare(vabc)),
                sgn(vhi.compare(va)), sgn(z.compare(z2)), sgn(end.compare(end2)));
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "front") == 0) return empty_front();
    if (argc > 1 && std::strcmp(argv[1], "substr") == 0) return substr_oob();
    owned();
    from_cstr();
    prefixes();
    slices();
    orders();
    return 0;
}
