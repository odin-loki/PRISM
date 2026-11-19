// Doctests: the r1 FALSE triage item 6 answers (OS-Proving data/coverage/prism_false_triage/SUMMARY.md, patterns 5,
// 9, 10, 12, 14): asm condition-code outputs, the sx-lock (OpenZFS mutex_enter) models, --pir-lock-scopes,
// --pir-bpf-helpers and --pir-global-harness-deep. Every answer is exact semantics or a named assumption; every
// *_bad function stays FAILED with the option on (a real imbalance where both ends are visible, a double release, a
// sleep under a caller-held spinlock, an _OR_NULL or scalar BPF argument, an access past the verified size, a bad
// value in a harnessed object).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace pp = prism::pir;

bool skip_without_frontend(const prism::Config& cfg) {
    std::string why;
    if (!pp::frontend_usable(cfg, &why)) {
        MESSAGE(why);
        return true;
    }
    if (!pp::z3_available()) {
        MESSAGE("NOTRUN: pir triage tests need Z3 (rebuild with -DPRISM_Z3=ON)");
        return true;
    }
    return false;
}

std::vector<prism::Finding> run_file(const std::string& file, const std::string& kernel,
                                     const std::function<void(prism::Config&)>& set) {
    auto cfg = prism::default_config();
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    cfg.pir_kernel_api = kernel;
    set(cfg);
    return pp::run_pir({dir / file}, cfg);
}

std::map<std::string, prism::Finding> by_function(const std::vector<prism::Finding>& out) {
    std::map<std::string, prism::Finding> m;
    for (auto& f : out)
        if (f.function && f.cls != "LOCK-ORDER-INVERSION" && !(f.cls == "SLEEP-IN-ATOMIC" && f.extra.count("context")))
            m[*f.function] = f;
    return m;
}

std::string assumptions_of(const prism::Finding& f) {
    auto it = f.extra.find("assumptions");
    return it == f.extra.end() ? std::string() : it->second;
}

// status check: `want` exact; tag (when not empty) must be in the assumptions of a PROVED-ASSUMING row
void expect(const std::map<std::string, prism::Finding>& got, const std::string& fn, const std::string& want,
            const std::string& tag = "") {
    auto it = got.find(fn);
    REQUIRE_MESSAGE(it != got.end(), fn + " missing");
    const auto& f = it->second;
    CHECK_MESSAGE(f.status == want, fn + ": " + f.status + " != " + want + " (" + f.message + ")");
    if (!tag.empty() && f.status == prism::laws::PROVED_ASSUMING)
        CHECK_MESSAGE(assumptions_of(f).find(tag) != std::string::npos, fn + " lacks '" + tag + "': " + assumptions_of(f));
}

struct KRun {
    std::string status, msg;
    std::vector<std::string> assumptions;
};

KRun kernel_run(const std::string& ir, const std::string& fn) {
    static std::vector<pp::ir::Module> keep;
    keep.push_back(pp::ir::parse_module_kernel(ir));
    const auto* f = keep.back().find(fn);
    REQUIRE(f != nullptr);
    pp::TranslateOptions o;
    o.kernel_api = "linux";
    auto t = pp::translate(keep.back(), *f, o);
    if (!t.fn) return {std::string(prism::laws::NEEDS_HARNESS), t.reason, {}};
    auto v = pp::check_function(*t.fn, 4, 30.0);
    return {v.status, v.message, t.fn->assumptions};
}

bool has(const std::vector<std::string>& v, const std::string& s) {
    for (auto& x : v)
        if (x.find(s) != std::string::npos) return true;
    return false;
}

// an asm statement PRISM cannot model exactly, with a condition-code output (as FreeBSD's atomic_fcmpset and Linux's
// test_bit asm); clang states the output's range with an llvm.assume right after it
const char* kCc = R"IR(declare void @llvm.assume(i1)
define i32 @cc_ok() {
entry:
  %r = call i8 asm sideeffect "frobnicate", "={@ccz},~{dirflag},~{fpsr},~{flags}"()
  %c = icmp ult i8 %r, 2
  call void @llvm.assume(i1 %c)
  %z = zext i8 %r to i32
  ret i32 %z
}
define i32 @cc_div_bad() {
entry:
  %r = call i8 asm sideeffect "frobnicate", "={@ccz},~{dirflag},~{fpsr},~{flags}"()
  %z = zext i8 %r to i32
  %d = udiv i32 100, %z
  ret i32 %d
}
define i32 @plain_assume_bad(i32 %x) {
entry:
  %c = icmp ult i32 %x, 2
  call void @llvm.assume(i1 %c)
  ret i32 %x
}
)IR";

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir triage item 6: an asm condition-code output is 0 or 1 (the llvm.assume clang adds holds)") {
    auto ok = kernel_run(kCc, "cc_ok");
    CHECK_MESSAGE(ok.status == prism::laws::PROVED, (ok.status + " " + ok.msg));
    CHECK(has(ok.assumptions, "condition-code output(s): 0 or 1"));
    // the flag can still be 0
    CHECK(kernel_run(kCc, "cc_div_bad").status == prism::laws::FAILED);
    // an llvm.assume on an input is still a checked contract
    CHECK(kernel_run(kCc, "plain_assume_bad").status == prism::laws::FAILED);
}
#endif

TEST_CASE("pir triage item 6: sx locks (OpenZFS mutex_enter / mutex_exit / mutex_tryenter) pair, failures stay FAILED") {
    auto cfg0 = prism::default_config();
    if (skip_without_frontend(cfg0)) return;
    const std::string PA = prism::laws::PROVED_ASSUMING, F = prism::laws::FAILED;
    auto got = by_function(run_file("kernel_item6_freebsd.c", "freebsd", [](prism::Config&) {}));
    expect(got, "zfs_mutex_ok", PA, "freebsd-lock");
    expect(got, "sx_try_ok", PA, "freebsd-lock");
    expect(got, "sx_sig_bad", F);
    expect(got, "sx_try_bad", F);
    expect(got, "zfs_leak_bad", F);
}

TEST_CASE("pir triage item 6: --pir-lock-scopes: per-lock caller-held / returned-held, imbalances still FAILED") {
    auto cfg0 = prism::default_config();
    if (skip_without_frontend(cfg0)) return;
    const std::string PA = prism::laws::PROVED_ASSUMING, F = prism::laws::FAILED;
    auto off = by_function(run_file("kernel_item6_linux.c", "linux", [](prism::Config&) {}));
    expect(off, "rel_a_ok", F);
    expect(off, "acq_a_ok", F);
    expect(off, "rcu_unlock_only_ok", F);
    expect(off, "rcu_lock_only_ok", F);
    auto on = by_function(run_file("kernel_item6_linux.c", "linux", [](prism::Config& c) { c.pir_lock_scopes = true; }));
    expect(on, "rel_a_ok", PA, "lock-scope: @rel_a_ok releases @lock_a and acquires it nowhere");
    expect(on, "acq_a_ok", PA, "lock-scope: @acq_a_ok acquires @lock_a and releases it nowhere");
    expect(on, "balanced_ok", PA, "linux-lock");
    expect(on, "rcu_unlock_only_ok", PA, "lock-scope: @rcu_unlock_only_ok leaves RCU read-side sections");
    expect(on, "rcu_lock_only_ok", PA, "lock-scope: @rcu_lock_only_ok enters RCU read-side sections");
    // real imbalances where both ends are visible, double releases and sleeping under a caller-held spinlock
    expect(on, "leak_path_bad", F);
    expect(on, "unheld_path_bad", F);
    expect(on, "rel_a_twice_bad", F);
    expect(on, "spin_caller_held_sleep_bad", F);
    expect(on, "rcu_unlock_twice_bad", F);
    for (auto* fn : {"leak_path_bad", "unheld_path_bad"}) {
        auto it = on.find(fn);
        REQUIRE(it != on.end());
        CHECK(assumptions_of(it->second).find("lock-scope:") == std::string::npos);
    }
}

TEST_CASE("pir triage item 6: --pir-lock-scopes: guard() scopes end at their cleanup; constant branches are dead") {
    auto cfg0 = prism::default_config();
    if (skip_without_frontend(cfg0)) return;
    auto inversions = [](const std::vector<prism::Finding>& out) {
        std::vector<std::string> v;
        for (auto& f : out)
            if (f.cls == "LOCK-ORDER-INVERSION" && f.extra.count("lock_classes")) v.push_back(f.extra.at("lock_classes"));
        return v;
    };
    auto hardirq_sleeps = [](const std::vector<prism::Finding>& out) {
        std::vector<std::string> v;
        for (auto& f : out)
            if (f.cls == "SLEEP-IN-ATOMIC" && f.extra.count("context") && f.function) v.push_back(*f.function);
        return v;
    };
    auto off = run_file("kernel_item6_scopes.c", "linux", [](prism::Config&) {});
    auto inv_off = inversions(off);
    // without the option the guard's lock is never released: a false lock_a / lock_b inversion
    CHECK(std::find(inv_off.begin(), inv_off.end(), "lock_a,lock_b") != inv_off.end());
    CHECK(std::find(inv_off.begin(), inv_off.end(), "lock_c,lock_d") != inv_off.end());
    auto sl_off = hardirq_sleeps(off);
    CHECK(std::find(sl_off.begin(), sl_off.end(), "irq_poll_zero") != sl_off.end());

    auto on = run_file("kernel_item6_scopes.c", "linux", [](prism::Config& c) { c.pir_lock_scopes = true; });
    auto inv_on = inversions(on);
    CHECK(std::find(inv_on.begin(), inv_on.end(), "lock_a,lock_b") == inv_on.end());
    // lock_d is taken inside lock_c's guard scope, and d_then_c takes them the other way: still an inversion
    CHECK(std::find(inv_on.begin(), inv_on.end(), "lock_c,lock_d") != inv_on.end());
    auto sl_on = hardirq_sleeps(on);
    CHECK(std::find(sl_on.begin(), sl_on.end(), "irq_poll_zero") == sl_on.end());
    CHECK(std::find(sl_on.begin(), sl_on.end(), "irq_poll_one") != sl_on.end());
}

TEST_CASE("pir triage item 6: --pir-bpf-helpers: ARG_PTR_TO_* arguments are valid objects; _OR_NULL and scalars are not") {
    auto cfg0 = prism::default_config();
    if (skip_without_frontend(cfg0)) return;
    const std::string PA = prism::laws::PROVED_ASSUMING, F = prism::laws::FAILED;
    auto off = by_function(run_file("kernel_item6_linux.c", "linux", [](prism::Config&) {}));
    CHECK(off.at("helper_ctx_ok").status != prism::laws::PROVED_ASSUMING);
    CHECK(off.at("helper_mem_ok").status != prism::laws::PROVED_ASSUMING);
    auto on = by_function(run_file("kernel_item6_linux.c", "linux", [](prism::Config& c) { c.pir_bpf_helpers = true; }));
    expect(on, "helper_ctx_ok", PA, "bpf verifier: argument arg1 (r1) of @helper_ctx_ok typed ARG_PTR_TO_CTX");
    expect(on, "helper_mem_ok", PA, "typed ARG_PTR_TO_MEM");
    expect(on, "helper_ctx_null_bad", F);
    expect(on, "helper_scalar_bad", F);
    expect(on, "helper_mem_bad", F);
}

TEST_CASE("pir triage item 6: --pir-global-harness-deep: nested fields and static list heads, values still checked") {
    auto cfg0 = prism::default_config();
    if (skip_without_frontend(cfg0)) return;
    const std::string PA = prism::laws::PROVED_ASSUMING, F = prism::laws::FAILED;
    auto shallow = by_function(run_file("kernel_item6_linux.c", "linux", [](prism::Config& c) { c.pir_global_harness = true; }));
    CHECK(shallow.at("nest_ok").status != prism::laws::PROVED_ASSUMING);
    CHECK(shallow.at("list_count_ok").status != prism::laws::PROVED_ASSUMING);
    auto deep = by_function(run_file("kernel_item6_linux.c", "linux", [](prism::Config& c) {
        c.pir_global_harness = true;
        c.pir_global_harness_deep = true;
    }));
    expect(deep, "nest_ok", PA, "global harness (deep): @the_dev->*->@0");
    expect(deep, "list_count_ok", PA, "global harness (deep): @items (a struct list_head)");
    expect(deep, "nest_div_bad", F);
    expect(deep, "list_div_bad", F);
}
