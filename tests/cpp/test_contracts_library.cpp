// Contract library (docs/CONTRACTS.md, docs/TARGETED_PROVING.md W4): the
// contracts stage checks ACSL contracts of pointer functions whose pointers
// have \valid requires; the pir stage answers a declared kernel callee with
// its library contract (models/kernel/kernel.cpp), listed as an assumption.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"
#include "prism/stages.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#ifdef _WIN32
#  include <process.h>
#  define getpid _getpid
#else
#  include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
namespace pp = prism::pir;

bool no_checker(const prism::Config& cfg) {
    std::string why;
    if (!pp::frontend_usable(cfg, &why)) {
        MESSAGE(why);
        return true;
    }
    if (!pp::z3_available()) {
        MESSAGE("NOTRUN: needs Z3");
        return true;
    }
    return false;
}

fs::path scratch(const std::string& tag) {
    auto d = fs::temp_directory_path() / ("prism_clib_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::map<std::string, prism::Finding> by_function(const std::vector<prism::Finding>& rows) {
    std::map<std::string, prism::Finding> m;
    for (auto& f : rows)
        if (f.function) m[*f.function] = f;
    return m;
}

const char* kPointerContracts = R"(struct s { int a, b; };

/*@ requires \valid(p);
    requires -1000 < p->a < 1000;
    assigns p->a;
    ensures p->a == \old(p->a) + 1;
*/
void inc_ok(struct s *p) { p->a++; }

/*@ requires \valid(p);
    requires -1000 < p->a < 1000;
    ensures p->a == \old(p->a) + 2;
*/
void inc_bad(struct s *p) { p->a++; }

/*@ requires 0 <= x && x < 20;
    ensures 0 <= \result < 10;
*/
int chain_bad(int x) { return x; }

/*@ ensures x > 0 ==> \result == 1;
    ensures x <= 0 ==> \result == 0;
*/
int imp_ok(int x) { return x > 0; }

/*@ requires 0 <= n <= 8;
    ensures \result == n;
*/
int entry_value_ok(int n) { int r = 0; while (n > 0) { n--; r++; } return r; }

/*@ requires \valid(p + (0..n-1));
    requires 0 <= n <= 4;
    ensures \forall integer i; 0 <= i < n ==> p[i] == 0;
*/
int forall_refused(int *p, int n) { for (int i = 0; i < n; i++) p[i] = 0; return 0; }

/*@ requires \valid(p);
    ensures \result == -1 || \result == p->a;
*/
long long_result_ok(struct s *p) { if (p->a < 0) return -1; return p->a; }

int no_valid(struct s *p) {
    // requires: p != 0
    // ensures: result >= 0
    return p->a;
}
)";

}  // namespace

TEST_CASE("contracts library: pointer contracts with \\valid requires are checked (\\old, ==>, chains, entry values)") {
    auto cfg = prism::default_config();
    if (no_checker(cfg)) return;
    auto dir = scratch("ptr");
    auto src = dir / "c.c";
    std::ofstream(src) << kPointerContracts;
    auto fns = prism::extract_functions(src, src.string());
    auto m = by_function(prism::prove_contracts(fns, 8));
    fs::remove_all(dir);
    // a pointer contract is a proof under the binding, never PROVED
    CHECK(m["inc_ok"].status == std::string(prism::laws::PROVED_ASSUMING));
    CHECK(m["inc_ok"].extra["assumptions"].find("p points to the start of a valid object") != std::string::npos);
    // the frame is not checked, and the row says so (Law 7)
    CHECK(m["inc_ok"].extra["assigns"] == "p->a");
    CHECK(m["inc_ok"].extra["assigns_checked"] == "false");
    CHECK(m["inc_ok"].message.find("assigns clause not checked") != std::string::npos);
    CHECK_FALSE(m["imp_ok"].extra.contains("assigns"));
    CHECK(m["inc_bad"].status == std::string(prism::laws::FAILED));
    // 0 <= \result < 10 is a chain, not C's (0 <= r) < 10 (always true)
    CHECK(m["chain_bad"].status == std::string(prism::laws::FAILED));
    CHECK(prism::laws::is_proof(m["imp_ok"].status));
    // a parameter in an ensures is its value on entry
    CHECK(prism::laws::is_proof(m["entry_value_ok"].status));
    CHECK(m["forall_refused"].status == std::string(prism::laws::ERROR));
    CHECK(m["forall_refused"].message.find("\\forall") != std::string::npos);
    // the result has the function's type (long), not int
    CHECK(prism::laws::is_proof(m["long_result_ok"].status));
    // without a \valid requires a pointer contract still invents no buffer
    CHECK(m["no_valid"].status == std::string(prism::laws::NEEDS_HARNESS));
    CHECK(m["no_valid"].message.find("invent a buffer") != std::string::npos);
}

TEST_CASE("pir: a kernel library contract answers a declared callee, listed as an assumption") {
    auto cfg = prism::default_config();
    if (no_checker(cfg)) return;
    auto dir = scratch("kcall");
    std::ofstream(dir / "k.c") << R"(#include <stddef.h>
struct list_head { struct list_head *next, *prev; };
void INIT_LIST_HEAD(struct list_head *list);
void list_add_tail(struct list_head *node, struct list_head *head);
int list_empty(const struct list_head *head);
void *kzalloc(size_t size, unsigned int flags);
void kfree(const void *p);
int __ilog2_u32(unsigned int n);
typedef struct spinlock { int raw; } spinlock_t;
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);
unsigned int readl(const volatile void *addr);

int list_ok(void) {
    struct list_head q, a;
    INIT_LIST_HEAD(&q);
    list_add_tail(&a, &q);
    return list_empty(&q);
}

int log2_ok(unsigned v) {
    int t[32] = {0};
    if (v == 0) return 0;
    return t[__ilog2_u32(v)];
}

int lock_ok(int v) {
    struct { spinlock_t l; int c; unsigned r; } d;
    if (v < 0 || v > 9) return 0;
    d.c = v;
    spin_lock(&d.l);
    d.c++;
    spin_unlock(&d.l);
    if (d.c != v + 1) d.r = 1 / (d.c - d.c);
    return (int)(readl(&d.r) & 1u);
}

int readl_known_bad(void) {
    unsigned r = 0;
    int t[2] = {0, 0};
    return t[readl(&r) & 3u];
}

int kzalloc_unchecked_bad(void) {
    int *p = kzalloc(4 * sizeof(int), 0);
    int v = p[0];
    kfree(p);
    return v;
}
)";
    std::ofstream(dir / "w.c") << "long __ilog2_u32(long n);\nint wrong_sig(long n) { return (int)__ilog2_u32(n); }\n";
    cfg.root = dir;
    cfg.jobs = 1;
    auto m = by_function(pp::run_pir({dir / "k.c", dir / "w.c"}, cfg));
    fs::remove_all(dir);
    CHECK(m["list_ok"].status == std::string(prism::laws::PROVED_ASSUMING));
    CHECK(m["list_ok"].extra["assumptions"].find("library contract kernel:list_add_tail") != std::string::npos);
    CHECK(m["log2_ok"].status == std::string(prism::laws::PROVED_ASSUMING));
    CHECK(m["log2_ok"].extra["assumptions"].find("kernel:__ilog2_u32") != std::string::npos);
    // locks change no memory the caller sees; an MMIO read is any value
    CHECK(m["lock_ok"].status == std::string(prism::laws::PROVED_ASSUMING));
    CHECK(m["lock_ok"].extra["assumptions"].find("kernel:spin_lock") != std::string::npos);
    CHECK(m["readl_known_bad"].status == std::string(prism::laws::FAILED));
    // the contract's requires are checked: kzalloc may fail
    CHECK(m["kzalloc_unchecked_bad"].status == std::string(prism::laws::FAILED));
    // another signature is another function: not answered by the contract
    CHECK(m["wrong_sig"].status == std::string(prism::laws::NEEDS_HARNESS));
    CHECK(m["wrong_sig"].message.find("does not match the library contract") != std::string::npos);
}

TEST_CASE("pir: a violation excluded by a requires pir does not assume is NEEDS-HARNESS, not FAILED") {
    auto cfg = prism::default_config();
    if (no_checker(cfg)) return;
    auto dir = scratch("unbound");
    std::ofstream(dir / "u.c") << R"(struct r { unsigned head; unsigned char d[8]; };

/*@ requires \valid(r);
    requires r->head < 8;
*/
void put(struct r *r, unsigned char b) { r->d[r->head] = b; }

/*@ requires \valid(r); */
void put_bad(struct r *r, unsigned char b) { r->d[r->head] = b; }

struct n { struct n *next; int v; };
/*@ requires \valid(p) && \valid(p->next); */
int next_v(struct n *p) { return p->next->v; }
)";
    cfg.root = dir;
    cfg.jobs = 1;
    auto m = by_function(pp::run_pir({dir / "u.c"}, cfg));
    fs::remove_all(dir);
    CHECK(m["put"].status == std::string(prism::laws::NEEDS_HARNESS));
    CHECK(m["put"].extra["unbound_requires"].find("r->head < 8") != std::string::npos);
    CHECK(m["put"].extra["verdict_under_unbound_requires"].starts_with("FAILED"));
    CHECK(m["put_bad"].status == std::string(prism::laws::FAILED));
    CHECK(m["next_v"].status != std::string(prism::laws::FAILED));
    CHECK_FALSE(prism::laws::is_proof(m["next_v"].status));
}
