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

bool clang_ast_timed_out(const std::vector<prism::Finding>& findings) {
    for (auto& f : findings) {
        if (f.status != prism::laws::NOTRUN) continue;
        if (f.message.find("timed out") != std::string::npos) return true;
    }
    return false;
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
    memset(p, sizeof *p, 0);
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
        if (clang_ast_timed_out(out)) {
            MESSAGE("clang timed out on this runner: C++ stdlib unit skipped");
            continue;
        }
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
        if (clang_ast_timed_out(out)) {
            MESSAGE("clang timed out on this runner: " << name << " skipped");
            return;
        }
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
#include <strings.h>
#include <sys/stat.h>
#ifndef _WIN32
int mkstemp(char *);
int mkstemps(char *, int);
#endif
#ifndef _WIN32
int setuid(int);
int seteuid(int);
int setgid(int);
#endif
int chmod(const char *path, unsigned mode);
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
void gets_bad(char *buf) { gets(buf); } // expect: API-GETS
void gets_ok(char *buf) { fgets(buf, 16, stdin); }
void strtok_bad(char *s) { (void)strtok(s, " "); } // expect: API-STRTOK-REENTRANT
void strtok_ok(char *s) { if (s && *s) puts(s); }
void tmpnam_bad(char *buf) { tmpnam(buf); } // expect: API-TMPNAM
void tempnam_bad(void) { (void)tempnam("/tmp", "pfx"); } // expect: API-TMPNAM
void tmpnam_ok(void) {
#ifndef _WIN32
    char t[] = "/tmp/xXXXXXX";
    (void)mkstemp(t);
#endif
}
void mktemp_bad(void) { char t[] = "/tmp/xXXXXXX"; mktemp(t); } // expect: API-MKTEMP
void mktemp_ok(void) {
#ifndef _WIN32
    char t[] = "/tmp/xXXXXXX";
    (void)mkstemp(t);
#endif
}
int umask_bad(void) { umask(0); return 0; } // expect: API-UMASK
int umask_ok(void) { umask(077); return 0; }
#ifndef _WIN32
void mkstemp_bad(void) { mkstemp("/tmp/xXXXXXX"); } // expect: API-MKSTEMP
void mkstemps_bad(void) { mkstemps("/tmp/xXXXXXX", 0); } // expect: API-MKSTEMP
void mkstemp_ok(void) {
    char t[] = "/tmp/xXXXXXX";
    (void)mkstemp(t);
}
#endif
void chmod_world_bad(void) { chmod("/tmp/x", 0777); } // expect: API-CHMOD-WORLD
void chmod_world_ok(void) { chmod("/tmp/x", 0755); }
#ifndef _WIN32
void setuid_bad(void) { setuid(0); } // expect: API-SETUID
void setuid_ok(void) {
    if (setuid(0) != 0) return;
}
#endif
void scanf_bad(int *p) { scanf("%d", p); } // expect: API-SCANF-UNCHECKED
void scanf_ok(int *p) {
    if (scanf("%d", p) != 1) return;
}
void overlap(char *s) { strcpy(s, s); } // expect: MEM-OVERLAP, STR-NULL-ARG
void bcopy_self(char *p) { bcopy(p, p, 4); } // expect: MEM-BCOPY
char *rself(char *p) {
    p = realloc(p, 10); // expect: MEM-REALLOC-SELF
    return p;
}
char *rsafe(char *p) {
    char *q = realloc(p, 10);
    if (!q) return p;
    return q;
}
void unbounded_into_local(void) {
    char buf[8];
    strcpy(buf, "hi"); // expect: STR-UNBOUNDED-COPY
}
void fixed_copy(char *out) {
    char small[4];
    strcpy(small, "overflow"); // expect: STR-OFF-BY-ONE, STR-UNBOUNDED-COPY
    strcpy(out, "overflow");
    puts(small);
}
int bool_bit(int a, int b) { return (a > 0) & (b > 0) ? 1 : !a & 4; } // expect: INT-BOOL-AS-BIT
int taut_bounds(int idx) {
    if (idx >= 0 || idx < 100) return 1; // expect: INT-TAUTOLOGY
    return 0;
}
int taut_ok(int i, int n) {
    if (i >= 0 && i < n) return 1;
    return 0;
}
struct copy_buf { char data[8]; unsigned data_max; };
void copy_len_bad(struct copy_buf *b, const char *src, unsigned len) {
    memcpy(b->data, src, len); // expect: MEM-COPY-LEN
}
void copy_len_ok(struct copy_buf *b, const char *src, unsigned len) {
    if (len > b->data_max) return;
    memcpy(b->data, src, len);
}
void missing_nul_plant(void) {
    char buf[8];
    memcpy(buf, "hello", 5); // expect: STR-MISSING-NUL
    char ok[8];
    memcpy(ok, "hello", 6);
    char ok2[8];
    memcpy(ok2, "hello", sizeof("hello"));
}
void memset_swap_plant(char *p) {
    memset(p, sizeof *p, 0); // expect: MEM-MEMSET-SWAP
    memset(p, 0, 4);
}
char *getenv(const char *);
char *strdup(const char *);
#ifndef SIGINT
#define SIGINT 2
#endif
#ifndef SIG_DFL
#define SIG_DFL ((void (*)(int))0)
#endif
#ifndef SIG_IGN
#define SIG_IGN ((void (*)(int))1)
#endif
int signal(int, void (*)(int));
void prism_sig_handler(int);
void signal_bad(void) { signal(SIGINT, prism_sig_handler); } // expect: API-SIGNAL
void signal_ok(void) { signal(SIGINT, SIG_IGN); }
void system_bad_cmd(void) {
    char *cmd = getenv("X");
    system(cmd); // expect: API-SYSTEM
}
void system_ok_literal(void) { system("true"); }
void getenv_null_plant(void) {
    char *s = getenv("X");
    (void)s[0]; // expect: API-GETENV-NULL
}
void strdup_null_plant(void) {
    char *s = strdup("x");
    (void)s[0]; // expect: API-STRDUP-NULL
}
void unchecked_malloc_plant(void) {
    char *p = malloc(4);
    p[0] = 1; // expect: PTR-UNCHECKED-ALLOC
}
int missing_ret_bad(void) {
    int x = 1;
    x = 2; // expect: CTRL-DEAD-STORE
} // expect: CTRL-MISSING-RETURN
int missing_ret_ok(void) { return 1; }
void noreturn_bad(void) { exit(1); } // expect: FUNC-NORETURN
_Noreturn void noreturn_ok(void) { exit(1); }
void wrap_bad(unsigned n) {
    (void)malloc(n * sizeof(int)); // expect: INT-WRAP-ALLOC
}
int atoi_idx_bad(const char *s) {
    char a[4];
    int i = atoi(s);
    return a[i]; // expect: INT-ATOI, TRUST-UNVALIDATED-INPUT
}
int masked_bad(int f) {
    int d;
    switch (f & 3) { // expect: UNINIT-SWITCH
    case 0:
        d = 0;
        break;
    case 1:
        d = 1;
        break;
    }
    return d;
}
#include <pthread.h>
#include <threads.h>
// pthread_mutex_t / mtx_t and their lock, unlock and init
// functions come from the system headers above: a local typedef
// clashes with glibc's union pthread_mutex_t, which stdlib.h
// already pulls in on some hosts.
//
//
void prism_mtx_lock(int *m);
void prism_mtx_unlock(int *m);
void double_unlock_bad(int *m) {
    prism_mtx_lock(m);
    prism_mtx_unlock(m);
    prism_mtx_unlock(m); // expect: LOCK-DOUBLE-UNLOCK
}
void double_unlock_ok(int *m) {
    prism_mtx_lock(m);
    prism_mtx_unlock(m);
    prism_mtx_lock(m);
    prism_mtx_unlock(m);
}
void double_lock_bad(int *m) {
    prism_mtx_lock(m);
    prism_mtx_lock(m); // expect: LOCK-DOUBLE-LOCK
    prism_mtx_unlock(m);
}
int lock_imbalance_bad(int *m, int x) {
    prism_mtx_lock(m);
    if (x < 0) return -1; // expect: LOCK-IMBALANCE
    prism_mtx_unlock(m);
    return 0;
}
void lock_init_bad(void) {
    pthread_mutex_t m;
    pthread_mutex_lock(&m); // expect: LOCK-MISSING-INIT
}
void lock_init_ok(void) {
    pthread_mutex_t m;
    pthread_mutex_init(&m, 0);
    pthread_mutex_lock(&m);
}
void order_ab(int *la, int *lb) {
    prism_mtx_lock(la); // expect: LOCK-ORDER
    prism_mtx_lock(lb);
    prism_mtx_unlock(lb);
    prism_mtx_unlock(la);
}
void order_ab_same(int *la, int *lb) {
    prism_mtx_lock(la); // expect: LOCK-ORDER
    prism_mtx_lock(lb);
    prism_mtx_unlock(lb);
    prism_mtx_unlock(la);
}
void order_ab_only(int *la, int *lc) {
    prism_mtx_lock(la);
    prism_mtx_lock(lc);
    prism_mtx_unlock(lc);
    prism_mtx_unlock(la);
}
void order_ba(int *la, int *lb) {
    prism_mtx_lock(lb); // expect: LOCK-ORDER
    prism_mtx_lock(la);
    prism_mtx_unlock(la);
    prism_mtx_unlock(lb);
}
struct ast_item { char *name; };
size_t str_null_member_plant(struct ast_item *it) {
    return strlen(it->name); // expect: STR-NULL-MEMBER
}
struct ast_link { struct ast_link *next; int v; };
void ptr_chain_null_plant(struct ast_link *a, int v) {
    a->next->v = v; // expect: PTR-CHAIN-NULL
}
size_t str_null_arg_plant(char *s) {
    return strlen(s); // expect: STR-NULL-ARG
}
#define M_NOWAIT 0x02
// Hosted malloc takes one argument: the flag rides in the size expression.
void nowait_bad(unsigned n) {
    char *p = malloc(n | M_NOWAIT);
    p[0] = 1; // expect: MEM-NOWAIT
}
void nowait_ok(unsigned n) {
    char *p = malloc(n | M_NOWAIT);
    if (!p) return;
    p[0] = 1;
}
void *alloca(unsigned long);
void alloca_bad(int n) {
    char *p = alloca(n); // expect: MEM-ALLOCA
    (void)p;
}
void alloca_ok(void) {
    char *p = alloca(16);
    (void)p;
}
struct cap_buf { char *data; unsigned cap; };
int cap_first_bad(struct cap_buf *b) {
    char *n;
    b->cap *= 2; // expect: MEM-CAPACITY-FIRST
    n = realloc(b->data, b->cap);
    if (n == NULL)
        return -1;
    b->data = n;
    return 0;
}
int cap_first_restored(struct cap_buf *b) {
    char *n;
    b->cap *= 2;
    n = realloc(b->data, b->cap);
    if (n == NULL) {
        b->cap /= 2;
        return -1;
    }
    b->data = n;
    return 0;
}
int cap_after_ok(struct cap_buf *b) {
    char *n = realloc(b->data, b->cap * 2);
    if (!n)
        return -1;
    b->data = n;
    b->cap *= 2;
    return 0;
}
void sprintf_bad(const char *s) {
    char b[8];
    sprintf(b, "%s", s); // expect: STR-SPRINTF
}
void sprintf_ok(const char *s) {
    char b[8];
    snprintf(b, sizeof b, "%s", s);
}
// snprintf comes from <stdio.h>.
void snprintf_bad(void) {
    char b[4];
    snprintf(b, 64, "%s", "x"); // expect: STR-SNPRINTF
}
void snprintf_ok(void) {
    char b[4];
    snprintf(b, sizeof b, "x");
}
typedef int wchar_t;
wchar_t *wcscpy(wchar_t *dst, const wchar_t *src);
void wcs_bad(void) {
    wchar_t b[4];
    wcscpy(b, L"abc"); // expect: STR-WCSCPY
}
int rand(void);
void crypto_key_bad(char *key) {
    int i;
    for (i = 0; i < 8; i++)
        key[i] = rand(); // expect: CRYPTO-MISUSE, INT-TRUNC
}
void srand(unsigned);
typedef long time_t;
time_t time(time_t *);
void srand_bad(void) {
    srand((unsigned)time(0)); // expect: CRYPTO-SRAND
}
void srand_ok(int seed) {
    srand((unsigned)seed);
}
int realloc_zero_bad(void) {
    char *q;
    char *p = malloc(4);
    q = realloc(p, 0);
    p[0] = 1; // expect: MEM-REALLOC-ZERO, PTR-UNCHECKED-ALLOC
    (void)q;
    return 0;
}
void realloc_zero_ok(void) {
    char *p = malloc(4);
    char *q = realloc(p, 8);
    (void)p;
    (void)q;
}
int arith_bad(int *p, int n) {
    return *(p + n); // expect: MEM-PTR-ARITH
}
int arith_ok(int *p, int n) {
    if (!p)
        return 0;
    if (n < 0)
        return 0;
    return p[n];
}
int arith_one(int *p, int n) {
    if (!p)
        return 0;
    return *(p + n); // expect: MEM-PTR-ARITH
}
void vla_bad(int n) {
    int a[n]; // expect: MEM-VLA-SIZE
    (void)a;
}
void vla_ok(void) {
    int a[4];
    (void)a;
}
void vla_caps_ok(void) {
    int a[BUFSIZ];
    (void)a;
}
void vla_sizeof_ok(void) {
    int b[sizeof(int)];
    (void)b;
}
struct ast_flex {
    int n;
    char data[];
};
struct ast_plain {
    int x;
};
void flex_bad(void) {
    struct ast_flex *p = malloc(sizeof(struct ast_flex)); // expect: MEM-FLEX-ARRAY
    (void)p;
}
void flex_star_bad(void) {
    struct ast_flex *p;
    p = malloc(sizeof(*p)); // expect: MEM-FLEX-ARRAY
    (void)p;
}
void flex_ok(int n) {
    struct ast_flex *p = malloc(sizeof(struct ast_flex) + (unsigned)n);
    (void)p;
}
void flex_plain_ok(void) {
    struct ast_plain *p = malloc(sizeof(struct ast_plain));
    (void)p;
}
struct pad_s {
    char a;
    int b;
};
void pad_bad(struct pad_s *out) {
    struct pad_s s;
    s.a = 1;
    memcpy(out, &s, sizeof(s)); // expect: INFOLEAK-PAD
}
void pad_ok(struct pad_s *out) {
    struct pad_s s;
    memset(&s, 0, sizeof(s));
    s.a = 1;
    memcpy(out, &s, sizeof(s));
}
int onesided_bad(int i, int n) {
    int buf[8];
    if (i > n) // expect: MEM-ONESIDED-INDEX
        return 0;
    return buf[i];
}
int onesided_floor_ok(int i, int n) {
    int buf[8];
    if (i < 0)
        return 0;
    if (i > n)
        return 0;
    return buf[i];
}
int onesided_unsigned_ok(unsigned i, unsigned n) {
    int buf[8];
    if (i > n)
        return 0;
    return buf[i];
}
int mem_leak_bad(int err) {
    char *p = malloc(64);
    if (err)
        return -1; // expect: MEM-LEAK
    free(p);
    return 0;
}
int mem_leak_ok(int err) {
    char *p = malloc(64);
    if (err) {
        free(p);
        return -1;
    }
    free(p);
    return 0;
}
int mem_guard_ok(void) {
    char *p = malloc(64);
    if (!p)
        return -1;
    free(p);
    return 0;
}
int fd_leak_bad(const char *path, int err) {
    FILE *fp = fopen(path, "r");
    if (err)
        return -1; // expect: RES-FD-LEAK
    fclose(fp);
    return 0;
}
int fd_leak_ok(const char *path, int err) {
    FILE *fp = fopen(path, "r");
    if (err) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}
int fd_guard_ok(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;
    fclose(fp);
    return 0;
}
void popen_bad(void) {
    FILE *f = popen("true", "r");
    (void)f;
    return; // expect: API-POPEN
}
void popen_ok(void) {
    FILE *f = popen("true", "r");
    if (f)
        pclose(f);
}
void chroot_bad(void) {
    chroot("/jail"); // expect: API-CHROOT
}
void chroot_ok(void) {
    chroot("/jail");
    chdir("/");
}
int toctou_bad(const char *path) {
    if (access(path, 0) != 0)
        return -1;
    return open(path, 0); // expect: CONC-TOCTOU
}
int toctou_ok(const char *path) {
    return open(path, 0);
}
int unval_bad(const char *s) {
    int a[4];
    int i = atoi(s);
    return a[i]; // expect: INT-ATOI, TRUST-UNVALIDATED-INPUT
}
int unval_ok(const char *s) {
    int a[4];
    int i = atoi(s);
    if (i < 0)
        return 0;
    if (i >= 4)
        return 0;
    return a[i];
}
int unval_div_bad(const char *s) {
    int x = 10;
    int i = atoi(s);
    return x / i; // expect: TRUST-UNVALIDATED-INPUT
}
int clz_zero_bad(void) {
    return __builtin_clz(0); // expect: INT-CLZ-ZERO
}
int clz_zero_ok(unsigned n) {
    if (!n)
        return 0;
    return __builtin_clz(n);
}
int clz_cast_ok(void) {
    return __builtin_clz((unsigned)0);
}
int g_counter;
void atom_bad(void) {
    g_counter = g_counter + 1; // expect: CONC-ATOMICITY
}
void atom_ok(void) {
    prism_mtx_lock(0);
    g_counter = g_counter + 1;
    prism_mtx_unlock(0);
}
void atom_inc_bad(void) {
    ++g_counter; // expect: CONC-ATOMICITY
}
void atom_local_ok(void) {
    int g_counter = 0;
    g_counter++;
    (void)g_counter;
}
int toctou_other_path_ok(const char *path, const char *other) {
    if (access(path, 0) != 0)
        return -1;
    return open(other, 0);
}
int fd_assign_guard_ok(const char *path) {
    FILE *fp;
    if ((fp = fopen(path, "r")) == NULL)
        return -1;
    fclose(fp);
    return 0;
}
int fd_open_bad(const char *path, int err) {
    int fd = open(path, 0);
    if (fd < 0)
        return -1;
    if (err)
        return 1; // expect: RES-FD-LEAK
    close(fd);
    return 0;
}
enum { VLA_K = 4 };
#define VLA_N 8
void vla_const_ok(void) {
    int a[VLA_K];
    char b[VLA_N * 2];
    (void)a;
    (void)b;
}
void vla_2d_bad(int n) {
    int m[n][4]; // expect: MEM-VLA-SIZE
    (void)m;
}
int sib_read(int sib_idx) {
    if (sib_idx < 0)
        return -1;
    return sib_idx;
}
int sib_write_bad(int sib_idx) {
    int buf[4];
    buf[sib_idx] = 1; // expect: CTRL-SIBLING-ASYMMETRY
    return buf[0];
}
int sib_write_ok(int sib_idx) {
    int buf[4];
    if (sib_idx > 3)
        return 0;
    buf[sib_idx] = 1;
    return buf[0];
}
long sib_other_type(long sib_idx) {
    long buf[4];
    buf[sib_idx] = 1;
    return buf[0];
}
#include <pwd.h>
#include <sys/mman.h>
#include <sys/socket.h>
int sock_bad(void) {
    int s;
    s = socket(2, 1, 0); // expect: API-SOCKET
    return s + 1;
}
int sock_ok(void) {
    int s = socket(2, 1, 0);
    if (s < 0)
        return -1;
    return s + 1;
}
int sock_cond_ok(void) {
    int s;
    if ((s = socket(2, 1, 0)) < 0)
        return -1;
    return s + 1;
}
const char *pw_bad(void) {
    struct passwd *pw = getpwuid(0); // expect: API-GETPWUID
    return pw->pw_name;
}
const char *pw_ok(void) {
    struct passwd *pw = getpwuid(0);
    if (!pw)
        return 0;
    return pw->pw_name;
}
int map_bad(int fd) {
    char *m = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0); // expect: API-MMAP
    return m[0];
}
int map_ok(int fd) {
    char *m = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED)
        return -1;
    return m[0];
}
int map_sizeof_ok(int fd) {
    char *m = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    return (int)sizeof(*m);
}
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
#include <algorithm>
#include <bit>
#include <numeric>
int tok_gcd_bad(int x, int y) {
    int a[8] = {};
    int g = std::gcd(x, y);
    return a[g]; // expect: CXX-GCD
}
int tok_gcd_ok(int x, int y) {
    int a[8] = {};
    int g = std::gcd(x, y);
    if (g >= 8)
        return 0;
    return a[g];
}
int tok_clamp_direct(int x) {
    std::vector<int> v(4);
    return v[std::clamp(x, 0, 9)]; // expect: CXX-CLAMP, CXX-VECTOR-INDEX
}
int tok_bitwidth_bad(unsigned x) {
    int a[8] = {};
    int w = std::bit_width(x);
    return a[w]; // expect: CXX-BIT-WIDTH
}
int tok_rotl_unused(unsigned x) {
    int a[8] = {};
    unsigned r = std::rotl(x, 3);
    return a[0] + static_cast<int>(r);
}
#include <list>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>
int cont_set_bad(const std::set<int> &s) {
    return *s.find(3); // expect: CXX-SET-FIND
}
int cont_set_ok(const std::set<int> &s) {
    auto it = s.find(3);
    return it != s.end() ? *it : 0;
}
int cont_map_bad(const std::map<int, int> &m) {
    return m.at(1); // expect: CXX-MAP-AT
}
int cont_umap_ok(const std::unordered_map<int, int> &m) {
    return m.contains(1) ? m.at(1) : 0;
}
int cont_list_bad() {
    std::list<int> l;
    return l.front(); // expect: CXX-LIST-FRONT
}
int cont_pq_ok() {
    std::priority_queue<int> q;
    if (q.empty())
        return 0;
    return q.top();
}
#include <ranges>
int view_take_bad(const std::vector<int> &v, int i) {
    auto t = v | std::views::take(2);
    (void)t;
    return v[i]; // expect: CXX-TAKE, CXX-VECTOR-INDEX
}
int view_take_ok(const std::vector<int> &v, int i) {
    auto t = v | std::views::take(2);
    (void)t;
    if (i < 0 || i >= 2)
        return 0;
    return v[i]; // expect: CXX-VECTOR-INDEX
}
int view_none(const std::vector<int> &v, int i) {
    return v[i]; // expect: CXX-VECTOR-INDEX
}
#include <deque>
#include <stack>
int deque_bad(const std::deque<int> &d, unsigned i) {
    return d[i]; // expect: CXX-DEQUE-INDEX
}
int deque_ok(const std::deque<int> &d, unsigned i) {
    if (i < d.size())
        return d[i];
    return 0;
}
int stack_bad() {
    std::stack<int> st;
    return st.top(); // expect: CXX-STACK-TOP
}
#include <functional>
#include <future>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <exception>
#include <stdexcept>
#include <cstdlib>
#include <memory>
#include <initializer_list>
int bind_target(int a, int b) { return a + b; }
int bind_bad() {
    auto f = std::bind(bind_target, 1, std::placeholders::_1); // expect: CXX-STD-BIND
    return f(2);
}
int bind_ok() {
    auto f = [](int x) { return bind_target(1, x); };
    return f(2);
}
std::condition_variable g_cv;
std::mutex g_m;
void notify_bad() {
    std::unique_lock<std::mutex> lk(g_m);
    std::notify_all_at_thread_exit(g_cv, std::move(lk)); // expect: CXX-NOTIFY-THREAD-EXIT
}
void notify_ok(bool &ready) {
    std::unique_lock<std::mutex> lk(g_m);
    std::notify_all_at_thread_exit(g_cv, std::move(lk));
    std::unique_lock<std::mutex> lk2(g_m);
    g_cv.wait(lk2, [&] { return ready; });
}
void nested_bad() {
    std::throw_with_nested(std::runtime_error("x")); // expect: CXX-NESTED-EXCEPTION
}
void nested_ok() {
    try {
        throw std::runtime_error("a");
    } catch (...) {
        std::throw_with_nested(std::runtime_error("b"));
    }
}
void qe_handler() {}
void quick_bad() {
    std::quick_exit(1); // expect: CXX-QUICK-EXIT
}
void quick_ok() {
    std::at_quick_exit(qe_handler);
    std::quick_exit(1);
}
int async_work() { return 1; }
void async_bad() {
    std::async(std::launch::async, async_work); // expect: CXX-STD-ASYNC
}
int async_ok() {
    auto fut = std::async(std::launch::async, async_work);
    return fut.get();
}
const int *ilist_bad() {
    return std::initializer_list<int>{1, 2, 3}.begin(); // expect: CXX-INIT-LIST-DANGLE
}
int ilist_ok(std::initializer_list<int> il) {
    return *il.begin();
}
template <class T> struct task {
    T v;
};
template <class T> T sync_wait(task<T> t) { return t.v; }
int task_bad() {
    task<int> t = {1}; // expect: CXX-TASK
    return t.v;
}
int task_ok() {
    task<int> t = {1};
    return sync_wait(t);
}
int auto_ptr_bad() {
    std::auto_ptr<int> p(new int(1)); // expect: CXX-AUTO-PTR
    return *p;
}
#include <optional>
#include <variant>
#include <vector>
#include <string>
void may_throw();
void catch_all_bad() {
    try {
        may_throw();
    } catch (...) { // expect: CXX-CATCH-ALL
    }
}
void catch_all_ok() {
    try {
        may_throw();
    } catch (...) {
        throw;
    }
}
void thread_body() {}
void thread_bad() {
    std::thread th(thread_body); // expect: CXX-STD-THREAD
}
void thread_ok() {
    std::thread th(thread_body);
    th.join();
}
std::optional<int> maybe_int();
int optional_bad() {
    std::optional<int> o = maybe_int();
    return *o; // expect: CXX-OPTIONAL-NULL
}
int optional_ok() {
    std::optional<int> o = maybe_int();
    if (o.has_value())
        return *o;
    return 0;
}
int variant_bad(std::variant<int, float> v) {
    return std::get<int>(v); // expect: CXX-VARIANT-GET
}
int variant_ok(std::variant<int, float> v) {
    if (std::holds_alternative<int>(v))
        return std::get<int>(v);
    return 0;
}
int vec_index_bad(const std::vector<int> &v, unsigned k) {
    return v[k]; // expect: CXX-VECTOR-INDEX
}
int vec_index_ok(const std::vector<int> &v, unsigned k) {
    if (k < v.size())
        return v[k];
    return 0;
}
struct DynBase {
    virtual ~DynBase() = default;
};
struct DynDerived : DynBase {
    int x = 0;
};
int dyn_bad(DynBase *b) {
    DynDerived *d = dynamic_cast<DynDerived *>(b);
    return d->x; // expect: CXX-DYNAMIC-CAST-NULL
}
int dyn_ok(DynBase *b) {
    DynDerived *d = dynamic_cast<DynDerived *>(b);
    if (d)
        return d->x;
    return 0;
}
#include <bit>
#include <cstdint>
int unique_reset_bad() {
    std::unique_ptr<int> up(new int(1));
    int *raw = up.get();
    up.reset();
    return *raw; // expect: CXX-UNIQUE-RESET
}
int unique_reset_ok() {
    std::unique_ptr<int> up(new int(1));
    int *raw = up.get();
    int v = *raw;
    up.reset();
    return v;
}
int shared_get_bad() {
    std::shared_ptr<int> sp = std::make_shared<int>(1);
    int *raw = sp.get();
    sp = nullptr;
    return *raw; // expect: CXX-SHARED-PTR-GET
}
char string_data_bad() {
    std::string s = "abc";
    const char *raw = s.c_str();
    s += "defghijklmnopqrstuvwxyz";
    return raw[0]; // expect: CXX-STRING-DATA
}
char string_data_ok() {
    std::string s = "abc";
    const char *raw = s.c_str();
    char c = raw[0];
    s.clear();
    return c;
}
int unique_release_bad() {
    std::unique_ptr<int> up(new int(1));
    int *p = up.release();
    delete p;
    return up.get() != nullptr; // expect: CXX-UNIQUE-RELEASE
}
int optional_value_bad() {
    std::optional<int> o = maybe_int();
    return o.value(); // expect: CXX-OPTIONAL-VALUE
}
int optional_value_ok() {
    std::optional<int> o = maybe_int();
    return o.has_value() ? o.value() : 0;
}
void fwd_ref_bad(std::vector<std::string> &out, std::string &&s) {
    out.push_back(s); // expect: CXX-FORWARDING-REF
}
void fwd_ref_ok(std::vector<std::string> &out, std::string &&s) {
    out.push_back(std::move(s));
}
std::uintptr_t bit_cast_ok(char *p) {
    return std::bit_cast<std::uintptr_t>(p);
}
float *bit_cast_bad(int *p) {
    return std::bit_cast<float *>(p); // expect: CXX-BIT-CAST
}
#include <any>
#include <latch>
#include <shared_mutex>
#include <stop_token>
void jthread_bad() {
    std::jthread jt(thread_body); // expect: CXX-STD-JTHREAD
}
void jthread_ok() {
    std::jthread jt(thread_body);
    jt.request_stop();
}
int smx_bad(int &v) {
    std::shared_mutex smx;
    smx.lock(); // expect: CXX-SHARED-MUTEX
    return v;
}
int smx_ok(int &v) {
    std::shared_mutex smx;
    smx.lock();
    int r = v;
    smx.unlock();
    return r;
}
void latch_bad() {
    std::latch lt(1);
    lt.wait(); // expect: CXX-LATCH
}
void latch_ok() {
    std::latch lt(1);
    lt.count_down();
    lt.wait();
}
void cv_bad() {
    std::unique_lock<std::mutex> lk(g_m);
    g_cv.wait(lk); // expect: CXX-CONDITION-WAIT
}
void cv_ok(bool &ready) {
    std::unique_lock<std::mutex> lk(g_m);
    g_cv.wait(lk, [&] { return ready; });
}
int fn_null_bad(std::function<int()> make) {
    std::function<int()> f = make;
    return f(); // expect: CXX-STD-FUNCTION-NULL
}
int fn_null_ok(std::function<int()> make) {
    std::function<int()> f = make;
    if (f)
        return f();
    return 0;
}
int future_bad() {
    std::future<int> fu;
    return fu.get(); // expect: CXX-FUTURE-GET
}
int future_ok() {
    std::future<int> fu = std::async(std::launch::async, async_work);
    return fu.get();
}
int any_bad(const std::any &a) {
    return std::any_cast<int>(a); // expect: CXX-ANY-CAST
}
int any_ok(const std::any &a) {
    const int *p = std::any_cast<int>(&a);
    return p ? *p : 0;
}
#include <semaphore>
#include <syncstream>
#include <iostream>
void sem_bad() {
    std::counting_semaphore<4> sem(2);
    sem.acquire(); // expect: CXX-SEMAPHORE
}
void sem_ok() {
    std::counting_semaphore<4> sem(2);
    sem.acquire();
    sem.release();
}
void binsem_bad() {
    std::binary_semaphore bs(0);
    bs.acquire(); // expect: CXX-BINARY-SEMAPHORE
}
void binsem_ok() {
    std::binary_semaphore bs(0);
    if (bs.try_acquire())
        bs.release();
}
void syncbuf_bad() {
    std::syncbuf sb(std::cout.rdbuf()); // expect: CXX-SYNCBUF
}
void syncbuf_ok() {
    std::syncbuf sb(std::cout.rdbuf());
    sb.emit();
}
int ptask_bad() {
    std::packaged_task<int()> pt(async_work);
    pt(); // expect: CXX-PACKAGED-TASK
    return 0;
}
int ptask_ok() {
    std::packaged_task<int()> pt(async_work);
    if (pt.valid())
        pt();
    return 0;
}
int promise_bad() {
    std::promise<int> pr;
    std::future<int> fu = pr.get_future();
    return fu.get(); // expect: CXX-PROMISE
}
int promise_ok() {
    std::promise<int> pr;
    std::future<int> fu = pr.get_future();
    pr.set_value(1);
    return fu.get();
}
int weak_bad(std::weak_ptr<int> w) {
    std::shared_ptr<int> sp = w.lock(); // expect: CXX-WEAK-PTR
    return *sp;
}
int weak_ok(std::weak_ptr<int> w) {
    std::shared_ptr<int> sp = w.lock();
    if (sp)
        return *sp;
    return 0;
}
void stop_bad(std::stop_token st, int &n) {
    while (true) { // expect: CXX-STOP-TOKEN
        ++n;
    }
}
void stop_ok(std::stop_token st, int &n) {
    while (!st.stop_requested()) {
        ++n;
    }
}
#include <regex>
#include <random>
#include <chrono>
#include <stacktrace>
#include <iterator>
void vol_bad() {
    volatile int vreg = 0;
    *(int *)&vreg = 5; // expect: CXX-VOLATILE-CAST
}
void vol_ok() {
    volatile int vreg = 0;
    vreg = 5;
}
void cc_bad() {
    const int ci = 1;
    *const_cast<int *>(&ci) = 2; // expect: CXX-CONST-CAST
}
void cc_ok(int &x) {
    *const_cast<int *>(&x) = 2;
}
float pun_bad(int *ip) {
    return *reinterpret_cast<float *>(ip); // expect: CXX-REINTERPRET
}
unsigned char pun_ok(int *ip) {
    return *reinterpret_cast<unsigned char *>(ip);
}
bool rx_bad(const std::string &pat, const std::string &s) {
    std::regex re(pat); // expect: CXX-REGEX
    return std::regex_match(s, re);
}
bool rx_ok(const std::string &s) {
    std::regex re("[a-z]+");
    return std::regex_match(s, re);
}
unsigned seed_bad() {
    auto t = std::chrono::system_clock::now().time_since_epoch().count(); // expect: CXX-CHRONO-SEED
    std::mt19937 g(static_cast<unsigned>(t));
    return g();
}
unsigned seed_ok() {
    std::random_device rd;
    std::mt19937 g(rd());
    return g();
}
int st_bad() {
    auto st = std::stacktrace::current();
    return static_cast<int>(st[0].source_line()); // expect: CXX-STACKTRACE
}
int st_ok() {
    auto st = std::stacktrace::current();
    if (st.empty())
        return 0;
    return static_cast<int>(st[0].source_line());
}
int counted_bad(std::vector<int> &v, int n) {
    std::counted_iterator it(v.begin(), n); // expect: CXX-COUNTED-ITERATOR
    return *it;
}
int counted_ok(std::vector<int> &v, int n) {
    if (n > static_cast<int>(v.size()))
        return 0;
    std::counted_iterator it(v.begin(), n);
    return *it;
}
#include <array>
#include <tuple>
#include <memory>
#include <atomic>
#include <utility>
int apply_bad(std::function<int(int)> f, std::tuple<int> t) {
    return std::apply(f, t); // expect: CXX-APPLY
}
int apply_ok(std::function<int(int)> f, std::tuple<int> t) {
    if (!f)
        return 0;
    return std::apply(f, t);
}
int arr_bad(int i) {
    std::array<int, 4> a{};
    return a[i]; // expect: CXX-ARRAY-INDEX
}
int arr_ok(int i) {
    std::array<int, 4> a{};
    if (i < 0 || i >= 4)
        return 0;
    return a[i];
}
const int &asc_bad() {
    int loc = 3;
    return std::as_const(loc); // expect: CXX-AS-CONST
}
const int &asc_ok(const int &x) {
    return std::as_const(x);
}
float aa_bad(float *buf, int i) {
    float *p = std::assume_aligned<16>(buf);
    return p[i]; // expect: CXX-ASSUME-ALIGNED
}
float aa_ok(float *buf, int i) {
    float *p = std::assume_aligned<16>(buf);
    if (i < 0 || i > 15)
        return 0;
    return p[i];
}
bool flag_bad() {
    std::atomic_flag fl;
    return fl.test_and_set(); // expect: CXX-ATOMIC-FLAG
}
bool flag_ok() {
    std::atomic_flag fl;
    fl.clear();
    return fl.test_and_set();
}
int addr_bad(int *tab, int k) {
    int *p = std::addressof(k);
    return tab[*p]; // expect: CXX-ADDRESSOF
}
int addr_ok(int *tab, int k) {
    int *p = std::addressof(k);
    if (*p < 0 || *p > 7)
        return 0;
    return tab[*p];
}
#include <barrier>
#include <bit>
#include <bitset>
#include <condition_variable>
std::atomic_ref<int> aref_bad() {
    int loc = 0;
    std::atomic_ref<int> r(loc);
    return r; // expect: CXX-ATOMIC-REF
}
std::atomic_ref<int> aref_ok(int &x) {
    std::atomic_ref<int> r(x);
    return r;
}
void barrier_bad() {
    std::barrier<> bar(2);
    (void)bar.arrive(); // expect: CXX-BARRIER
}
void barrier_ok() {
    std::barrier<> bar(2);
    bar.arrive_and_wait();
}
int bitceil_bad(int *tab, unsigned x) {
    return tab[std::bit_ceil(x)]; // expect: CXX-BIT-CEIL
}
int bitceil_ok(int *tab, unsigned x) {
    unsigned k = std::bit_ceil(x);
    if (k >= 16)
        return 0;
    return tab[k];
}
bool bitset_bad(std::size_t pos) {
    std::bitset<8> bs;
    return bs.test(pos); // expect: CXX-BITSET-INDEX
}
bool bitset_ok(std::size_t pos) {
    std::bitset<8> bs;
    if (pos >= 8)
        return false;
    return bs.test(pos);
}
void once_bad(std::once_flag &f) {
    std::call_once(f, [] {}); // expect: CXX-CALL-ONCE
}
void once_ok() {
    static std::once_flag of;
    std::call_once(of, [] {});
}
int cmpless_bad(int *tab, int a, unsigned b) {
    return tab[std::cmp_less(a, b)]; // expect: CXX-CMP-LESS
}
int cmpless_ok(int *tab, int a, unsigned b) {
    return std::cmp_less(a, b) ? tab[0] : tab[1];
}
void cva_bad(std::mutex &m) {
    std::condition_variable_any cv;
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk); // expect: CXX-CONDVAR-ANY, CXX-CONDITION-WAIT
}
void cva_ok(std::mutex &m, bool &ready) {
    std::condition_variable_any cv;
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return ready; });
}
int cat_bad(int *p) {
    std::destroy_at(p);
    return *p; // expect: CXX-CONSTRUCT-AT
}
int cat_ok(int *p) {
    std::destroy_at(p);
    std::construct_at(p, 1);
    return *p;
}
#include <exception>
#include <system_error>
void cexc_bad() {
    std::exception_ptr ep = std::current_exception();
    std::rethrow_exception(ep); // expect: CXX-CURRENT-EXCEPTION
}
void cexc_ok() {
    std::exception_ptr ep = std::current_exception();
    if (ep)
        std::rethrow_exception(ep);
}
void eptr_bad(std::exception_ptr src) {
    std::exception_ptr held = src;
    std::rethrow_exception(held); // expect: CXX-EXCEPTION-PTR
}
void eptr_ok(std::exception_ptr src) {
    std::exception_ptr held = src;
    if (held != nullptr)
        std::rethrow_exception(held);
}
void dn_bad(std::string *p, std::size_t n) {
    std::destroy_n(p, n); // expect: CXX-DESTROY-N
}
void dn_ok(std::string *p, std::size_t n) {
    if (n > 4)
        return;
    std::destroy_n(p, n);
}
std::string ec_bad() {
    std::error_code ec;
    return ec.message(); // expect: CXX-ERROR-CODE
}
std::string ec_ok() {
    std::error_code ec;
    if (ec)
        return ec.message();
    return {};
}
int may_throw(int);
int leak_bad() {
    int *p = new int(3);
    int r = may_throw(*p); // expect: CXX-EXCEPTION-LEAK
    delete p;
    return r;
}
int leak_ok() {
    int *p = new int(3);
    int r = *p;
    delete p;
    return may_throw(r);
}
int xchg_bad(int *tab, int &cur) {
    int old = std::exchange(cur, 0);
    return tab[old]; // expect: CXX-EXCHANGE
}
int xchg_ok(int *tab, int &cur) {
    int old = std::exchange(cur, 0);
    if (old < 0 || old > 7)
        return 0;
    return tab[old];
}
#include <numeric>
#include <filesystem>
#include <format>
#include <charconv>
void exscan_bad(std::vector<int> &v) {
    std::exclusive_scan(v.begin(), v.end(), v.begin(), 0); // expect: CXX-EXCLUSIVE-SCAN
}
void exscan_ok(const std::vector<int> &v, std::vector<int> &o) {
    std::exclusive_scan(v.begin(), v.end(), o.begin(), 0);
}
int inscan_bad(const std::vector<int> &v, int *o, int i) {
    std::inclusive_scan(v.begin(), v.end(), o);
    return o[i]; // expect: CXX-INCLUSIVE-SCAN, MEM-PTR-ARITH
}
int inscan_ok(const std::vector<int> &v, int *o, int i) {
    std::inclusive_scan(v.begin(), v.end(), o);
    if (i < 0 || i >= 4)
        return 0;
    return o[i]; // expect: MEM-PTR-ARITH
}
int tscan_bad(const std::vector<int> &v, int *o, int i) {
    std::transform_inclusive_scan(v.begin(), v.end(), o, std::plus<>{}, [](int x) { return x; });
    return o[i]; // expect: CXX-TRANSFORM-SCAN, MEM-PTR-ARITH
}
int tscan_ok(const std::vector<int> &v, int *o) {
    std::transform_inclusive_scan(v.begin(), v.end(), o, std::plus<>{}, [](int x) { return x; });
    return o[0];
}
bool fs_bad(const std::filesystem::path &p) {
    std::filesystem::remove(p); // expect: API-UNLINK
    return std::filesystem::exists(p); // expect: CXX-FILESYSTEM
}
bool fs_ok(const std::filesystem::path &p) {
    return std::filesystem::remove(p);
}
void fmt_bad(int x) {
    char buf[8];
    std::format_to(buf, "{}", x); // expect: CXX-FORMAT-TO
}
void fmt_ok(int x) {
    char buf[8];
    std::format_to_n(buf, sizeof buf, "{}", x);
}
int fc_bad(const char *s, const char *e) {
    int v;
    std::from_chars(s, e, v);
    return v; // expect: CXX-FROM-CHARS
}
int fc_ok(const char *s, const char *e) {
    int v = 0;
    auto r = std::from_chars(s, e, v);
    if (r.ec != std::errc())
        return 0;
    return v;
}
char tc_bad(int x) {
    char out[4];
    std::to_chars(out, out + 4, x);
    return out[0]; // expect: CXX-TO-CHARS
}
char tc_ok(int x) {
    char out[4] = {};
    auto r = std::to_chars(out, out + 4, x);
    if (r.ec != std::errc())
        return 0;
    return out[0];
}
)";

}  // namespace

TEST_CASE("astlint: ported regex classes on C (planted + guards, both front ends)") {
    check_marks("ported.c", kPortedC);
}

TEST_CASE("astlint: testdata STR-MISSING-NUL, STR-STRNCPY-NUL, MEM-MEMSET-SWAP, MEM-REALLOC-SELF, "
          "API-SYSTEM, API-SIGNAL, API-GETENV-NULL, API-STRDUP-NULL, PTR-UNCHECKED-ALLOC, INT-SHIFT-UB, "
          "PTR-NULL-DEREF, MEM-STACK-ESCAPE (both front ends)") {
    auto clang = find_clang();
    if (!clang) {
        MESSAGE("clang not installed: testdata string/memory astlint skipped");
        return;
    }
    const auto root = fs::path(PRISM_SOURCE_DIR) / "testdata" / "planted";
    prism::Config cfg;
    cfg.jobs = 1;
    cfg.root = root;
    auto ast_lines = [&](const char* file, const char* cls, const char* func, Backend b) {
        std::set<int> lines;
        for (auto& f : prism::astlint::run_lints_ast({root / file}, root, cfg, clang, b)) {
            auto it = f.extra.find("engine");
            if (it == f.extra.end() || it->second != "clang-ast") continue;
            if (f.status != prism::laws::FAILED || f.cls != cls) continue;
            if (f.function.value_or("") != func) continue;
            lines.insert(f.line.value_or(0));
        }
        return lines;
    };
    for (auto& [fe, b] : front_ends(clang)) {
        INFO("front end: " << fe);
        CHECK(ast_lines("missing_nul.c", "STR-MISSING-NUL", "missing_nul_bad", b) == std::set<int>{5});
        CHECK(ast_lines("missing_nul.c", "STR-MISSING-NUL", "missing_nul_ok", b).empty());
        CHECK(ast_lines("missing_nul.c", "STR-MISSING-NUL", "missing_nul_sizeof_ok", b).empty());
        CHECK(ast_lines("strncpy_nul.c", "STR-STRNCPY-NUL", "strncpy_nul_bad", b) == std::set<int>{5});
        CHECK(ast_lines("strncpy_nul.c", "STR-STRNCPY-NUL", "strncpy_nul_ok", b).empty());
        CHECK(ast_lines("memset_swap.c", "MEM-MEMSET-SWAP", "memset_swap_bad", b) == std::set<int>{5});
        CHECK(ast_lines("memset_swap.c", "MEM-MEMSET-SWAP", "memset_ok", b).empty());
        CHECK(ast_lines("realloc_self.c", "MEM-REALLOC-SELF", "grow", b) == std::set<int>{5});
        CHECK(ast_lines("system_call.c", "API-SYSTEM", "system_bad", b) == std::set<int>{5});
        CHECK(ast_lines("system_call.c", "API-SYSTEM", "system_ok", b).empty());
        CHECK(ast_lines("signal_api.c", "API-SIGNAL", "signal_bad", b) == std::set<int>{14});
        CHECK(ast_lines("signal_api.c", "API-SIGNAL", "signal_ok", b).empty());
        CHECK(ast_lines("signal_api.c", "API-SIGNAL", "signal_dfl_ok", b).empty());
        CHECK(ast_lines("getenv_null.c", "API-GETENV-NULL", "getenv_bad", b) == std::set<int>{5});
        CHECK(ast_lines("getenv_null.c", "API-GETENV-NULL", "getenv_ok", b).empty());
        CHECK(ast_lines("strdup_null.c", "API-STRDUP-NULL", "strdup_bad", b) == std::set<int>{5});
        CHECK(ast_lines("strdup_null.c", "API-STRDUP-NULL", "strdup_ok", b).empty());
        CHECK(ast_lines("unchecked_alloc.c", "PTR-UNCHECKED-ALLOC", "unchecked_alloc", b) == std::set<int>{10});
        CHECK(ast_lines("unchecked_alloc.c", "PTR-UNCHECKED-ALLOC", "checked_alloc", b).empty());
        CHECK(ast_lines("unchecked_alloc.c", "PTR-UNCHECKED-ALLOC", "alloc_in_condition", b).empty());
        CHECK(ast_lines("shift_ub.c", "INT-SHIFT-UB", "shift_ub", b) == std::set<int>{3});
        CHECK(ast_lines("null_branch.c", "PTR-NULL-DEREF", "null_branch", b) == std::set<int>{4});
        CHECK(ast_lines("stack_escape.c", "MEM-STACK-ESCAPE", "esc_bad", b) == std::set<int>{4});
        CHECK(ast_lines("stack_escape.c", "MEM-STACK-ESCAPE", "arr_esc_bad", b) == std::set<int>{8});
        CHECK(ast_lines("stack_escape.c", "MEM-STACK-ESCAPE", "esc_paren_addr", b) == std::set<int>{29});
        CHECK(ast_lines("stack_escape.c", "MEM-STACK-ESCAPE", "esc_ok", b).empty());
        CHECK(ast_lines("noreturn_fatal.c", "FUNC-NORETURN", "fatal", b) == std::set<int>{4});
        CHECK(ast_lines("noreturn_fatal.c", "FUNC-NORETURN", "dead_noreturn", b).empty());
        CHECK(ast_lines("noreturn_fatal.c", "FUNC-NORETURN", "dead_attr", b).empty());
        CHECK(ast_lines("missing_return.c", "CTRL-MISSING-RETURN", "missing_return_bad", b) == std::set<int>{4});
        CHECK(ast_lines("missing_return.c", "CTRL-MISSING-RETURN", "missing_return_oneline_bad", b) ==
              std::set<int>{14});
        CHECK(ast_lines("missing_return.c", "CTRL-MISSING-RETURN", "missing_return_ok", b).empty());
        CHECK(ast_lines("missing_return.c", "CTRL-MISSING-RETURN", "missing_return_oneline_ok", b).empty());
        CHECK(ast_lines("malloc_wrap.c", "INT-WRAP-ALLOC", "wrap_alloc_bad", b) == std::set<int>{9});
        CHECK(ast_lines("malloc_wrap.c", "INT-WRAP-ALLOC", "calloc_ok", b).empty());
        CHECK(ast_lines("atoi_index.c", "INT-ATOI", "atoi_bad", b) == std::set<int>{6});
        CHECK(ast_lines("atoi_index.c", "INT-ATOI", "atoi_ok", b).empty());
        CHECK(ast_lines("atoi_index.c", "INT-ATOI", "atoi_return_ok", b).empty());
        CHECK(ast_lines("masked_switch.c", "UNINIT-SWITCH", "masked_switch", b) == std::set<int>{5});
        CHECK(ast_lines("null_arg.c", "STR-NULL-ARG", "null_arg_bad", b) == std::set<int>{4});
        CHECK(ast_lines("null_arg.c", "STR-NULL-ARG", "null_arg_ok", b).empty());
        CHECK(ast_lines("null_member.c", "STR-NULL-MEMBER", "null_member_bad", b) == std::set<int>{9});
        CHECK(ast_lines("null_member.c", "STR-NULL-MEMBER", "null_member_ok", b).empty());
        CHECK(ast_lines("chain_null.c", "PTR-CHAIN-NULL", "chain_null_bad", b) == std::set<int>{9});
        CHECK(ast_lines("chain_null.c", "PTR-CHAIN-NULL", "chain_null_ok", b).empty());
        CHECK(ast_lines("nowait_use.c", "MEM-NOWAIT", "nowait_unchecked", b) == std::set<int>{10});
        CHECK(ast_lines("nowait_use.c", "MEM-NOWAIT", "nowait_checked", b).empty());
        CHECK(ast_lines("alloca_var.c", "MEM-ALLOCA", "alloca_bad", b) == std::set<int>{4});
        CHECK(ast_lines("alloca_var.c", "MEM-ALLOCA", "alloca_ok", b).empty());
        CHECK(ast_lines("sprintf_buf.c", "STR-SPRINTF", "sprintf_bad", b) == std::set<int>{5});
        CHECK(ast_lines("sprintf_buf.c", "STR-SPRINTF", "sprintf_ok", b).empty());
        CHECK(ast_lines("snprintf_buf.c", "STR-SNPRINTF", "snprintf_bad", b) == std::set<int>{5});
        CHECK(ast_lines("snprintf_buf.c", "STR-SNPRINTF", "snprintf_ok", b).empty());
        CHECK(ast_lines("wcs_unbounded.c", "STR-WCSCPY", "wcs_copy_bad", b) == std::set<int>{7});
        CHECK(ast_lines("wcs_unbounded.c", "STR-WCSCPY", "wcs_copy_ok", b).empty());
        CHECK(ast_lines("crypto_misuse.c", "CRYPTO-MISUSE", "crypto_key_bad", b) == std::set<int>{6});
        CHECK(ast_lines("crypto_misuse.c", "CRYPTO-MISUSE", "crypto_key_ok", b).empty());
        CHECK(ast_lines("srand_time.c", "CRYPTO-SRAND", "srand_bad", b) == std::set<int>{5});
        CHECK(ast_lines("srand_time.c", "CRYPTO-SRAND", "srand_ok", b).empty());
        CHECK(ast_lines("realloc_zero.c", "MEM-REALLOC-ZERO", "realloc_zero_bad", b) == std::set<int>{7});
        CHECK(ast_lines("realloc_zero.c", "MEM-REALLOC-ZERO", "realloc_zero_ok", b).empty());
        CHECK(ast_lines("ptr_arith.c", "MEM-PTR-ARITH", "arith_bad", b) == std::set<int>{2});
        CHECK(ast_lines("ptr_arith.c", "MEM-PTR-ARITH", "arith_ok", b).empty());
        CHECK(ast_lines("vla.c", "MEM-VLA-SIZE", "vla_bad", b) == std::set<int>{2});
        CHECK(ast_lines("vla.c", "MEM-VLA-SIZE", "vla_ok", b).empty());
        CHECK(ast_lines("flex_array.c", "MEM-FLEX-ARRAY", "flex_bad", b) == std::set<int>{14});
        CHECK(ast_lines("flex_array.c", "MEM-FLEX-ARRAY", "flex_star_bad", b) == std::set<int>{19});
        CHECK(ast_lines("flex_array.c", "MEM-FLEX-ARRAY", "flex_ok", b).empty());
        CHECK(ast_lines("flex_array.c", "MEM-FLEX-ARRAY", "flex_plain_ok", b).empty());
        CHECK(ast_lines("infoleak_pad.c", "INFOLEAK-PAD", "leak_bad", b) == std::set<int>{9});
        CHECK(ast_lines("infoleak_pad.c", "INFOLEAK-PAD", "leak_ok", b).empty());
        CHECK(ast_lines("onesided.c", "MEM-ONESIDED-INDEX", "onesided_index", b) == std::set<int>{5});
        CHECK(ast_lines("onesided.c", "MEM-ONESIDED-INDEX", "two_sided_index", b).empty());
        CHECK(ast_lines("onesided.c", "MEM-ONESIDED-INDEX", "unsigned_index", b).empty());
        CHECK(ast_lines("mem_leak.c", "MEM-LEAK", "mem_leak_bad", b) == std::set<int>{6});
        CHECK(ast_lines("mem_leak.c", "MEM-LEAK", "mem_leak_ok", b).empty());
        CHECK(ast_lines("fd_leak.c", "RES-FD-LEAK", "fd_leak_bad", b) == std::set<int>{6});
        CHECK(ast_lines("fd_leak.c", "RES-FD-LEAK", "fd_leak_ok", b).empty());
        CHECK(ast_lines("popen.c", "API-POPEN", "popen_bad", b) == std::set<int>{8});
        CHECK(ast_lines("popen.c", "API-POPEN", "popen_ok", b).empty());
        CHECK(ast_lines("chroot.c", "API-CHROOT", "chroot_bad", b) == std::set<int>{6});
        CHECK(ast_lines("chroot.c", "API-CHROOT", "chroot_ok", b).empty());
        CHECK(ast_lines("toctou.c", "CONC-TOCTOU", "toctou_bad", b) == std::set<int>{7});
        CHECK(ast_lines("toctou.c", "CONC-TOCTOU", "toctou_ok", b).empty());
        CHECK(ast_lines("unvalidated.c", "TRUST-UNVALIDATED-INPUT", "unval_bad", b) == std::set<int>{7});
        CHECK(ast_lines("unvalidated.c", "TRUST-UNVALIDATED-INPUT", "unval_div_bad", b) == std::set<int>{24});
        CHECK(ast_lines("unvalidated.c", "TRUST-UNVALIDATED-INPUT", "unval_ok", b).empty());
        CHECK(ast_lines("clz_zero.c", "INT-CLZ-ZERO", "clz_zero_bad", b) == std::set<int>{2});
        CHECK(ast_lines("clz_zero.c", "INT-CLZ-ZERO", "clz_zero_ok", b).empty());
        CHECK(ast_lines("atomicity.c", "CONC-ATOMICITY", "atom_bad", b) == std::set<int>{7});
        CHECK(ast_lines("atomicity.c", "CONC-ATOMICITY", "atom_ok", b).empty());
    }
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
