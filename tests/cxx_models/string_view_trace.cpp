// Differential trace of std::string_view (tests/cpp/test_cxx_string_view.cpp):
// compiled once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/string_view) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS. The traces must be identical.
// compare(basic_string_view) is called. find(basic_string_view, pos) and
// rfind(basic_string_view, pos) are called. An empty needle does not read.
// starts_with, ends_with, and substr are. An empty needle matches.
// A longer needle is false, and the comparison does not read past the view.
// substr past size() is a failed check. A count of npos, or a count that would
// pass size(), stops at size(). substr(0, 0) does not read a character.
// Two empty views compare equal. The printed compare values are signs, so a
// libstdc++ magnitude other than -1 or 1 still matches. A high byte is ordered
// as unsigned char. A proper prefix is less. A zero-length view is not read.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
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

static void put_line(const char* name, std::initializer_list<std::size_t> xs) {
    std::fputs(name, stdout);
    std::fputc('=', stdout);
    bool first = true;
    for (const std::size_t p : xs) {
        if (!first) std::fputc(' ', stdout);
        first = false;
        if (p == std::string_view::npos) std::fputs("npos", stdout);
        else std::printf("%zu", p);
    }
    std::fputc('\n', stdout);
}

static void searches() {
    static constexpr char raw[] = {'a', 'b', 'a', 'b', 'a'};
    static constexpr std::string_view cv(raw, 5);
    static constexpr char an[] = {'a', 'b', 'a'};
    static constexpr std::string_view cn(an, 3);
    // One past the last character. A read of this empty needle is not a
    // constant expression.
    static constexpr std::string_view cempty(raw + 5, 0);
    static constexpr char hi[] = {static_cast<char>(0x80), 'a'};
    static constexpr std::string_view hv(hi, 2);
    static constexpr std::string_view hn(hi, 1);
    static constexpr char aaa[] = {'a', 'a', 'a'};
    static constexpr std::string_view ca(aaa, 3);
    static constexpr std::string_view caa(aaa, 2);
    static constexpr char ab[] = {'a', 'b'};
    static constexpr char abc[] = {'a', 'b', 'c'};
    static_assert(cv.find(cn, 0) == 0);
    static_assert(cv.find(cn, 1) == 2);
    static_assert(cv.find(cn, 2) == 2);
    static_assert(cv.find(cn, 3) == std::string_view::npos);
    static_assert(cv.find(cn, 5) == std::string_view::npos);
    static_assert(cv.find(cn, 6) == std::string_view::npos);
    static_assert(cv.find(cempty, 0) == 0);
    static_assert(cv.find(cempty, 5) == 5);
    static_assert(cv.find(cempty, 6) == std::string_view::npos);
    static_assert(cv.rfind(cn) == 2);
    static_assert(cv.rfind(cn, 2) == 2);
    static_assert(cv.rfind(cn, 1) == 0);
    static_assert(cv.rfind(cn, 0) == 0);
    static_assert(cv.rfind(cempty, 0) == 0);
    static_assert(cv.rfind(cempty, 3) == 3);
    static_assert(cv.rfind(cempty, 9) == 5);
    static_assert(cv.rfind(cempty) == 5);
    static_assert(cv.find(cv) == 0);
    static_assert(cv.rfind(cv) == 0);
    static_assert(std::string_view{}.find(cn, 0) == std::string_view::npos);
    static_assert(std::string_view{}.find(std::string_view{}, 0) == 0);
    static_assert(std::string_view{}.find(std::string_view{}, 1) == std::string_view::npos);
    static_assert(std::string_view{}.rfind(cn) == std::string_view::npos);
    static_assert(std::string_view{}.rfind(std::string_view{}, 4) == 0);
    static_assert(std::string_view{}.rfind(std::string_view{}, 0) == 0);
    static_assert(hv.find(hn, 0) == 0);
    static_assert(hv.find(hn, 1) == std::string_view::npos);
    static_assert(hv.rfind(hn) == 0);
    static_assert(ca.find(caa, 0) == 0);
    static_assert(ca.rfind(caa) == 1);
    static_assert(std::string_view(ab, 2).find(std::string_view(abc, 3), 0) == std::string_view::npos);
    static_assert(std::string_view(ab, 2).rfind(std::string_view(abc, 3)) == std::string_view::npos);

    // Exact objects. A read past size() is an ASan fault, including a read
    // of an empty needle whose pointer is one past the end, and a read of
    // the null pointer in a default view.
    char buf[] = {'a', 'b', 'a', 'b', 'a'};
    std::string_view v(buf, 5);
    char aba[] = {'a', 'b', 'a'};
    std::string_view naba(aba, 3);
    char abn[] = {'a', 'b'};
    std::string_view nab(abn, 2);
    char ba[] = {'b', 'a'};
    std::string_view nba(ba, 2);
    char zz[] = {'z'};
    std::string_view nz(zz, 1);
    char aaab[] = {'a', 'a', 'a'};
    std::string_view va(aaab, 3);
    char aan[] = {'a', 'a'};
    std::string_view naa(aan, 2);
    char aab[] = {'a', 'a', 'b'};
    std::string_view vaab(aab, 3);
    char ab2[] = {'a', 'b'};
    std::string_view nab2(ab2, 2);
    char cdh[] = {'a', 'b', 'c', 'd'};
    std::string_view vcd(cdh, 4);
    char cdn[] = {'c', 'd'};
    std::string_view ncd(cdn, 2);
    char abc3[] = {'a', 'b', 'c'};
    std::string_view vabc(abc3, 3);
    char abd[] = {'a', 'b', 'd'};
    std::string_view nabd(abd, 3);
    char hib[] = {'a', static_cast<char>(0x80), 'b'};
    std::string_view vhi(hib, 3);
    char oneb[] = {static_cast<char>(0x80)};
    std::string_view nhi(oneb, 1);
    char one[] = {'q'};
    std::string_view vo(one, 1);
    char exb[] = {'a', 'b'};
    std::string_view ex(exb, 2);
    char exlong[] = {'a', 'b', 'c'};
    std::string_view exn(exlong, 3);
    const auto gap = v.substr(5, 0);
    const auto end = ex.substr(2, 0);
    std::string_view empty;

    put_line("find_hit", {v.find(naba, 0), v.find(naba, 1), v.find(naba, 2), v.find(naba, 3)});
    put_line("rfind_hit", {v.rfind(naba), v.rfind(naba, 2), v.rfind(naba, 1), v.rfind(naba, 0)});
    put_line("find_ab", {v.find(nab, 0), v.find(nab, 1), v.find(nab, 4)});
    put_line("rfind_ab", {v.rfind(nab), v.rfind(nab, 1)});
    put_line("find_ba", {v.find(nba, 0), v.rfind(nba)});
    put_line("find_aaa", {va.find(naa, 0), va.rfind(naa)});
    put_line("find_pref", {vaab.find(nab2, 0), vaab.rfind(nab2)});
    put_line("find_tail", {vcd.find(ncd, 0), vcd.find(ncd, 3), vcd.rfind(ncd), vcd.rfind(ncd, 1)});
    put_line("find_miss", {v.find(nz, 0), vabc.find(nabd, 0)});
    put_line("find_hi", {vhi.find(nhi, 0), vhi.find(nhi, 2), vhi.rfind(nhi)});
    put_line("find_empty", {v.find(gap, 0), v.find(gap, 5), v.find(gap, 6), v.rfind(gap, 0), v.rfind(gap, 3),
                            v.rfind(gap, 9), v.rfind(gap)});
    put_line("find_past", {v.find(naba, 5), v.find(naba, 6), vo.find(vo, 1), vo.find(vo, 2)});
    put_line("find_null", {empty.find(naba, 0), empty.find(gap, 0), empty.find(gap, 1), empty.rfind(naba),
                           empty.rfind(gap, 4), empty.rfind(gap, 0)});
    put_line("find_long", {ex.find(exn, 0), ex.rfind(exn)});
    put_line("find_endgap", {ex.find(end, 0), ex.find(end, 2), ex.find(end, 3), ex.rfind(end, 1), ex.rfind(end, 7)});
    put_line("find_self", {v.find(v), v.rfind(v)});
    std::printf("find_alive=%c%c%c%c%c\n", buf[0], buf[1], buf[2], buf[3], buf[4]);
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "front") == 0) return empty_front();
    if (argc > 1 && std::strcmp(argv[1], "substr") == 0) return substr_oob();
    owned();
    from_cstr();
    prefixes();
    slices();
    orders();
    searches();
    return 0;
}
