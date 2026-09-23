// Doctests for the Clang-AST lint layer (roadmap 2.8, src/prism/astlint.cpp).
// Planted bugs must be found at their line; the false-positive guards must
// stay silent; no clang / a unit that does not parse is NOTRUN (Law 1 / 7)
// while the regex lints still run.

#include <doctest/doctest.h>

#include "prism/astlint.hpp"
#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;

std::optional<fs::path> find_clang() {
    prism::Config cfg;
    auto fe = prism::pir::find_frontend(cfg);
    return fe.clang ? fe.clang : fe.clangxx;
}

struct Tree {
    fs::path dir;
    Tree() {
        auto t = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        dir = fs::temp_directory_path() / ("prism_astlint_" + std::to_string(t));
        fs::create_directories(dir);
    }
    ~Tree() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path put(const std::string& rel, const std::string& text) const {
        auto p = dir / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
        return p;
    }
};

std::vector<prism::Finding> lint(const Tree& t, const std::vector<fs::path>& files,
                                 const std::optional<fs::path>& clang) {
    prism::Config cfg;
    cfg.jobs = 1;
    cfg.root = t.dir;
    return prism::astlint::run_lints_ast(files, t.dir, cfg, clang);
}

using prism::astlint::Backend;

std::vector<prism::Finding> lint_with(const Tree& t, const std::vector<fs::path>& files,
                                      const std::optional<fs::path>& clang, Backend b) {
    prism::Config cfg;
    cfg.jobs = 1;
    cfg.root = t.dir;
    return prism::astlint::run_lints_ast(files, t.dir, cfg, clang, b);
}

// The front ends to test on this machine: the clang process when clang is
// installed, libclang when it loads. Both must give the same findings.
std::vector<std::pair<std::string, Backend>> front_ends(const std::optional<fs::path>& clang) {
    std::vector<std::pair<std::string, Backend>> out;
    if (clang) out.emplace_back("process", Backend::Process);
    if (prism::astlint::libclang_available()) out.emplace_back("libclang", Backend::LibClang);
    return out;
}

std::set<std::pair<int, std::string>> ast_hits(const std::vector<prism::Finding>& fs_) {
    std::set<std::pair<int, std::string>> out;
    for (auto& f : fs_) {
        auto it = f.extra.find("engine");
        if (it != f.extra.end() && it->second == "clang-ast" && f.status == prism::laws::FAILED)
            out.insert({f.line.value_or(0), f.cls});
    }
    return out;
}

const char* kPlantedC = R"(#include <string.h>
enum color { RED, GREEN, BLUE };
int take(int *p, unsigned n, enum color c, char *buf) {
    int x = 0;
    if (x = *p) return 1;
    memset(buf, 0, sizeof(buf));
    memset(p, 7, 0);
    for (int i = 0; i < n; i++) p[i] = 0;
    switch (c) { case RED: return 1; case GREEN: return 2; }
    x = x;
    int dead;
    dead = 4;
    double avg = x / 3;
    return (int)avg + x;
}
)";

}  // namespace

TEST_CASE("astlint: planted C bugs are found at their lines") {
    auto clang = find_clang();
    if (!clang) {
        MESSAGE("clang not installed: AST lint planted test skipped");
        return;
    }
    Tree t;
    auto p = t.put("planted.c", kPlantedC);
    for (auto& [fe, b] : front_ends(clang)) {
        INFO("front end: " << fe);
        auto out = lint_with(t, {p}, clang, b);
        auto hits = ast_hits(out);
        CHECK(hits.contains({5, "CTRL-ASSIGN-COND"}));
        CHECK(hits.contains({6, "MEM-SIZEOF-PTR"}));
        CHECK(hits.contains({7, "MEM-MEMSET-SWAP"}));
        CHECK(hits.contains({8, "INT-SIGN-CONV"}));
        CHECK(hits.contains({9, "INT-ENUM-HOLE"}));
        CHECK(hits.contains({10, "CTRL-SELF-ASSIGN"}));
        CHECK(hits.contains({12, "CTRL-DEAD-STORE"}));
        CHECK(hits.contains({13, "INT-DIV-TO-FLOAT"}));
        // x is read, so it is not a dead store; nothing else is reported.
        CHECK(hits.size() == 8);
        for (auto& f : out) {
            if (f.extra.contains("engine")) {
                CHECK(f.stage == "lints");
                CHECK(f.file == "planted.c");
                CHECK(f.function == std::optional<std::string>("take"));
                CHECK(f.strength == prism::laws::STRENGTH_FINDS);
            }
            CHECK(f.status != prism::laws::NOTRUN);  // the layer ran
        }
        // The enum-hole message names the missing enumerator.
        for (auto& f : out)
            if (f.cls == "INT-ENUM-HOLE" && f.extra.contains("engine"))
                CHECK(f.message.find("BLUE") != std::string::npos);
    }
}

TEST_CASE("astlint: false-positive guards stay silent") {
    auto clang = find_clang();
    if (!clang) return;
    Tree t;
    auto p = t.put("clean.c", R"(#include <stdio.h>
#include <string.h>
#include <stddef.h>
#define LEN 0
enum mode { M_A, M_B, M_ALIAS = M_A };
typedef enum { T_X, T_Y } tkind;
int next(void);
int use(int *p, size_t n, enum mode m, tkind k, char **out, int a, int b) {
    int x, c, total = 0;
    if ((x = next())) total += x;
    while ((c = getchar()) != EOF) total += c;
    for (size_t i = 0; i < n; i++) p[i] = 0;
    for (int j = 0; j < 10; j++) total += j;
    for (unsigned u = 0; u < n; u++) total++;
    switch (m) { case M_A: total++; break; case M_B: total--; break; }
    switch (k) { case T_X: total++; break; default: break; }
    switch (a) { case 1: total++; break; }
    memset(p, 0, n);
    memset(p, 0xff, LEN);
    memset(p, 0, sizeof(*p));
    memcpy(out, &p, sizeof(p));
    volatile int reg = 0;
    reg = reg;
    int unused_ok __attribute__((unused));
    unused_ok = 3;
    double half = 10 / 2;
    double ratio = (double)a / b;
    double same = a / 1;
    int kept;
    kept = a;
    return total + (int)half + (int)ratio + (int)same + kept;
}
)");
    for (auto& [fe, b] : front_ends(clang)) {
        INFO("front end: " << fe);
        auto out = lint_with(t, {p}, clang, b);
        auto hits = ast_hits(out);
        for (auto& h : hits) MESSAGE(fe << " unexpected: line " << h.first << " " << h.second);
        CHECK(hits.empty());
        for (auto& f : out) CHECK(f.status != prism::laws::NOTRUN);
    }
}

TEST_CASE("astlint: C++ unit with the standard library, enum class, templates") {
    auto clang = find_clang();
    if (!clang) return;
    Tree t;
    auto p = t.put("m.cpp", R"(#include <vector>
#include <string>
enum class Dir { North, South, East };
struct Walker {
    int step(Dir d, const std::vector<int>& v) {
        int last;
        last = 5;
        switch (d) { case Dir::North: return 1; case Dir::South: return 2; }
        for (int i = 0; i < v.size(); ++i) {}
        return 0;
    }
};
template <class T> int tmpl(T a) { int unused; unused = a; return 0; }
int lam() { auto f = [](int q) { int w; w = q; return 0; }; return f(1); }
)");
    for (auto& [fe, b] : front_ends(clang)) {
        INFO("front end: " << fe);
        auto out = lint_with(t, {p}, clang, b);
        auto hits = ast_hits(out);
        CHECK(hits.contains({7, "CTRL-DEAD-STORE"}));
        CHECK(hits.contains({8, "INT-ENUM-HOLE"}));
        CHECK(hits.contains({9, "INT-SIGN-CONV"}));
        CHECK(hits.contains({14, "CTRL-DEAD-STORE"}));  // lambda body: one row, not two
        // Dependent template code stays unchecked: `unused = a` stores a T.
        CHECK_FALSE(hits.contains({13, "CTRL-DEAD-STORE"}));
        CHECK(hits.size() == 4);
    }
}

TEST_CASE("astlint: no clang is NOTRUN for the layer and the regex lints still run") {
    Tree t;
    auto p = t.put("g.c", "#include <stdio.h>\nvoid f(void) { char b[8]; gets(b); puts(b); }\n");
    auto h = t.put("g.h", "int g(void);\n");
    auto out = lint(t, {p, h}, std::nullopt);
    int notrun = 0;
    bool regex = false;
    for (auto& f : out) {
        if (f.status == prism::laws::NOTRUN) {
            ++notrun;
            CHECK(prism::astlint::is_layer_row(f));
            CHECK(f.extra.at("layer") == "clang-ast");
        }
        if (f.cls == "API-GETS" && f.status == prism::laws::FAILED) regex = true;
        CHECK_FALSE(f.extra.contains("engine"));
    }
    CHECK(notrun == 2);  // the missing clang, and the header note
    CHECK(regex);
}

TEST_CASE("astlint: a unit that does not parse is NOTRUN for that file only") {
    auto clang = find_clang();
    if (!clang) return;
    Tree t;
    auto bad = t.put("bad.c", "int f(void) { return }\n");
    auto good = t.put("good.c", "void g(int *p) { int y; y = *p; }\n");
    auto out = lint(t, {bad, good}, clang);
    bool bad_nr = false;
    for (auto& f : out) {
        if (f.status == prism::laws::NOTRUN) {
            CHECK(f.file == "bad.c");
            CHECK(f.message.find("does not parse") != std::string::npos);
            CHECK(prism::astlint::is_layer_row(f));
            bad_nr = true;
        }
    }
    CHECK(bad_nr);
    CHECK(ast_hits(out).contains({1, "CTRL-DEAD-STORE"}));
}

TEST_CASE("astlint: an AST finding supersedes the regex finding on the same line") {
    auto clang = find_clang();
    if (!clang) return;
    Tree t;
    auto p = t.put("s.c", "#include <string.h>\nvoid z(char *p, int n) {\n    memset(p, n, 0);\n}\n");
    auto out = lint(t, {p}, clang);
    int rows = 0;
    for (auto& f : out) {
        if (f.cls != "MEM-MEMSET-SWAP") continue;
        ++rows;
        CHECK(f.line == 3);
        CHECK(f.extra.at("engine") == "clang-ast");
        CHECK(f.extra.at("supersedes") == "regex");
    }
    CHECK(rows == 1);
    // Without clang the same line is still reported, by the regex lint.
    auto regex_only = lint(t, {p}, std::nullopt);
    int regex_rows = 0;
    for (auto& f : regex_only)
        if (f.cls == "MEM-MEMSET-SWAP" && f.line == 3) ++regex_rows;
    CHECK(regex_rows == 1);
}

TEST_CASE("astlint: compile_commands.json flags reach clang") {
    auto clang = find_clang();
    if (!clang) return;
    Tree t;
    t.put("inc/cfg.h", "#define WIDTH 4\n");
    auto p = t.put("src/u.c", "#include \"cfg.h\"\n#ifndef NEED_FLAG\n#error flag missing\n#endif\n"
                              "int w(int a) { int d; d = a * WIDTH; return 0; }\n");
    auto fl = prism::astlint::compile_db_flags(t.dir, p);
    CHECK(fl.empty());
    auto without = lint(t, {p}, clang);
    bool nr = false;
    for (auto& f : without) nr = nr || prism::astlint::is_layer_row(f);
    CHECK(nr);
    t.put("build/compile_commands.json",
          "[{\"directory\": \"" + (t.dir / "build").generic_string() +
              "\", \"command\": \"cc -I../inc -DNEED_FLAG=1 -O2 -c ../src/u.c\", \"file\": \"../src/u.c\"}]");
    fl = prism::astlint::compile_db_flags(t.dir, p);
    REQUIRE(fl.size() == 2);
    CHECK(fl[0].starts_with("-I"));
    CHECK(fl[1] == "-DNEED_FLAG=1");
    auto with = lint(t, {p}, clang);
    CHECK(ast_hits(with).contains({5, "CTRL-DEAD-STORE"}));
    for (auto& f : with) CHECK(f.status != prism::laws::NOTRUN);
}

TEST_CASE("astlint: dump splitting keeps only the main file and tracks elided file names") {
    // Hand-written dump in clang's shape: the header decl names its file, the
    // main-file function inherits "file" from the location printed before it
    // (the dumper elides a repeated file), and includedFrom is not a location.
    std::string src = "int f(int *p) {\n  int a;\n  a = 1;\n  return 0;\n}\n";
    std::string dump = R"({"id":"0x1","kind":"TranslationUnitDecl","loc":{},"range":{"begin":{},"end":{}},"inner":[
 {"id":"0x2","kind":"TypedefDecl","loc":{},"range":{"begin":{},"end":{}},"isImplicit":true,"name":"t"},
 {"id":"0x3","kind":"FunctionDecl","loc":{"offset":4,"file":"/h.h","line":1,"col":5,"tokLen":1,"includedFrom":{"file":"/m.c"}},"range":{"begin":{"offset":0,"col":1,"tokLen":3},"end":{"offset":9,"col":10,"tokLen":1}},"name":"hdr"},
 {"id":"0x4","kind":"FunctionDecl","loc":{"offset":4,"file":"/m.c","line":1,"col":5,"tokLen":1},"range":{"begin":{"offset":0,"col":1,"tokLen":3},"end":{"offset":50,"line":5,"col":1,"tokLen":1}},"name":"f","inner":[
  {"id":"0x5","kind":"ParmVarDecl","loc":{"offset":11,"col":12,"tokLen":1},"range":{"begin":{"offset":6,"col":7,"tokLen":3},"end":{"offset":11,"col":12,"tokLen":1}},"name":"p","type":{"qualType":"int *"}},
  {"id":"0x6","kind":"CompoundStmt","range":{"begin":{"offset":14,"col":15,"tokLen":1},"end":{"offset":50,"line":5,"col":1,"tokLen":1}},"inner":[
   {"id":"0x7","kind":"DeclStmt","range":{"begin":{"offset":18,"line":2,"col":3,"tokLen":3},"end":{"offset":23,"col":8,"tokLen":1}},"inner":[
    {"id":"0x8","kind":"VarDecl","loc":{"offset":22,"col":7,"tokLen":1},"range":{"begin":{"offset":18,"col":3,"tokLen":3},"end":{"offset":22,"col":7,"tokLen":1}},"name":"a","type":{"qualType":"int"}}]},
   {"id":"0x9","kind":"BinaryOperator","range":{"begin":{"offset":27,"line":3,"col":3,"tokLen":1},"end":{"offset":31,"col":7,"tokLen":1}},"type":{"qualType":"int"},"opcode":"=","inner":[
    {"id":"0xa","kind":"DeclRefExpr","range":{"begin":{"offset":27,"col":3,"tokLen":1},"end":{"offset":27,"col":3,"tokLen":1}},"type":{"qualType":"int"},"referencedDecl":{"id":"0x8","kind":"VarDecl","name":"a","type":{"qualType":"int"}}},
    {"id":"0xb","kind":"IntegerLiteral","range":{"begin":{"offset":31,"col":7,"tokLen":1},"end":{"offset":31,"col":7,"tokLen":1}},"type":{"qualType":"int"},"value":"1"}]}]}]}
]})";
    auto r = prism::astlint::analyze_dump(dump, "/m.c", src, "m.c");
    REQUIRE(r.ran);
    REQUIRE(r.findings.size() == 1);
    CHECK(r.findings[0].cls == "CTRL-DEAD-STORE");
    CHECK(r.findings[0].line == 3);
    CHECK(r.findings[0].function == std::optional<std::string>("f"));
    // The header function is not analysed, and a truncated dump is an error.
    auto bad = prism::astlint::analyze_dump(dump.substr(0, dump.size() / 2), "/m.c", src, "m.c");
    CHECK_FALSE(bad.ran);
    CHECK_FALSE(bad.error.empty());
    auto junk = prism::astlint::analyze_dump("error: nothing", "/m.c", src, "m.c");
    CHECK_FALSE(junk.ran);
}

// ------------------------------------------------------------------ ported regex classes
// Each planted line carries `// expect: CLASS` (several separated by commas);
// the AST layer must report exactly those (line, class) pairs on both front
// ends, so every unmarked line is also a false-positive guard.

namespace {

std::set<std::pair<int, std::string>> expected_marks(const std::string& text) {
    std::set<std::pair<int, std::string>> out;
    int line = 1;
    std::size_t at = 0;
    while (at <= text.size()) {
        auto nl = text.find('\n', at);
        auto ln = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        if (auto m = ln.find("// expect: "); m != std::string::npos) {
            std::string rest = ln.substr(m + 11);
            std::size_t b = 0;
            while (b < rest.size()) {
                auto e = rest.find(',', b);
                auto cls = rest.substr(b, e == std::string::npos ? std::string::npos : e - b);
                while (!cls.empty() && cls.front() == ' ') cls.erase(cls.begin());
                while (!cls.empty() && cls.back() == ' ') cls.pop_back();
                if (!cls.empty()) out.insert({line, cls});
                if (e == std::string::npos) break;
                b = e + 1;
            }
        }
        if (nl == std::string::npos) break;
        at = nl + 1;
        ++line;
    }
    return out;
}

// Lint `name` (plus `extra` files: path, text, path, text...) on every front
// end and compare the AST rows of the file named `report` (default `name`)
// with its `// expect:` marks.
void check_marks(const std::string& name, const std::string& text, const std::vector<std::string>& extra = {},
                 const std::string& report = "") {
    auto clang = find_clang();
    auto fes = front_ends(clang);
    if (fes.empty()) {
        MESSAGE("clang / libclang not installed: " << name << " skipped");
        return;
    }
    Tree t;
    std::vector<fs::path> files{t.put(name, text)};
    std::string marked = text;
    for (std::size_t i = 0; i + 1 < extra.size(); i += 2) {
        files.push_back(t.put(extra[i], extra[i + 1]));
        if (extra[i] == report) marked = extra[i + 1];
    }
    const std::string target = report.empty() ? name : report;
    auto want = expected_marks(marked);
    for (auto& [fe, b] : fes) {
        INFO("front end: " << fe << ", file " << target);
        auto out = lint_with(t, files, clang, b);
        std::set<std::pair<int, std::string>> got;
        for (auto& f : out) {
            if (f.status == prism::laws::NOTRUN && prism::astlint::is_layer_row(f) && !f.file.empty())
                FAIL_CHECK("NOTRUN: " << f.message);
            auto it = f.extra.find("engine");
            if (it != f.extra.end() && f.file == target && f.status == prism::laws::FAILED) {
                got.insert({f.line.value_or(0), f.cls});
                CHECK(f.extra.at("ast_backend") == fe);
            }
        }
        for (auto& w : want)
            if (!got.contains(w)) FAIL_CHECK("missed: line " << w.first << " " << w.second);
        for (auto& g : got)
            if (!want.contains(g)) FAIL_CHECK("unexpected: line " << g.first << " " << g.second);
    }
}

const char* kPortedC = R"(#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct node { int v; struct node *next; };
int null_after_check(struct node *n) {
    if (n == NULL) { puts("none"); }
    return n->v; // expect: PTR-NULL-DEREF
}
int null_guarded(struct node *n) {
    if (n == NULL) return 0;
    return n->v;
}
int null_in_branch(struct node *n) {
    if (!n) { // expect: PTR-NULL-DEREF
        return n->v;
    }
    return 0;
}
int null_reassigned(struct node *n, struct node *dflt) {
    if (n == NULL) { puts("default"); n = dflt; }
    return n->v;
}
int null_exits(struct node *n) {
    if (n == NULL) { puts("fatal"); exit(1); }
    return n->v;
}
int null_in_loop(struct node *n) {
    int s = 0;
    for (; n; n = n->next) {
        if (n->next == NULL) break;
        s += n->v;
    }
    return s;
}
void fmt_args(long l, const char *s, int i) {
    printf("%d %s\n", l, s); // expect: FMT-ARGS
    printf("%s %d\n", s); // expect: FMT-ARGS
    printf("%ld %s %d %%\n", l, s, i);
    printf("%zu %lu %hd %c %5.2f\n", sizeof s, (unsigned long)i, (short)i, 'c', 1.0);
    printf("%lld\n", i); // expect: FMT-ARGS
}
void fmt_string(const char *user) {
    printf(user); // expect: FMT-STRING
    printf("%s", user);
}
void percent_n(int *n) { printf("abc%n", n); } // expect: FMT-PERCENT-N
int fall(int k) {
    int r = 0;
    switch (k) {
    case 1:
        r = 1; // expect: CTRL-FALLTHROUGH
        r++;
    case 2:
        r += 2;
        break;
    case 3:
        r = 3;
        __attribute__((fallthrough));
    case 4:
    case 5:
        r += 5;
        break;
    }
    return r;
}
int never_neg(unsigned u, int s) {
    if (u < 0) return 1; // expect: CTRL-DEAD-GUARD
    if (s < 0) return 2;
    return 0;
}
size_t arr_param(char buf[16]) { return sizeof(buf); } // expect: MEM-SIZEOF-PTR
size_t arr_local(void) { char buf[16]; return sizeof(buf); }
void ncopy(const char *src) {
    char d[8];
    strncpy(d, src, sizeof d); // expect: STR-STRNCPY-NUL
    puts(d);
    char e[8];
    strncpy(e, src, sizeof e - 1);
    e[sizeof e - 1] = '\0';
    puts(e);
}
int *escape(void) {
    int x = 1;
    return &x; // expect: MEM-STACK-ESCAPE
}
int *no_escape(void) {
    static int y = 1;
    return &y;
}
int uninit(int a) {
    int y;
    int z = y + a; // expect: UNINIT-READ
    int w;
    w = a;
    return z + w;
}
int shadow(int a) {
    int t = a;
    {
        int t = 2; // expect: CTRL-SHADOW
        a += t;
    }
    return a + t;
}
void uaf(void) {
    char *p = malloc(4);
    if (!p) return;
    free(p);
    p[0] = 1; // expect: MEM-UAF
}
void no_uaf(void) {
    char *p = malloc(4);
    free(p);
    p = malloc(4);
    if (p) p[0] = 1;
    free(p);
}
void dfree(char *p) {
    free(p);
    free(p); // expect: MEM-DOUBLE-FREE
}
int shift(int x) { return x << 40; } // expect: INT-SHIFT-UB
int shift_ok(unsigned long long x) { return (int)(x << 40); }
void ignored(void) { malloc(10); } // expect: API-IGNORED-ERROR
void kept(void) { void *m = malloc(10); free(m); }
void overlap(char *s) { strcpy(s, s); } // expect: MEM-OVERLAP
char *rself(char *p) {
    p = realloc(p, 10); // expect: MEM-REALLOC-SELF
    return p;
}
char *rsafe(char *p) {
    char *q = realloc(p, 10);
    if (!q) return p;
    return q;
}
void fixed_copy(char *out) {
    char small[4];
    strcpy(small, "overflow"); // expect: STR-OFF-BY-ONE
    strcpy(out, "overflow");
    puts(small);
}
int bool_bit(int a, int b) { return (a > 0) & (b > 0) ? 1 : !a & 4; } // expect: INT-BOOL-AS-BIT
)";

const char* kPortedCxx = R"(#include <string>
#include <utility>
#include <vector>
[[nodiscard]] int must(int);
struct Base {
    virtual void f();
    ~Base();
};
struct Derived : Base {
    void f() override;
};
void kill(Base *b) { delete b; } // expect: CXX-MISSING-VIRTUAL-DTOR
struct Good {
    virtual void f();
    virtual ~Good();
};
void kill_ok(Good *g) { delete g; }
struct Ctor {
    Ctor() { init(); } // expect: CXX-VIRTUAL-IN-CTOR
    virtual void init();
    virtual ~Ctor() { Ctor::init(); }
};
void self_move(std::string s) {
    s = std::move(s); // expect: CXX-SELF-MOVE
    std::string t;
    t = std::move(s);
}
const char *dangling(int n) {
    std::string s(static_cast<std::size_t>(n), 'x');
    return s.c_str(); // expect: CXX-DANGLING-REF
}
std::string by_value(int n) {
    std::string s(static_cast<std::size_t>(n), 'x');
    return s;
}
const char *static_ok() {
    static std::string s = "x";
    return s.c_str();
}
void after_move(std::string s) {
    std::string t = std::move(s);
    t += s; // expect: CXX-USE-AFTER-MOVE
    s = "again";
    t += s;
}
void nodiscard() {
    must(1); // expect: CXX-NODISCARD
    (void)must(2);
    int k = must(3);
    (void)k;
}
void move_const(const std::string s) {
    std::string t = std::move(s); // expect: CXX-MOVE-CONST
    (void)t;
}
void new_delete() {
    int *a = new int[3];
    delete a; // expect: MEM-NEW-DELETE
    int *b = new int[3];
    delete[] b;
}
void nx() noexcept {
    throw 1; // expect: CXX-THROW-NOEXCEPT
}
void tnew() {
    throw new int(1); // expect: CXX-THROW-NEW
}
)";

}  // namespace

TEST_CASE("astlint: ported regex classes on C (planted + guards, both front ends)") {
    check_marks("ported.c", kPortedC);
}

TEST_CASE("astlint: ported regex classes on C++ (planted + guards, both front ends)") {
    check_marks("ported.cpp", kPortedCxx);
}

TEST_CASE("astlint: project headers, nested enums and non-dependent template code are checked") {
    const std::string hdr = R"(#pragma once
struct Shape {
    enum Kind { Circle, Square, Tri };
    int area(Kind k) {
        switch (k) { // expect: INT-ENUM-HOLE
        case Circle: return 1;
        case Square: return 2;
        }
        return 0;
    }
};
namespace geo {
enum class Axis { X, Y, Z };
inline int pick(Axis a) {
    switch (a) { // expect: INT-ENUM-HOLE
    case Axis::X: return 0;
    case Axis::Y: return 1;
    }
    return 2;
}
}  // namespace geo
template <class T> struct Box {
    T v;
    int count() {
        int n;
        n = 3; // expect: CTRL-DEAD-STORE
        return 0;
    }
    int dep(T a) {
        T w;
        w = a;
        return 0;
    }
};
)";
    const std::string main_cpp = "#include \"shapes.h\"\n"
                                 "int use() { Shape s; Box<int> b; return s.area(Shape::Circle) + "
                                 "geo::pick(geo::Axis::X) + b.count() + b.dep(1); }\n";
    const std::string other_cpp = "#include \"shapes.h\"\nint other() { Box<long> b; return b.count(); }\n";
    // Rows in the header, on both front ends, exactly as marked.
    check_marks("main.cpp", main_cpp, {"shapes.h", hdr, "other.cpp", other_cpp, "orphan.h", "int orphan(void);\n"},
                "shapes.h");
    auto clang = find_clang();
    Tree t;
    std::vector<fs::path> files{t.put("main.cpp", main_cpp), t.put("other.cpp", other_cpp), t.put("shapes.h", hdr),
                                t.put("orphan.h", "int orphan(void);\n")};
    for (auto& [fe, b] : front_ends(clang)) {
        INFO("front end: " << fe);
        auto out = lint_with(t, files, clang, b);
        int hdr_rows = 0, notes = 0;
        for (auto& f : out) {
            if (f.extra.contains("engine") && f.file == "shapes.h") ++hdr_rows;
            if (prism::astlint::is_layer_row(f)) {
                ++notes;
                // Law 7: the header nothing includes is said to be regex-only.
                CHECK(f.file.empty());
                CHECK(f.message.find("1 of 2 header file(s)") != std::string::npos);
            }
        }
        CHECK(hdr_rows == 3);  // once, although two units include the header
        CHECK(notes == 1);
    }
}

TEST_CASE("astlint: an AST row of a ported class supersedes the regex row") {
    auto clang = find_clang();
    Tree t;
    auto p = t.put("sup.c", "#include <stdlib.h>\n"
                            "int f(int k) {\n"
                            "    int r = 0;\n"
                            "    switch (k) {\n"
                            "    case 1:\n"
                            "        r = 1;\n"
                            "    case 2:\n"
                            "        r = 2;\n"
                            "        break;\n"
                            "    }\n"
                            "    malloc(4);\n"
                            "    return r;\n"
                            "}\n");
    auto regex_only = lint_with(t, {p}, std::nullopt, Backend::Process);
    std::set<std::pair<int, std::string>> regex_rows;
    for (auto& f : regex_only)
        if (f.status == prism::laws::FAILED) regex_rows.insert({f.line.value_or(0), f.cls});
    CHECK(regex_rows.contains({6, "CTRL-FALLTHROUGH"}));
    CHECK(regex_rows.contains({11, "API-IGNORED-ERROR"}));
    for (auto& [fe, b] : front_ends(clang)) {
        INFO("front end: " << fe);
        auto out = lint_with(t, {p}, clang, b);
        for (auto& row : regex_rows) {
            int n = 0;
            for (auto& f : out) {
                if (f.status != prism::laws::FAILED || f.line != row.first || f.cls != row.second) continue;
                ++n;
                CHECK(f.extra.at("engine") == "clang-ast");
                CHECK(f.extra.at("supersedes") == "regex");
            }
            CHECK_MESSAGE(n == 1, "line " << row.first << " " << row.second);
        }
    }
}
