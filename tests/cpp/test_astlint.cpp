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
    auto out = lint(t, {p}, clang);
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
    auto out = lint(t, {p}, clang);
    auto hits = ast_hits(out);
    for (auto& h : hits) MESSAGE("unexpected: line " << h.first << " " << h.second);
    CHECK(hits.empty());
    for (auto& f : out) CHECK(f.status != prism::laws::NOTRUN);
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
    auto out = lint(t, {p}, clang);
    auto hits = ast_hits(out);
    CHECK(hits.contains({7, "CTRL-DEAD-STORE"}));
    CHECK(hits.contains({8, "INT-ENUM-HOLE"}));
    CHECK(hits.contains({9, "INT-SIGN-CONV"}));
    CHECK(hits.contains({14, "CTRL-DEAD-STORE"}));  // lambda body: one row, not two
    CHECK_FALSE(hits.contains({13, "CTRL-DEAD-STORE"}));  // uninstantiated template: skipped
    CHECK(hits.size() == 4);
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
