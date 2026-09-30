// Doctests for the C++ lint branches and guards in src/prism/checkers_cxx.cpp
// (and the C-only gate in checkers_core.cpp). Each case is a small source
// with a bad twin that must fire and, where the branch has a guard, a good
// twin that must stay silent.

#include <doctest/doctest.h>

#include "prism/cparse.hpp"
#include "prism/stages.hpp"

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;

struct Snippet {
    fs::path dir;
    fs::path file;
    Snippet(std::string_view name, std::string_view src) {
        static std::atomic<int> seq{0};
        dir = fs::temp_directory_path()
            / ("prism_ckcxx_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
        fs::create_directories(dir);
        file = dir / std::string(name);
        std::ofstream(file) << src;
    }
    ~Snippet() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::vector<prism::Finding> of(std::string_view cls) const {
        std::vector<prism::Finding> out;
        for (auto& f : prism::run_lints({file}, dir, 1))
            if (f.cls == cls) out.push_back(f);
        return out;
    }
};

struct Case {
    const char* what;
    const char* file;  // name decides C vs C++ dispatch
    const char* cls;
    const char* src;
    const char* message;   // nullptr: must stay silent; else a substring of the message
    const char* function;  // expected function (when it fires)
};

void run_cases(const std::vector<Case>& cases) {
    for (const auto& c : cases) {
        const std::string label = std::string(c.what) + " [" + c.cls + "]";
        INFO(label);
        Snippet s(c.file, c.src);
        auto hits = s.of(c.cls);
        if (!c.message) {
            std::string got;
            for (auto& h : hits) got += h.message + "; ";
            INFO("unexpected: " << got);
            CHECK(hits.empty());
            continue;
        }
        REQUIRE_FALSE(hits.empty());
        INFO("message: " << hits[0].message);
        CHECK(hits[0].message.find(c.message) != std::string::npos);
        if (c.function) CHECK(hits[0].function.value_or("") == c.function);
    }
}
}  // namespace

TEST_CASE("checkers: STR-STRNCPY-NUL and STR-SNPRINTF run on C sources only") {
    const char* ncpy = "void f(char *d, const char *s) {\n    strncpy(d, s, sizeof d);\n    puts(d);\n}\n";
    const char* snp =
        "int f(char *d, int n, const char *s) {\n    int k = snprintf(d, n, \"%s\", s);\n    return d[k];\n}\n";
    for (const char* ext : {".cpp", ".cc", ".cxx", ".ii"}) {
        INFO(ext);
        CHECK(Snippet(std::string("a") + ext, ncpy).of("STR-STRNCPY-NUL").empty());
        CHECK(Snippet(std::string("a") + ext, snp).of("STR-SNPRINTF").empty());
    }
    // The same code in a .c file keeps whatever the C check says; the C++
    // gate must not change the C result.
    auto c_ncpy = Snippet("a.c", ncpy).of("STR-STRNCPY-NUL");
    auto c_snp = Snippet("a.c", snp).of("STR-SNPRINTF");
    CHECK(c_ncpy.size() == Snippet("b.c", ncpy).of("STR-STRNCPY-NUL").size());
    CHECK(c_snp.size() == Snippet("b.c", snp).of("STR-SNPRINTF").size());
}

TEST_CASE("checkers: MEM-COPY-LEN names a variable length, not a constant") {
    // A literal length is fixed at compile time: it cannot be the
    // attacker-sized length this rule is about (pcre2: memcpy(code, classbits, 32)).
    const char* lit = "struct s { int len_max; };\nvoid f(char *d, char *s) {\n    memcpy(d, s, 32);\n}\n";
    const char* var = "struct s { int len_max; };\nvoid f(char *d, char *s, int n) {\n    memcpy(d, s, n);\n}\n";
    CHECK(Snippet("a.c", lit).of("MEM-COPY-LEN").empty());
    auto hits = Snippet("a.c", var).of("MEM-COPY-LEN");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].message == "memcpy() length n not checked against a *_max bound in this function");
}

TEST_CASE("cparse: qualified parameter types keep their spelling") {
    Snippet s("a.cpp",
              "int f(const std::vector<int>& v, const char *key, volatile unsigned  int n, char* p) {\n"
              "    return v[n];\n}\n");
    auto fns = prism::extract_functions(s.file, "a.cpp");
    REQUIRE(fns.size() == 1);
    const auto& ps = fns[0].params;
    REQUIRE(ps.size() == 4);
    CHECK(ps[0] == std::pair<std::string, std::string>{"std::vector<int>&", "v"});
    CHECK(ps[1] == std::pair<std::string, std::string>{"char *", "key"});
    CHECK(ps[2] == std::pair<std::string, std::string>{"unsigned int", "n"});
    CHECK(ps[3] == std::pair<std::string, std::string>{"char*", "p"});
    // So CXX-VECTOR-INDEX sees a const vector reference parameter.
    auto hits = s.of("CXX-VECTOR-INDEX");
    REQUIRE_FALSE(hits.empty());
    CHECK(hits[0].function.value_or("") == "f");
}

TEST_CASE("checkers: index-token rules name the token in the subscript message") {
    run_cases({
        {"bit_ceil(0)", "a.cpp", "CXX-BIT-CEIL",
         "int f() {\n    unsigned z = std::bit_ceil(0);\n    return (int)z;\n}\n",
         "bit_ceil/bit_floor/popcount used as an index without a range check, or bit_ceil(0)", "f"},
        {"bit_ceil(n) is not bit_ceil(0)", "a.cpp", "CXX-BIT-CEIL",
         "int f(unsigned n) {\n    unsigned z = std::bit_ceil(n);\n    return (int)z;\n}\n", nullptr, nullptr},
        {"popcount index", "a.cpp", "CXX-BIT-CEIL",
         "int f(int* t, unsigned x) {\n    int k = std::popcount(x);\n    return t[k];\n}\n",
         "t[k] indexes with bit_ceil/popcount without a range check", "f"},
        {"popcount index guarded", "a.cpp", "CXX-BIT-CEIL",
         "int f(int* t, unsigned x) {\n    int k = std::popcount(x);\n    if (k < 8) return t[k];\n    return 0;\n}\n",
         nullptr, nullptr},
        {"rotl index", "a.cpp", "CXX-ROTL",
         "int f(int* t, unsigned x) {\n    int k = std::rotl(x, 3);\n    return t[k];\n}\n",
         "t[k] indexes with std::rotl/rotr without a range check", "f"},
        {"endian index", "a.cpp", "CXX-ENDIAN",
         "int f(int* t) {\n    int k = (int)std::endian::native;\n    return t[k];\n}\n",
         "t[k] indexes with std::endian without a range check", "f"},
        {"endian compared with little/big", "a.cpp", "CXX-ENDIAN",
         "int f(int* t) {\n    if (std::endian::native == std::endian::little) return 0;\n"
         "    int k = (int)std::endian::native;\n    return t[k];\n}\n",
         nullptr, nullptr},
        {"cmp_less inline index", "a.cpp", "CXX-CMP-LESS",
         "int f(int* t, int a, unsigned b) {\n    return t[std::cmp_less(a, b)];\n}\n",
         "std::cmp_less/in_range result used as an array index", "f"},
        {"in_range ignored", "a.cpp", "CXX-CMP-LESS",
         "int f(int* t, long k) {\n    bool ok = std::in_range<int>(k);\n    return t[k];\n}\n",
         "std::in_range ignored before a subscript", "f"},
        {"in_range tested", "a.cpp", "CXX-CMP-LESS",
         "int f(int* t, long k) {\n    if (!std::in_range<int>(k)) return 0;\n    return t[k];\n}\n", nullptr,
         nullptr},
        {"forward_like source reused", "a.cpp", "CXX-FORWARD-LIKE",
         "int f(int a) {\n    auto&& r = std::forward_like<int>(a);\n    return a + 1;\n}\n",
         "a used after std::forward_like", "f"},
        {"forward_like source not reused", "a.cpp", "CXX-FORWARD-LIKE",
         "int f(int a) {\n    auto&& r = std::forward_like<int>(a);\n    return r;\n}\n", nullptr, nullptr},
        {"kill_dependency as the only sync", "a.cpp", "CXX-KILL-DEPENDENCY",
         "int g_flag;\nint f(int x) {\n    g_flag = 1;\n    int y = std::kill_dependency(x);\n    return y;\n}\n",
         "std::kill_dependency used as only sync or as an index without a range check", "f"},
        {"kill_dependency with an atomic", "a.cpp", "CXX-KILL-DEPENDENCY",
         "int g_flag;\nint f(int x) {\n    g_flag = 1;\n    std::atomic_thread_fence(std::memory_order_acquire);\n"
         "    int y = std::kill_dependency(x);\n    return y;\n}\n",
         nullptr, nullptr},
        {"reduce", "a.cpp", "CXX-REDUCE",
         "int f(int* t, std::vector<int>& v) {\n    int k = std::reduce(v.begin(), v.end());\n    return t[k];\n}\n",
         "t[k] indexes with CXX-REDUCE without a range check", "f"},
        {"reduce next to transform_reduce is left to CXX-TRANSFORM-REDUCE", "a.cpp", "CXX-REDUCE",
         "int f(int* t, std::vector<int>& v) {\n    int k = std::reduce(v.begin(), v.end());\n"
         "    int j = std::transform_reduce(v.begin(), v.end(), 0, g, h);\n    return t[k] + j;\n}\n",
         nullptr, nullptr},
    });
}

TEST_CASE("checkers: addressof, as_const and basic_const_iterator branches") {
    run_cases({
        {"addressof stored then returned", "a.cpp", "CXX-ADDRESSOF",
         "int* f() {\n    int local = 3;\n    int* p = std::addressof(local);\n    return p;\n}\n",
         "p from addressof(local) returned after the object dies", "f"},
        {"addressof deref as index", "a.cpp", "CXX-ADDRESSOF",
         "int f(int* t, int v) {\n    int* p = std::addressof(v);\n    return t[*p];\n}\n",
         "std::addressof result used as an array index without a bound", "f"},
        {"addressof deref index guarded", "a.cpp", "CXX-ADDRESSOF",
         "int f(int* t, int v) {\n    int* p = std::addressof(v);\n    if (*p < 4) return t[*p];\n    return 0;\n}\n",
         nullptr, nullptr},
        {"addressof then an unrelated subscript", "a.cpp", "CXX-ADDRESSOF",
         "int f(int* t, int v, int i) {\n    int* p = std::addressof(v);\n    return t[i] + *p;\n}\n", nullptr,
         nullptr},
        {"return addressof inline", "a.cpp", "CXX-ADDRESSOF",
         "int* f() {\n    int local = 3;\n    return std::addressof(local);\n}\n",
         "std::addressof result used as an array index or stored then the object dies", "f"},
        {"as_const view returned by reference", "a.cpp", "CXX-AS-CONST",
         "const int& f() {\n    int local = 3;\n    const int& r = std::as_const(local);\n    return r;\n}\n",
         "as_const view r of local returned after the local dies", "f"},
        {"as_const returned inline", "a.cpp", "CXX-AS-CONST",
         "const int& f() {\n    int local = 3;\n    return std::as_const(local);\n}\n",
         "as_const view of a local returned after the local dies", "f"},
        {"as_const in a by-value function", "a.cpp", "CXX-AS-CONST",
         "int f() {\n    int local = 3;\n    const int& r = std::as_const(local);\n    return r;\n}\n", nullptr,
         nullptr},
        {"write through basic_const_iterator", "a.cpp", "CXX-CONST-ITERATOR",
         "void f(int* v) {\n    std::basic_const_iterator<int*> it = v;\n    *it = 3;\n}\n",
         "it written through a basic_const_iterator", "f"},
        {"read through basic_const_iterator", "a.cpp", "CXX-CONST-ITERATOR",
         "int f(int* v) {\n    std::basic_const_iterator<int*> it = v;\n    return *it;\n}\n", nullptr, nullptr},
        {"other array indexed next to a named basic_const_iterator", "a.cpp", "CXX-CONST-ITERATOR",
         "int f(int* v, int* t, int i) {\n    std::basic_const_iterator<int*> it = v;\n    return t[i];\n}\n",
         nullptr, nullptr},
    });
}

TEST_CASE("checkers: embed, generator, quick_exit, set_terminate, unreachable") {
    run_cases({
        {"#embed <file> is a literal", "a.cpp", "CXX-EMBED",
         "int f() {\n    static const char d[] = {\n#embed <file.bin>\n    };\n    return sizeof(d);\n}\n", nullptr,
         nullptr},
        {"#embed of a macro", "a.cpp", "CXX-EMBED",
         "int f() {\n    static const char d[] = {\n#embed PATH\n    };\n    return sizeof(d);\n}\n",
         "#embed path is not a string literal", "f"},
        {"#embed storage returned without a size", "a.cpp", "CXX-EMBED",
         "const char* f() {\n    static const char d[] = {\n#embed \"file.bin\"\n    };\n"
         "    const char* p = d;\n    return p;\n}\n",
         "#embed storage returned as a pointer without a size", "f"},
        {"generator<T&> in the return type", "a.cpp", "CXX-GENERATOR",
         "std::generator<const int&> f() {\n    int x = 1;\n    co_yield x;\n}\n",
         "std::generator yields a pointer/reference to a local", "f"},
        {"generator of values", "a.cpp", "CXX-GENERATOR",
         "std::generator<int> f() {\n    int x = 1;\n    co_yield x;\n}\n", nullptr, nullptr},
        {"quick_exit in a destructor despite at_quick_exit", "a.cpp", "CXX-QUICK-EXIT",
         "struct Q {\n    ~Q();\n};\nQ::~Q() {\n    std::at_quick_exit(nullptr);\n    std::quick_exit(1);\n}\n",
         "quick_exit from a destructor", nullptr},
        {"quick_exit after at_quick_exit", "a.cpp", "CXX-QUICK-EXIT",
         "void f() {\n    std::at_quick_exit(nullptr);\n    std::quick_exit(1);\n}\n", nullptr, nullptr},
        {"set_terminate with a throwing handler", "a.cpp", "CXX-SET-TERMINATE",
         "void f() {\n    auto prev = std::get_terminate();\n    std::set_terminate([] { throw 1; });\n}\n",
         "or terminate handler throws", "f"},
        {"set_terminate storing the previous handler", "a.cpp", "CXX-SET-TERMINATE",
         "void f() {\n    auto prev = std::get_terminate();\n    std::set_terminate(h);\n}\n", nullptr, nullptr},
        {"unreachable with a subscript and no return", "a.cpp", "CXX-UNREACHABLE",
         "void f(int* t, int k) {\n    if (k) std::unreachable();\n    t[k] = 1;\n}\n",
         "std::unreachable() used as a recoverable path or in a function that also returns", "f"},
        {"unreachable alone", "a.cpp", "CXX-UNREACHABLE", "void f(int k) {\n    if (k) std::unreachable();\n}\n",
         nullptr, nullptr},
    });
}

TEST_CASE("checkers: per-function gates report the function that passes them") {
    // The first function fails the gate; the finding must be in the second,
    // not the first file-wide match.
    run_cases({
        {"assume", "a.cpp", "CXX-ASSUME",
         "void a() {\n    [[assume(false)]];\n}\nint b() {\n    [[assume(0)]];\n    return 1;\n}\n",
         "[[assume(false)]] / [[assume(0)]] is an unreachable lie", "b"},
        {"generator discard", "a.cpp", "CXX-GENERATOR-DISCARD",
         "void a() {\n    std::generator<int> g = gen();\n    for (int x : g) use(x);\n}\n"
         "void b() {\n    std::generator<int> g = gen();\n}\n",
         "std::generator constructed and not iterated", "b"},
        {"contracts need a return", "a.cpp", "CXX-CONTRACTS",
         "void a() {\n    contract_assert(false);\n}\n", nullptr, nullptr},
        {"contracts", "a.cpp", "CXX-CONTRACTS", "int b() {\n    contract_assert(false);\n    return 1;\n}\n",
         "contract_assert(false)/pre(false) is an unreachable lie", "b"},
        {"std::bind once per function", "a.cpp", "CXX-STD-BIND",
         "void a() {\n    auto g = std::bind(h, 1);\n}\nvoid b() {\n    auto g = std::bind(h, 2);\n}\n",
         "std::bind(); prefer a lambda", "a"},
    });
    Snippet s("a.cpp", "void a() {\n    auto g = std::bind(h, 1);\n}\nvoid b() {\n    auto g = std::bind(h, 2);\n}\n");
    CHECK(s.of("CXX-STD-BIND").size() == 2);
    Snippet ap("a.cpp", "void a() {\n    std::auto_ptr<int> p;\n}\nvoid b() {\n    std::auto_ptr<int> q;\n}\n");
    CHECK(ap.of("CXX-AUTO-PTR").size() == 2);
    Snippet es("a.cpp", "void a() {\n    auto p = shared_from_this();\n}\nvoid b() {\n    auto q = shared_from_this();\n}\n");
    CHECK(es.of("CXX-ENABLE-SHARED").size() == 2);
}

TEST_CASE("checkers: guards that keep correct code silent") {
    run_cases({
        {"endian ranged", "a.cpp", "CXX-ENDIAN",
         "int f(int* t) {\n    if (std::endian::native == std::endian::big) return 1;\n"
         "    return t[(int)std::endian::native];\n}\n",
         nullptr, nullptr},
        {"pack pragma member copied with memcpy", "a.cpp", "CXX-PACK-PRAGMA",
         "#pragma pack(1)\nstruct P { char c; int v; };\n#pragma pack()\n"
         "void f(P* p, int* o) {\n    memcpy(o, &p->v, sizeof(int));\n}\n",
         nullptr, nullptr},
        {"views pipeline over a materialized copy", "a.cpp", "CXX-RANGES-DANGLE",
         "int f() {\n    std::vector<int> v(make().begin(), make().end());\n"
         "    auto r = make() | std::views::take(2);\n    return 0;\n}\n",
         nullptr, nullptr},
        {"text_encoding from utf8()", "a.cpp", "CXX-TEXT-ENCODING",
         "void f() {\n    std::text_encoding e(utf8());\n}\n", nullptr, nullptr},
        {"text_encoding from a variable", "a.cpp", "CXX-TEXT-ENCODING",
         "void f(const char* n) {\n    std::text_encoding e(n);\n}\n",
         "std::text_encoding name is not a string literal", "f"},
        {"sleep_for with no shared store", "a.cpp", "CXX-THIS-THREAD",
         "void f() {\n    std::this_thread::sleep_for(std::chrono::milliseconds(5));\n}\n", nullptr, nullptr},
        {"sleep_for around a shared store", "a.cpp", "CXX-THIS-THREAD",
         "int g_ready;\nvoid f() {\n    g_ready = 1;\n    std::this_thread::sleep_for(std::chrono::milliseconds(5));\n}\n",
         "this_thread::sleep_for used as the only sync around a shared store", "f"},
        {"sleep_for with a lock_guard", "a.cpp", "CXX-THIS-THREAD",
         "int g_ready;\nvoid f() {\n    std::lock_guard<std::mutex> l(m);\n    g_ready = 1;\n"
         "    std::this_thread::sleep_for(std::chrono::milliseconds(5));\n}\n",
         nullptr, nullptr},
        {"error_code passed to be filled", "a.cpp", "CXX-ERROR-CODE",
         "bool f(const std::filesystem::path& p) {\n    std::error_code ec;\n    return fs::exists(p, ec);\n}\n",
         nullptr, nullptr},
        {"error_code read without a test", "a.cpp", "CXX-ERROR-CODE",
         "int f(const std::filesystem::path& p) {\n    std::error_code ec;\n    fs::remove(p, ec);\n"
         "    return ec.value() + log(ec.message());\n}\n",
         nullptr, nullptr},
        {"error_code message read unchecked", "a.cpp", "CXX-ERROR-CODE",
         "void f(const std::filesystem::path& p) {\n    std::error_code ec;\n    fs::remove(p, ec);\n"
         "    log(ec.message());\n}\n",
         "ec used without if(ec) or .value()", "f"},
        {"filesystem_error is not system_error", "a.cpp", "CXX-SYSTEM-ERROR",
         "int f() {\n    try { g(); } catch (const std::filesystem::filesystem_error& e) {\n"
         "        log(e.what());\n    }\n    return 0;\n}\n",
         nullptr, nullptr},
        {"system_error thrown without a code check", "a.cpp", "CXX-SYSTEM-ERROR",
         "void f(int e) {\n    throw std::system_error(e, std::generic_category());\n}\n",
         "std::system_error used without a .code() check", "f"},
        {"nested exception inside try/catch", "a.cpp", "CXX-NESTED-EXCEPTION",
         "void f() {\n    try { g(); } catch (...) { std::throw_with_nested(E()); }\n}\n", nullptr, nullptr},
        {"nested exception outside a handler", "a.cpp", "CXX-NESTED-EXCEPTION",
         "void f() {\n    std::throw_with_nested(E());\n}\n",
         "throw_with_nested / nested_exception without a try/catch", "f"},
        {"timed_mutex try_lock tested in while", "a.cpp", "CXX-TIMED-MUTEX",
         "void f() {\n    std::timed_mutex m;\n    while (!m.try_lock()) {}\n    m.unlock();\n}\n", nullptr, nullptr},
        {"timed_mutex try_lock returned", "a.cpp", "CXX-TIMED-MUTEX",
         "bool f() {\n    std::timed_mutex m;\n    return m.try_lock();\n}\n", nullptr, nullptr},
        {"timed_mutex try_lock discarded", "a.cpp", "CXX-TIMED-MUTEX",
         "void f() {\n    std::timed_mutex m;\n    m.try_lock();\n    m.unlock();\n}\n",
         "m try_lock/lock discarded or lock without unlock", "f"},
        {"uninitialized_fill: only its destination counts", "a.cpp", "CXX-UNINITIALIZED-FILL",
         "int f(int* d, int* t, int i, int n) {\n    std::uninitialized_fill(d, d + n, 0);\n    return t[i];\n}\n",
         nullptr, nullptr},
        {"uninitialized_fill destination indexed", "a.cpp", "CXX-UNINITIALIZED-FILL",
         "int f(int* d, int i, int n) {\n    std::uninitialized_fill(d, d + n, 0);\n    return d[i];\n}\n",
         "uninitialized_fill dest indexed without a size guard", "f"},
        {"get() before release()", "a.cpp", "CXX-UNIQUE-RELEASE",
         "int* f() {\n    std::unique_ptr<int> p(new int);\n    int* r = p.get();\n    p.release();\n    return r;\n}\n",
         nullptr, nullptr},
        {"get() after release()", "a.cpp", "CXX-UNIQUE-RELEASE",
         "int* f() {\n    std::unique_ptr<int> p(new int);\n    p.release();\n    return p.get();\n}\n",
         "p.get() used after release()", "f"},
        {"ranges::to: only its result counts", "a.cpp", "CXX-RANGES-TO",
         "int f(int* t, int i) {\n    auto v = std::ranges::to<std::vector>(r);\n    return t[i];\n}\n", nullptr,
         nullptr},
        {"ranges::to result indexed", "a.cpp", "CXX-RANGES-TO",
         "int f(int i) {\n    auto v = std::ranges::to<std::vector>(r);\n    return v[i];\n}\n",
         "ranges::to container indexed without a size guard", "f"},
        {"counted_iterator count compared with size()", "a.cpp", "CXX-COUNTED-ITERATOR",
         "void f(std::vector<int>& v, int n) {\n    if (n > v.size()) return;\n"
         "    std::counted_iterator it(v.begin(), n);\n}\n",
         nullptr, nullptr},
        {"counted_iterator count unguarded", "a.cpp", "CXX-COUNTED-ITERATOR",
         "void f(std::vector<int>& v, int n) {\n    std::counted_iterator it(v.begin(), n);\n}\n",
         "counted_iterator count n has no remaining-size guard", "f"},
        {"to_underlying of a compared enum", "a.cpp", "CXX-TO-UNDERLYING",
         "int f(int* t, E e) {\n    if (e == E::last) return 0;\n    return t[std::to_underlying(e)];\n}\n", nullptr,
         nullptr},
        {"to_underlying of an unchecked enum", "a.cpp", "CXX-TO-UNDERLYING",
         "int f(int* t, E e) {\n    return t[std::to_underlying(e)];\n}\n",
         "to_underlying(e) without an enum-range guard", "f"},
        {"unexpected used later", "a.cpp", "CXX-UNEXPECTED",
         "void f() {\n    std::unexpected<int> u(1);\n    g(u);\n}\n", nullptr, nullptr},
        {"unexpected discarded", "a.cpp", "CXX-UNEXPECTED", "void f() {\n    std::unexpected<int> u(1);\n}\n",
         "u unexpected< constructed and discarded", "f"},
        {"expected error() after has_value()==false", "a.cpp", "CXX-EXPECTED-ERROR",
         "int f() {\n    std::expected<int, int> e = g();\n    if (e.has_value() == false) return e.error();\n"
         "    return 0;\n}\n",
         nullptr, nullptr},
        {"expected error() unguarded", "a.cpp", "CXX-EXPECTED-ERROR",
         "int f() {\n    std::expected<int, int> e = g();\n    return e.error();\n}\n",
         "e.error() without a prior !e check", "f"},
        {"variant index() after emplace", "a.cpp", "CXX-VARIANT-VALUELSS",
         "int f(std::variant<int, float>& v) {\n    v.emplace<1>(2.0f);\n    if (std::holds_alternative<int>(v)) return 0;\n"
         "    return t[v.index()];\n}\n",
         "std::get after emplace without valueless_by_exception()", "f"},
        {"variant checked for valueless", "a.cpp", "CXX-VARIANT-VALUELSS",
         "int f(std::variant<int, float>& v) {\n    v.emplace<1>(2.0f);\n    if (v.valueless_by_exception()) return 0;\n"
         "    return std::get<1>(v);\n}\n",
         nullptr, nullptr},
        {"weak_ptr lock() used directly", "a.cpp", "CXX-WEAK-PTR",
         "int f(std::weak_ptr<int> w) {\n    std::weak_ptr<int> x = w;\n    return *x.lock();\n}\n",
         "x.lock() used without expired() or a truth test", "f"},
        {"weak_ptr lock() result tested", "a.cpp", "CXX-WEAK-PTR",
         "int f(std::weak_ptr<int> w) {\n    std::weak_ptr<int> x = w;\n    auto p = x.lock();\n"
         "    if (p) return *p;\n    return 0;\n}\n",
         nullptr, nullptr},
        {"lambda capturing only this by reference", "a.cpp", "CXX-LAMBDA-DANGLE",
         "cb S::f() const {\n    return [&](int x) {\n        return x + member;\n    };\n}\n", nullptr, nullptr},
        {"lambda capturing a local by reference", "a.cpp", "CXX-LAMBDA-DANGLE",
         "cb f() {\n    int local = 1;\n    return [&](int x) {\n        return x + local;\n    };\n}\n",
         "returned lambda captures local by reference", "f"},
        {"bit_cast of a char pointer", "a.cpp", "CXX-BIT-CAST",
         "std::uintptr_t f(char* p) {\n    auto q = p;\n    return std::bit_cast<std::uintptr_t>(p);\n}\n", nullptr,
         nullptr},
        {"bit_cast of an object pointer", "a.cpp", "CXX-BIT-CAST",
         "std::uintptr_t f(S* p) {\n    return std::bit_cast<std::uintptr_t>(p);\n}\n",
         "bit_cast type-puns an object pointer", "f"},
        {"bit_cast of a float value", "a.cpp", "CXX-BIT-CAST",
         "unsigned f(float x) {\n    return std::bit_cast<unsigned>(x);\n}\n", nullptr, nullptr},
        {"current_exception bound then rethrown unchecked", "a.cpp", "CXX-CURRENT-EXCEPTION",
         "void f() {\n    auto e = std::current_exception();\n    std::rethrow_exception(e);\n}\n",
         "rethrow_exception(e) from current_exception without a nullptr test", "f"},
        {"current_exception bound then tested", "a.cpp", "CXX-CURRENT-EXCEPTION",
         "void f() {\n    auto e = std::current_exception();\n    if (e) std::rethrow_exception(e);\n}\n", nullptr,
         nullptr},
        {"member assigned in the constructor body", "a.cpp", "CXX-UNINIT-MEMBER",
         "struct R {\n    int n;\n    R() {\n        n = 0;\n    }\n};\nint f() {\n    R r;\n    return r.n;\n}\n",
         nullptr, nullptr},
        {"member left uninitialised", "a.cpp", "CXX-UNINIT-MEMBER",
         "struct R {\n    int n;\n    R() {\n    }\n};\nint f() {\n    R r;\n    return r.n;\n}\n",
         "R() leaves scalar member n uninitialised", "f"},
        {"c_str() pointer declared after mutation, address taken", "a.cpp", "CXX-STRING-DATA",
         "void f() {\n    std::string s(\"a\");\n    s += \"b\";\n    const auto * p = s.c_str();\n    pp = &p;\n}\n",
         nullptr, nullptr},
        {"c_str() pointer used after mutation", "a.cpp", "CXX-STRING-DATA",
         "void f() {\n    std::string s(\"a\");\n    const char* p = s.c_str();\n    s += \"b\";\n    use(p);\n}\n",
         "p from s.c_str()/data() used after mutation", "f"},
    });
}

TEST_CASE("checkers: messages of the reworded C++ rules") {
    run_cases({
        {"apply", "a.cpp", "CXX-APPLY", "void f(F fn) {\n    std::apply(fn, t);\n}\n",
         "std::apply on a nullable callable or tuple without a size/tuple_size guard", "f"},
        {"apply with tuple_size", "a.cpp", "CXX-APPLY",
         "void f(F fn) {\n    static_assert(std::tuple_size<T>::value == 2);\n    std::apply(fn, t);\n}\n", nullptr,
         nullptr},
        {"binary_semaphore", "a.cpp", "CXX-BINARY-SEMAPHORE",
         "void f() {\n    std::binary_semaphore s(0);\n    s.acquire();\n}\n",
         "s.acquire() on binary_semaphore(0) without try_acquire()/release()", "f"},
        {"binary_semaphore released", "a.cpp", "CXX-BINARY-SEMAPHORE",
         "void f() {\n    std::binary_semaphore s(0);\n    other.release();\n    s.acquire();\n}\n", nullptr, nullptr},
        {"call_once", "a.cpp", "CXX-CALL-ONCE", "void f() {\n    std::call_once(flag, g);\n}\n",
         "std::call_once invoked without an once_flag object in scope", "f"},
        {"format_to", "a.cpp", "CXX-FORMAT-TO", "void f(int x) {\n    char buf[8];\n    std::format_to(buf, \"{}\", x);\n}\n",
         "format_to into a raw char buffer without size / format_to_n", "f"},
        {"inplace_vector emplace_back", "a.cpp", "CXX-INPLACE-VECTOR",
         "void f() {\n    std::inplace_vector<int, 2> v;\n    v.emplace_back(1);\n}\n",
         "v.push_back without size()/capacity()/try_push_back()", "f"},
        {"sstream view", "a.cpp", "CXX-SSTREAM-VIEW",
         "void f() {\n    std::stringstream ss;\n    std::string_view v = ss.str();\n}\n",
         "string_view of ss.str() temporary", "f"},
        {"uncaught_exceptions", "a.cpp", "CXX-UNCAUGHT-EXCEPTIONS",
         "S::~S() {\n    if (std::uncaught_exceptions()) return;\n}\n",
         "std::uncaught_exceptions() used as a boolean destructor-safety check without a saved count", nullptr},
        {"nontype", "a.cpp", "CXX-NONTYPE", "int f(int* t, int i) {\n    auto h = std::nontype<g>;\n    return t[i];\n}\n",
         "std::nontype used as an unguarded index or to bypass a bound", "f"},
        {"bitset index parameter", "a.cpp", "CXX-BITSET-INDEX",
         "bool f(int n) {\n    std::bitset<8> b;\n    return b.test(n);\n}\n", "b.test/b[n] without a size()/N guard",
         "f"},
        {"bitset index parameter guarded", "a.cpp", "CXX-BITSET-INDEX",
         "bool f(int n) {\n    std::bitset<8> b;\n    if (n < 8) return b.test(n);\n    return false;\n}\n", nullptr,
         nullptr},
        {"bitset index local constant", "a.cpp", "CXX-BITSET-INDEX",
         "bool f() {\n    std::bitset<8> b;\n    return b[5];\n}\n", nullptr, nullptr},
        {"function_ref to a temporary", "a.cpp", "CXX-FUNCTION-REF",
         "void f() {\n    std::function_ref<int()> r = [] { return 1; };\n    r();\n}\n",
         "function_ref bound to a temporary lambda or call", "f"},
        {"array index", "a.cpp", "CXX-ARRAY-INDEX", "int f(int i) {\n    std::array<int, 4> a{};\n    return a[i];\n}\n",
         "a[i] without a size()/N guard", "f"},
        {"array index against N", "a.cpp", "CXX-ARRAY-INDEX",
         "int f(int i) {\n    std::array<int, 4> a{};\n    if (i < 4) return a[i];\n    return 0;\n}\n", nullptr, nullptr},
        {"notify_all_at_thread_exit", "a.cpp", "CXX-NOTIFY-THREAD-EXIT",
         "void f() {\n    std::notify_all_at_thread_exit(cv, std::move(lk));\n}\n",
         "notify_all_at_thread_exit without a waiting condition_variable", "f"},
    });
}
