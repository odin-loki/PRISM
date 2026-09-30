// Doctests for the C lint checks (src/prism/checkers_api.cpp,
// src/prism/checkers_core.cpp, and the CTRL-FALLTHROUGH switch rule of the
// Clang-AST layer): the "assigned then used unchecked" API branches, the
// C-only dispatch of STR-STRNCPY-NUL / STR-SNPRINTF, the real-code false
// alarms fixed in testdata_fp/ with their testdata_tp/ twins, and the CVE
// shapes of STR-NULL-MEMBER, PTR-CHAIN-NULL and MEM-COPY-LEN.

#include <doctest/doctest.h>

#include "prism/astlint.hpp"
#include "prism/checkers.hpp"
#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;

fs::path repo(const char* name) {
    return fs::path(__FILE__).parent_path().parent_path().parent_path() / name;
}

// One scratch directory per snippet, removed afterwards.
struct Snippet {
    fs::path dir;
    fs::path file;
    Snippet(const std::string& name, const std::string& text) {
        static std::atomic<int> n{0};
        auto t = std::chrono::steady_clock::now().time_since_epoch().count();
        dir = fs::temp_directory_path() /
              ("prism_checkers_c_" + std::to_string(t) + "_" + std::to_string(n++));
        fs::create_directories(dir);
        file = dir / name;
        std::ofstream(file, std::ios::binary) << text;
    }
    ~Snippet() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::vector<prism::Finding> lint() const { return prism::run_lints({file}, dir); }
};

// FAILED rows of one class: (line, message).
std::set<std::pair<int, std::string>> rows(const std::vector<prism::Finding>& fs_, const std::string& cls) {
    std::set<std::pair<int, std::string>> out;
    for (auto& f : fs_)
        if (f.status == prism::laws::FAILED && f.cls == cls) out.insert({f.line.value_or(0), f.message});
    return out;
}

std::set<int> lines_of(const std::vector<prism::Finding>& fs_, const std::string& cls) {
    std::set<int> out;
    for (auto& [line, _] : rows(fs_, cls)) out.insert(line);
    return out;
}

std::vector<prism::Finding> lint_tp(const char* name) {
    auto root = repo("testdata_tp");
    return prism::run_lints({root / name}, root);
}

std::vector<prism::Finding> lint_fp(const char* name) {
    auto root = repo("testdata_fp");
    return prism::run_lints({root / name}, root);
}

std::vector<prism::Finding> lint_td(const char* name) {
    auto root = repo("testdata");
    return prism::run_lints({root / name}, root);
}

std::size_t failed_count(const std::vector<prism::Finding>& fs_) {
    std::size_t n = 0;
    for (auto& f : fs_)
        if (f.status == prism::laws::FAILED) ++n;
    return n;
}

}  // namespace

// --- API: assigned, then used without the error test ------------------------

TEST_CASE("API-FORK: a pid assigned from fork() must be compared") {
    Snippet s("fork.c",
              "#include <unistd.h>\n"
              "void use(int);\n"
              "int spawn_bad(void) {\n"        // 3
              "    int pid;\n"
              "    pid = fork();\n"             // 5
              "    use(pid);\n"
              "    return 0;\n"
              "}\n"
              "int spawn_ok(void) {\n"         // 9
              "    int pid = fork();\n"         // 10
              "    if (pid < 0) return -1;\n"
              "    use(pid);\n"
              "    return 0;\n"
              "}\n"
              "int spawn_bang(void) {\n"       // 15
              "    int pid = vfork();\n"        // 16
              "    if (!pid) return 1;\n"
              "    return 0;\n"
              "}\n"
              "int spawn_cmp(void) {\n"        // 20
              "    if (fork() == 0) return 1;\n"
              "    fork();\n"                   // 22
              "    return 0;\n"
              "}\n");
    auto r = rows(s.lint(), "API-FORK");
    CHECK(r == std::set<std::pair<int, std::string>>{
                   {5, "fork()/vfork() result unused (not compared to 0/-1)"},
                   {22, "fork()/vfork() result unused (not compared to 0/-1)"}});
}

TEST_CASE("API-GETADDRINFO: a stored status must be tested against 0") {
    Snippet s("gai.c",
              "#include <netdb.h>\n"
              "int resolve_bad(const char *h, struct addrinfo **res) {\n"  // 2
              "    int rc = getaddrinfo(h, \"80\", 0, res);\n"               // 3
              "    freeaddrinfo(*res);\n"
              "    return 0;\n"
              "}\n"
              "int resolve_ok(const char *h, struct addrinfo **res) {\n"   // 7
              "    int rc = getaddrinfo(h, \"80\", 0, res);\n"
              "    if (rc != 0) return rc;\n"
              "    return 0;\n"
              "}\n"
              "int resolve_if(const char *h, struct addrinfo **res) {\n"   // 12
              "    int rc = getaddrinfo(h, \"80\", 0, res);\n"
              "    if (rc) return rc;\n"
              "    return 0;\n"
              "}\n"
              "int resolve_inline(const char *h, struct addrinfo **res) {\n"
              "    int rc;\n"
              "    if ((rc = getaddrinfo(h, \"80\", 0, res)) != 0) return rc;\n"
              "    return 0;\n"
              "}\n");
    auto r = rows(s.lint(), "API-GETADDRINFO");
    CHECK(r == std::set<std::pair<int, std::string>>{{3, "rc from getaddrinfo() used without a == 0 test"}});
}

TEST_CASE("API-KQUEUE/SHMGET/SEMGET/MSGGET: an id used without a < 0 test") {
    Snippet s("ids.c",
              "void use(int);\n"
              "void kq_bad(void) {\n"                       // 2
              "    int kq = kqueue();\n"                    // 3
              "    use(kq);\n"
              "}\n"
              "void kq_ok(void) {\n"                        // 6
              "    int kq = kqueue();\n"
              "    if (kq < 0) return;\n"
              "    use(kq);\n"
              "}\n"
              "void shm_bad(int k) {\n"                     // 11
              "    int id = shmget(k, 4096, 0600);\n"       // 12
              "    use(id);\n"
              "}\n"
              "void shm_ok(int k) {\n"                      // 15
              "    int id = shmget(k, 4096, 0600);\n"
              "    if (id < 0) return;\n"
              "    use(id);\n"
              "}\n"
              "void sem_bad(int k) {\n"                     // 20
              "    int id = semget(k, 1, 0600);\n"          // 21
              "    use(id);\n"
              "}\n"
              "void sem_ok(int k) {\n"                      // 24
              "    int id;\n"
              "    if ((id = semget(k, 1, 0600)) < 0) return;\n"
              "    use(id);\n"
              "}\n"
              "void msg_bad(int k) {\n"                     // 29
              "    int id = msgget(k, 0600);\n"             // 30
              "    use(id);\n"
              "}\n"
              "void msg_ok(int k) {\n"                      // 33
              "    int id = msgget(k, 0600);\n"
              "    if (id < 0) return;\n"
              "    use(id);\n"
              "}\n"
              "void msg_unused(int k) {\n"                  // 38
              "    int id = msgget(k, 0600);\n"             // never used: nothing to report
              "}\n");
    auto out = s.lint();
    CHECK(rows(out, "API-KQUEUE") == std::set<std::pair<int, std::string>>{{3, "kq from kqueue() used without a < 0 test"}});
    CHECK(rows(out, "API-SHMGET") == std::set<std::pair<int, std::string>>{{12, "id from shmget() used without a < 0 test"}});
    CHECK(rows(out, "API-SEMGET") == std::set<std::pair<int, std::string>>{{21, "id from semget() used without a < 0 test"}});
    CHECK(rows(out, "API-MSGGET") == std::set<std::pair<int, std::string>>{{30, "id from msgget() used without a < 0 test"}});
}

TEST_CASE("API-REALLOCARRAY/REALLOCF/VALLOC: a pointer used without a NULL check") {
    Snippet s("allocs.c",
              "#include <stdlib.h>\n"
              "void use(void *);\n"
              "void ra_bad(void *q, size_t n) {\n"          // 3
              "    char *p = reallocarray(q, n, 8);\n"      // 4
              "    p[0] = 1;\n"
              "}\n"
              "void ra_ok(void *q, size_t n) {\n"           // 7
              "    char *p = reallocarray(q, n, 8);\n"
              "    if (p == NULL) return;\n"
              "    p[0] = 1;\n"
              "}\n"
              "void rf_bad(void *q, size_t n) {\n"          // 12
              "    void *p = reallocf(q, n);\n"             // 13
              "    use(p);\n"
              "}\n"
              "void rf_ok(void *q, size_t n) {\n"           // 16
              "    void *p = reallocf(q, n);\n"
              "    if (!p) return;\n"
              "    use(p);\n"
              "}\n"
              "void va_bad(size_t n) {\n"                   // 21
              "    char *p = valloc(n);\n"                  // 22
              "    *p = 0;\n"
              "}\n"
              "void va_ok(size_t n) {\n"                    // 25
              "    char *p;\n"
              "    if ((p = valloc(n)) == NULL) return;\n"
              "    *p = 0;\n"
              "}\n");
    auto out = s.lint();
    CHECK(rows(out, "API-REALLOCARRAY") ==
          std::set<std::pair<int, std::string>>{{4, "p from reallocarray() used without a NULL check"}});
    CHECK(rows(out, "API-REALLOCF") ==
          std::set<std::pair<int, std::string>>{{13, "p from reallocf() used without a NULL check"}});
    CHECK(rows(out, "API-VALLOC") ==
          std::set<std::pair<int, std::string>>{{22, "p from valloc() used without a NULL check"}});
}

TEST_CASE("API discard rows are unchanged next to the new assign branches") {
    Snippet s("discard.c",
              "void f(int k, void *q) {\n"
              "    kqueue();\n"
              "    shmget(k, 1, 0);\n"
              "    reallocf(q, 8);\n"
              "}\n");
    auto out = s.lint();
    CHECK(rows(out, "API-KQUEUE") == std::set<std::pair<int, std::string>>{{2, "kqueue() return is discarded"}});
    CHECK(rows(out, "API-SHMGET") == std::set<std::pair<int, std::string>>{{3, "shmget() return is discarded"}});
    CHECK(rows(out, "API-REALLOCF") == std::set<std::pair<int, std::string>>{{4, "reallocf() return is discarded"}});
}

// --- dispatch: STR-STRNCPY-NUL and STR-SNPRINTF are C-only ------------------

TEST_CASE("STR-STRNCPY-NUL and STR-SNPRINTF run on C files, not C++ files") {
    const std::string text =
        "#include <stdio.h>\n"
        "#include <string.h>\n"
        "void copy_name(const char *src, int x) {\n"
        "    char d[8];\n"
        "    strncpy(d, src, sizeof d);\n"   // 5
        "    puts(d);\n"
        "    char b[8];\n"
        "    snprintf(b, 16, \"%d\", x);\n"  // 8
        "    puts(b);\n"
        "}\n";
    Snippet c("names.c", text);
    auto oc = c.lint();
    CHECK(lines_of(oc, "STR-STRNCPY-NUL") == std::set<int>{5});
    CHECK(lines_of(oc, "STR-SNPRINTF") == std::set<int>{8});
    for (auto* ext : {"names.cpp", "names.cc", "names.cxx"}) {
        INFO(ext);
        Snippet cpp(ext, text);
        auto ox = cpp.lint();
        CHECK(lines_of(ox, "STR-STRNCPY-NUL").empty());
        CHECK(lines_of(ox, "STR-SNPRINTF").empty());
    }
}

// --- MEM-COPY-LEN -------------------------------------------------------------

TEST_CASE("MEM-COPY-LEN: identifier lengths only; literals are constants") {
    Snippet s("copy.c",
              "#include <string.h>\n"
              "struct b { char d[64]; unsigned d_max; };\n"
              "void lit(char *d, const char *s) {\n"
              "    memcpy(d, s, 16);\n"                         // 4: literal, not reported
              "}\n"
              "void ident(struct b *x, const char *s, unsigned n) {\n"
              "    memcpy(x->d, s, n);\n"                       // 7
              "}\n"
              "void guarded(struct b *x, const char *s, unsigned n) {\n"
              "    if (n > x->d_max) return;\n"
              "    memmove(x->d, s, n);\n"                      // 11: guarded
              "}\n"
              "void late(struct b *x, const char *s, unsigned n) {\n"
              "    memcpy(x->d, s, n);\n"                       // 14: the test comes after
              "    if (n > x->d_max) return;\n"
              "}\n");
    auto r = rows(s.lint(), "MEM-COPY-LEN");
    CHECK(r == std::set<std::pair<int, std::string>>{
                   {7, "memcpy() length n not checked against a *_max bound in this function"},
                   {14, "memcpy() length n not checked against a *_max bound in this function"}});
    std::set<std::string> fns;
    for (auto& f : lint_td("copy_len.c"))
        if (f.status == prism::laws::FAILED && f.cls == "MEM-COPY-LEN") fns.insert(f.function.value_or(""));
    CHECK(fns == std::set<std::string>{"copy_len_bad"});
}

TEST_CASE("MEM-COPY-LEN: a wrapper copy is checked for X_max - v only") {
    // zlib inflate.c:431 `zmemcpy(state->window + state->wnext, end - copy,
    // dist)` in a file with *_max fields: a bare length through the wrapper
    // is not the CVE shape.
    Snippet s("wrap.c",
              "#include <string.h>\n"
              "#define zmemcpy memcpy\n"
              "struct w { unsigned char *window; unsigned wnext; unsigned w_max; };\n"
              "void upd(struct w *st, const unsigned char *end, unsigned dist) {\n"
              "    zmemcpy(st->window + st->wnext, end - dist, dist);\n"
              "}\n"
              "void wrapped(struct w *st, const unsigned char *s, unsigned v) {\n"
              "    zmemcpy(st->window, s, st->w_max - v);\n"          // 8
              "}\n");
    CHECK(lines_of(s.lint(), "MEM-COPY-LEN") == std::set<int>{8});
}

TEST_CASE("MEM-COPY-LEN: no *_max anywhere in the file, nothing to check against") {
    Snippet s("nomax.c",
              "#include <string.h>\n"
              "void f(char *d, const char *s, unsigned n) { memcpy(d, s, n); }\n");
    CHECK(rows(s.lint(), "MEM-COPY-LEN").empty());
}

TEST_CASE("MEM-COPY-LEN: zlib 1.2.12 zmemcpy with extra_max - len (CVE-2022-37434)") {
    auto tp = lint_tp("cve_shapes.c");
    auto r = rows(tp, "MEM-COPY-LEN");
    CHECK(r == std::set<std::pair<int, std::string>>{
                   {50, "zmemcpy() length uses extra_max - len with no len < extra_max test before it (it can wrap)"}});
    // The 1.2.12.1 fix `(len = ...) < extra_max`, a plain `len < extra_max`
    // and a literal length stay silent.
    CHECK(failed_count(lint_fp("copy_len_forms.c")) == 0);
}

// --- STR-NULL-MEMBER ----------------------------------------------------------

TEST_CASE("STR-NULL-MEMBER: a test after the use does not protect it (CVE-2023-50472)") {
    auto tp = lint_tp("cve_shapes.c");
    CHECK(lines_of(tp, "STR-NULL-MEMBER") == std::set<int>{33, 59});
    auto td = lint_td("null_member.c");
    std::set<std::string> fns;
    for (auto& f : td)
        if (f.status == prism::laws::FAILED && f.cls == "STR-NULL-MEMBER") fns.insert(f.function.value_or(""));
    CHECK(fns == std::set<std::string>{"null_member_bad"});
    Snippet s("spaced.c",
              "#include <string.h>\n"
              "struct o { char *v; };\n"
              "size_t f(struct o *o) {\n"
              "    if (! o->v) return 0;\n"
              "    return strlen(o->v);\n"
              "}\n"
              "size_t g(struct o *o) {\n"
              "    if (o -> v == NULL) return 0;\n"
              "    return strlen(o->v);\n"
              "}\n");
    CHECK(rows(s.lint(), "STR-NULL-MEMBER").empty());
}

// --- PTR-CHAIN-NULL -----------------------------------------------------------

TEST_CASE("PTR-CHAIN-NULL: guarded idioms are silent, one row per access") {
    CHECK(failed_count(lint_fp("chain_guards.c")) == 0);
    auto tp = lint_tp("cve_shapes.c");
    // chain_many: three uses of a->next->v, one row at the first.
    CHECK(lines_of(tp, "PTR-CHAIN-NULL") == std::set<int>{65});
    auto td = lint_td("chain_null.c");
    std::set<std::string> fns;
    for (auto& f : td)
        if (f.status == prism::laws::FAILED && f.cls == "PTR-CHAIN-NULL") fns.insert(f.function.value_or(""));
    CHECK(fns == std::set<std::string>{"chain_null_bad"});
    Snippet s("late.c",
              "struct l { struct l *next; int v; };\n"
              "int f(struct l *a) {\n"
              "    int v = a->next->v;\n"          // 3: the test is after the use
              "    if (a->next == 0) return 0;\n"
              "    return v;\n"
              "}\n");
    CHECK(lines_of(s.lint(), "PTR-CHAIN-NULL") == std::set<int>{3});
}

TEST_CASE("PTR-CHAIN-NULL and MEM-COPY-LEN stay linear on a long function") {
    std::string text = "#include <string.h>\nstruct c { int c; };\nstruct b { struct c *b0; unsigned n_max; };\n"
                       "void big(struct b *a, char *d, const char *s, unsigned n) {\n";
    constexpr int kLines = 5000;
    for (int i = 0; i < kLines; ++i) {
        text += "    a->b" + std::to_string(i % 50) + "->c = " + std::to_string(i) + ";\n";
        if (i % 100 == 0) text += "    memcpy(d, s, n);\n";
    }
    text += "}\n";
    Snippet s("big.c", text);
    auto t0 = std::chrono::steady_clock::now();
    auto out = s.lint();
    auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(secs < 10.0);
    // One row per distinct unchecked access, not one per line.
    CHECK(lines_of(out, "PTR-CHAIN-NULL").size() == 50);  // a->b0 .. a->b49
    CHECK(lines_of(out, "MEM-COPY-LEN").size() == 50);
}

// --- real-code false alarms and their twins -----------------------------------

TEST_CASE("PTR-UNCHECKED-ALLOC: a same-file macro that null-tests its argument is a check") {
    CHECK(failed_count(lint_fp("check_null_macro.c")) == 0);
    CHECK(lines_of(lint_tp("check_null_macro.c"), "PTR-UNCHECKED-ALLOC") == std::set<int>{14});
    Snippet s("bang.c",
              "#include <stdlib.h>\n"
              "#define NEED(p) \\\n"
              "    if (!(p)) \\\n"
              "        abort()\n"
              "struct s { int v; };\n"
              "struct s *mk(void) {\n"
              "    struct s *r = malloc(sizeof *r);\n"
              "    NEED(r);\n"
              "    r->v = 1;\n"
              "    return r;\n"
              "}\n");
    CHECK(lines_of(s.lint(), "PTR-UNCHECKED-ALLOC").empty());
}

TEST_CASE("CTRL-FALLTHROUGH: a nested switch that returns on every arm ends the case") {
    CHECK(failed_count(lint_fp("nested_switch.c")) == 0);
    // An inner break, or no default, leaves the outer arm running on.
    CHECK(lines_of(lint_tp("nested_switch.c"), "CTRL-FALLTHROUGH") == std::set<int>{8, 25});
}

TEST_CASE("STR-MISSING-NUL: a later NUL store into the same array terminates it") {
    CHECK(failed_count(lint_fp("later_nul.c")) == 0);
    CHECK(lines_of(lint_tp("later_nul.c"), "STR-MISSING-NUL") == std::set<int>{9});
}

TEST_CASE("MEM-CAPACITY-FIRST: a failure arm that frees the owner and leaves") {
    CHECK(failed_count(lint_fp("free_owner.c")) == 0);
    CHECK(lines_of(lint_tp("free_owner.c"), "MEM-CAPACITY-FIRST") == std::set<int>{19});
}

TEST_CASE("CTRL-FALLTHROUGH (Clang AST): SwitchStmt terminates only with a default and no break") {
    prism::Config cfg;
    auto fe = prism::pir::find_frontend(cfg);
    auto clang = fe.clang ? fe.clang : fe.clangxx;
    if (!clang) {
        MESSAGE("clang not installed: AST fallthrough test skipped");
        return;
    }
    auto ast_rows = [&](const char* dir, const char* name) {
        auto root = repo(dir);
        prism::Config c;
        c.jobs = 1;
        c.root = root;
        std::set<int> out;
        for (auto& f : prism::astlint::run_lints_ast({root / name}, root, c, clang)) {
            auto it = f.extra.find("engine");
            if (it != f.extra.end() && it->second == "clang-ast" && f.status == prism::laws::FAILED &&
                f.cls == "CTRL-FALLTHROUGH")
                out.insert(f.line.value_or(0));
        }
        return out;
    };
    CHECK(ast_rows("testdata_fp", "nested_switch.c").empty());
    CHECK(ast_rows("testdata_tp", "nested_switch.c") == std::set<int>{8, 25});
}
