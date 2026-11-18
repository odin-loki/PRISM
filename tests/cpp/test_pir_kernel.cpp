// Kernel API specs, inline asm / atomics models and the unknown-call policy
// (docs/KERNEL_MODELS.md). Table-driven on testdata/pir/kernel_*.c: every
// `*_ok` function is PROVED-ASSUMING with the named assumption of its spec, every
// `*_bad` function is FAILED (a true and a false variant of each spec), and the
// same functions are NEEDS-HARNESS (UNENCODED) when the opt-in is off.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace pp = prism::pir;

struct Row {
    const char* function;
    const char* status;
    const char* tag;  // PROVED-ASSUMING: a substring of the assumption list that must be there ("" = none required)
};

bool skip_without_frontend(const prism::Config& cfg) {
    std::string why;
    if (!pp::frontend_usable(cfg, &why)) {
        MESSAGE(why);
        return true;
    }
    if (!pp::z3_available()) {
        MESSAGE("NOTRUN: pir kernel tests need Z3 (rebuild with -DPRISM_Z3=ON)");
        return true;
    }
    return false;
}

std::map<std::string, prism::Finding> run(const std::string& file, const std::string& kernel, bool unknown) {
    auto cfg = prism::default_config();
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    cfg.pir_kernel_api = kernel;
    cfg.pir_assume_unknown_calls = unknown;
    auto out = pp::run_pir({dir / file}, cfg);
    std::map<std::string, prism::Finding> m;
    for (auto& f : out)
        if (f.function) m[*f.function] = f;
    return m;
}

std::string assumptions_of(const prism::Finding& f) {
    auto it = f.extra.find("assumptions");
    return it == f.extra.end() ? std::string() : it->second;
}

void check_table(const std::string& file, const std::string& kernel, bool unknown, const std::vector<Row>& rows) {
    auto got = run(file, kernel, unknown);
    std::vector<std::string> wrong;
    for (auto& r : rows) {
        auto it = got.find(r.function);
        if (it == got.end()) {
            wrong.push_back(std::string(r.function) + " missing");
            continue;
        }
        auto& f = it->second;
        if (f.status != r.status) {
            wrong.push_back(std::string(r.function) + " status " + f.status + " != " + r.status + " (" + f.message + ")");
            continue;
        }
        if (f.status == prism::laws::PROVED_ASSUMING && r.tag[0] && assumptions_of(f).find(r.tag) == std::string::npos)
            wrong.push_back(std::string(r.function) + " assumptions lack '" + r.tag + "': " + assumptions_of(f));
        if (f.status == prism::laws::PROVED && !assumptions_of(f).empty())
            wrong.push_back(std::string(r.function) + " PROVED with assumptions");
    }
    for (auto& w : wrong) MESSAGE(w);
    CHECK(wrong.empty());
}

}  // namespace

TEST_CASE("pir kernel: Linux API specs (allocation, locking, user access, MMIO, logging, panic)") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_linux.c", "linux", false,
                {{"alloc_ok", PA, "linux-alloc"},
                 {"alloc_nocheck_bad", F, ""},
                 {"zalloc_ok", PA, "__GFP_ZERO is bit 0x100"},
                 {"malloc_uninit_bad", F, ""},
                 {"double_free_bad", F, ""},
                 {"use_after_free_bad", F, ""},
                 {"oob_bad", F, ""},
                 {"kfree_null_ok", PA, "linux-free"},
                 {"krealloc_ok", PA, "linux-krealloc"},
                 {"kstrdup_ok", PA, "linux-alloc"},
                 {"vmalloc_ok", PA, "linux-alloc"},
                 {"lock_ok", PA, "linux-lock"},
                 {"lock_null_bad", F, ""},
                 {"trylock_ok", PA, "linux-lock"},
                 {"spin_irqsave_ok", PA, "linux-lock"},
                 {"copy_from_user_ok", PA, "linux-uaccess"},
                 {"copy_from_user_trust_bad", F, ""},
                 {"copy_from_user_oob_bad", F, ""},
                 {"copy_to_user_ok", PA, "linux-uaccess"},
                 {"copy_to_user_oob_bad", F, ""},
                 {"mmio_ok", PA, "linux-mmio"},
                 {"mmio_trust_bad", F, ""},
                 {"log_delay_ok", PA, "linux-log"},
                 {"log_delay_bad", F, ""},
                 {"panic_bad", F, ""},
                 {"panic_ok", PA, "linux-bug"},
                 {"container_of_ok", "PROVED", ""},
                 {"container_of_bad", F, ""}});
}

TEST_CASE("pir kernel: FreeBSD API specs (malloc, mtx, copyin/copyout, bus_space, logging, panic)") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_freebsd.c", "freebsd", false,
                {{"waitok_ok", PA, "M_WAITOK allocations succeed"},
                 {"nowait_nocheck_bad", F, ""},
                 {"nowait_ok", PA, "freebsd-alloc"},
                 {"waitok_uninit_bad", F, ""},
                 {"free_twice_bad", F, ""},
                 {"mtx_ok", PA, "freebsd-lock"},
                 {"mtx_null_bad", F, ""},
                 {"copyin_ok", PA, "freebsd-copy"},
                 {"copyin_trust_bad", F, ""},
                 {"copyin_oob_bad", F, ""},
                 {"copyout_oob_bad", F, ""},
                 {"bus_ok", PA, "freebsd-bus-space"},
                 {"bus_trust_bad", F, ""},
                 {"log_ok", PA, "freebsd-log"},
                 {"panic_bad", F, ""}});
}

TEST_CASE("pir kernel: lock balance, double lock, unlock of an unheld lock, sleeping in atomic context (ghost lock state)") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_lock_ctx.c", "linux", false,
                {{"lock_relock_ok", PA, "linux-lock"},
                 {"lock_double_bad", F, ""},
                 {"spin_double_bad", F, ""},
                 {"spin_nested_ok", PA, "linux-lock"},
                 {"unlock_unheld_bad", F, ""},
                 {"sleep_in_spin_bad", F, ""},
                 {"sleep_after_spin_ok", PA, "linux-lock"},
                 {"might_sleep_in_spin_bad", F, ""},
                 {"might_sleep_ok", PA, "linux-might-sleep"},
                 {"held_on_return_bad", F, ""},
                 {"held_on_return_ok", PA, "linux-lock"}});
    // each bad variant reports its own bug class, not another one
    auto got = run("kernel_lock_ctx.c", "linux", false);
    const std::map<std::string, std::string> want = {
        {"lock_double_bad", "LOCK-DOUBLE"},         {"spin_double_bad", "LOCK-DOUBLE"},
        {"unlock_unheld_bad", "LOCK-UNHELD-UNLOCK"}, {"sleep_in_spin_bad", "SLEEP-IN-ATOMIC"},
        {"held_on_return_bad", "LOCK-HELD-ON-RETURN"},
        {"might_sleep_in_spin_bad", "SLEEP-IN-ATOMIC"}};
    for (auto& [fn, cls] : want) {
        auto it = got.find(fn);
        REQUIRE(it != got.end());
        CHECK(it->second.cls == cls);
    }
}

TEST_CASE("pir kernel: lock contracts (__prism_lock_entry / __prism_lock_exit): a returned-held lock is not reported") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_lock_contract.c", "linux", false,
                {{"take_declared_ok", PA, "linux-lock-contract"},
                 {"take_undeclared_bad", F, ""},
                 {"take_exit_missing_bad", F, ""},
                 {"release_entry_ok", PA, "linux-lock-contract"},
                 {"entry_held_on_return_bad", F, ""},
                 {"pass_through_ok", PA, "linux-lock-contract"},
                 {"entry_spin_sleep_bad", F, ""},
                 {"caller_ok", PA, "linux-lock-overflow"}});
    auto got = run("kernel_lock_contract.c", "linux", false);
    const std::map<std::string, std::string> want = {
        {"take_undeclared_bad", "LOCK-HELD-ON-RETURN"},
        {"take_exit_missing_bad", "LOCK-EXIT-MISSING"},
        {"entry_held_on_return_bad", "LOCK-HELD-ON-RETURN"},
        {"entry_spin_sleep_bad", "SLEEP-IN-ATOMIC"}};
    for (auto& [fn, cls] : want) {
        auto it = got.find(fn);
        REQUIRE(it != got.end());
        CHECK(it->second.cls == cls);
    }
}

TEST_CASE("pir kernel: preempt and bh depth, sleeping in atomic context (SLEEP-IN-ATOMIC), GFP_KERNEL in a spinlock") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_lock_preempt.c", "linux", false,
                {{"preempt_sleep_bad", F, ""},
                 {"preempt_sleep_ok", PA, "linux-preempt"},
                 {"preempt_unbalanced_bad", F, ""},
                 {"preempt_held_on_return_bad", F, ""},
                 {"bh_sleep_bad", F, ""},
                 {"bh_sleep_ok", PA, "linux-preempt"},
                 {"kmalloc_in_spin_bad", F, ""},
                 {"kmalloc_atomic_ok", PA, "linux-alloc"},
                 {"kmalloc_kernel_ok", PA, "linux-alloc"}});
    auto got = run("kernel_lock_preempt.c", "linux", false);
    const std::map<std::string, std::string> want = {
        {"preempt_sleep_bad", "SLEEP-IN-ATOMIC"},
        {"preempt_unbalanced_bad", "PREEMPT-UNBALANCED"},
        {"preempt_held_on_return_bad", "PREEMPT-UNBALANCED"},
        {"bh_sleep_bad", "SLEEP-IN-ATOMIC"},
        {"kmalloc_in_spin_bad", "SLEEP-IN-ATOMIC"}};
    for (auto& [fn, cls] : want) {
        auto it = got.find(fn);
        REQUIRE(it != got.end());
        CHECK(it->second.cls == cls);
    }
}

TEST_CASE("pir kernel: RCU read-side nesting (RCU-UNBALANCED, RCU-HELD-ON-RETURN, RCU-SLEEP)") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_lock_rcu.c", "linux", false,
                {{"rcu_balance_ok", PA, "linux-rcu"},
                 {"rcu_unbalanced_bad", F, ""},
                 {"rcu_held_on_return_bad", F, ""},
                 {"rcu_sleep_bad", F, ""},
                 {"rcu_sleep_ok", PA, "linux-rcu"},
                 {"rcu_sync_bad", F, ""},
                 {"rcu_sync_ok", PA, "linux-rcu"}});
    auto got = run("kernel_lock_rcu.c", "linux", false);
    const std::map<std::string, std::string> want = {
        {"rcu_unbalanced_bad", "RCU-UNBALANCED"},
        {"rcu_held_on_return_bad", "RCU-HELD-ON-RETURN"},
        {"rcu_sleep_bad", "RCU-SLEEP"},
        {"rcu_sync_bad", "RCU-SLEEP"}};
    for (auto& [fn, cls] : want) {
        auto it = got.find(fn);
        REQUIRE(it != got.end());
        CHECK(it->second.cls == cls);
    }
}

TEST_CASE("pir kernel: inline asm, atomics, barriers and volatile are named assumptions") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_asm.c", "linux", false,
                {{"asm_out_ok", PA, "asm-havoc: \"=r,"},
                 {"asm_out_bad", F, ""},
                 {"asm_clobber_ok", PA, "memory clobber"},
                 {"asm_clobber_bad", F, ""},
                 {"asm_nomem_ok", PA, "asm-havoc"},
                 {"asm_indirect_out_bad", F, ""},
                 {"asm_ud2_bad", F, ""},
                 {"asm_ud2_ok", PA, "asm-havoc"},
                 {"atomic_add_ok", PA, "atomics-sequential: atomicrmw add"},
                 {"atomic_sub_bad", F, ""},
                 {"atomic_load_store_bad", F, ""},
                 {"cmpxchg_ok", PA, "atomics-sequential: cmpxchg"},
                 {"cmpxchg_fail_bad", F, ""},
                 {"cmpxchg_weak_bad", F, ""},
                 {"xchg_ok", PA, "atomicrmw xchg"},
                 {"fence_ok", PA, "atomics-sequential: fence"},
                 {"volatile_ok", PA, "volatile-access"},
                 {"volatile_bad", F, ""},
                 {"asm_goto_ok", PA, "asm-goto"},
                 {"asm_goto_bad", F, ""}});
}

TEST_CASE("pir kernel: a call without a spec is UNENCODED; --assume-unknown-calls is the opt-in and shows in the verdict") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* NH = "NEEDS-HARNESS";
    // default and kernel mode alone: unchanged, the call stops the function
    check_table("kernel_unknown.c", "", false,
                {{"unk_result_ok", NH, ""}, {"unk_result_bad", NH, ""}, {"unk_pointer_arg_ok", NH, ""}});
    check_table("kernel_unknown.c", "linux", false,
                {{"unk_result_ok", NH, ""}, {"unk_result_bad", NH, ""}, {"unk_ptr_result_ok", NH, ""}});
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_unknown.c", "linux", true,
                {{"unk_result_ok", PA, "unknown-call: @e1000e_poll_hw"},
                 {"unk_result_bad", F, ""},
                 {"unk_nopointer_keeps_locals_ok", PA, "unknown-call"},
                 {"unk_pointer_arg_ok", PA, "unknown-call: @e1000e_fill"},
                 {"unk_pointer_arg_bad", F, ""},
                 {"unk_literal_arg_ok", PA, "unknown-call: @e1000e_name"},
                 {"unk_ptr_result_ok", PA, "unknown-call: @e1000e_get_buf"},
                 {"unk_ptr_result_nocheck_bad", F, ""}});
    // the opt-in does not need the kernel specs
    check_table("kernel_unknown.c", "", true,
                {{"unk_result_ok", PA, "unknown-call: @e1000e_poll_hw"}, {"unk_result_bad", F, ""}});
}

TEST_CASE("pir kernel: without --kernel-api asm, atomics and kernel calls stay UNENCODED (the default is unchanged)") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* NH = "NEEDS-HARNESS";
    check_table("kernel_asm.c", "", false,
                {{"asm_out_ok", NH, ""}, {"asm_clobber_ok", NH, ""}, {"atomic_add_ok", NH, ""},
                 {"cmpxchg_ok", NH, ""}, {"fence_ok", NH, ""}, {"asm_goto_ok", NH, ""}});
    check_table("kernel_linux.c", "", false,
                {{"alloc_ok", NH, ""}, {"lock_ok", NH, ""}, {"mmio_ok", NH, ""}, {"container_of_ok", "PROVED", ""}});
}

// --lock-contracts (docs/KERNEL_CONCURRENCY.md section 14): table-driven contracts for code with no markers
namespace {
std::map<std::string, prism::Finding> run_with_table(const std::string& file, const std::string& tsv) {
    auto cfg = prism::default_config();
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    cfg.pir_kernel_api = "linux";
    if (!tsv.empty()) cfg.pir_lock_contracts = (dir / tsv).string();
    std::map<std::string, prism::Finding> m;
    for (auto& f : pp::run_pir({dir / file}, cfg))
        if (f.function) m[*f.function] = f;
    return m;
}
}  // namespace

TEST_CASE("pir kernel: --lock-contracts table: acquire and release halves are contracts, a wrong contract is reported") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    // without the table: the model artefacts (true variant)
    auto none = run_with_table("kernel_lock_table.c", "");
    REQUIRE(none.count("nvm_acquire_ok"));
    REQUIRE(none.count("nvm_release_ok"));
    CHECK(none["nvm_acquire_ok"].cls == "LOCK-HELD-ON-RETURN");
    CHECK(none["nvm_release_ok"].cls == "LOCK-UNHELD-UNLOCK");
    // with the table (false variant): no finding for the two halves, the contracts are assumed
    auto got = run_with_table("kernel_lock_table.c", "kernel_lock_table.tsv");
    REQUIRE(got.count("nvm_acquire_ok"));
    REQUIRE(got.count("nvm_release_ok"));
    CHECK(got["nvm_acquire_ok"].status == prism::laws::PROVED_ASSUMING);
    CHECK(assumptions_of(got["nvm_acquire_ok"]).find("lock-contract-table") != std::string::npos);
    CHECK(got["nvm_release_ok"].status == prism::laws::PROVED_ASSUMING);
    CHECK(assumptions_of(got["nvm_release_ok"]).find("lock-contract-table") != std::string::npos);
    // a wrong table row (acquires, but the function releases first): LOCK-EXIT-MISSING
    REQUIRE(got.count("wrong_contract_bad"));
    CHECK(got["wrong_contract_bad"].status == prism::laws::FAILED);
    CHECK(got["wrong_contract_bad"].cls == "LOCK-EXIT-MISSING");
    // a spin contract (kind spin): the mutex call while the caller's spinlock is held sleeps
    REQUIRE(got.count("spin_entry_sleep_bad"));
    CHECK(got["spin_entry_sleep_bad"].status == prism::laws::FAILED);
    CHECK(got["spin_entry_sleep_bad"].cls == "SLEEP-IN-ATOMIC");
}

TEST_CASE("pir kernel: --lock-contracts TSV parse: header and comments skipped, bad lines are errors") {
    std::string err;
    auto ok = pp::parse_lock_contracts("function\tlock\taction\n# c\nf\tglobal:m\tacquires\tspin\n", &err);
    CHECK(err.empty());
    REQUIRE(ok.size() == 1);
    CHECK(ok[0].function == "f");
    CHECK(ok[0].lock == "global:m");
    CHECK(ok[0].spin);
    auto bad = pp::parse_lock_contracts("f\tm\tacquires\n", &err);
    CHECK(bad.empty());
    CHECK(err.find("line 1") != std::string::npos);
    bad = pp::parse_lock_contracts("f\tglobal:m\ttakes\n", &err);
    CHECK(bad.empty());
    CHECK(err.find("action") != std::string::npos);
    bad = pp::parse_lock_contracts("f\targ:0\trequires-held\tfast\n", &err);
    CHECK(bad.empty());
    CHECK(err.find("kind") != std::string::npos);
}

// LOCK-ORDER-INVERSION (docs/KERNEL_CONCURRENCY.md section 14): a lock-class pair taken in both orders
TEST_CASE("pir kernel: lock order: a pair taken A-then-B and B-then-A is LOCK-ORDER-INVERSION, a consistent order is not") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    auto count_inversions = [&](const std::string& file, std::vector<prism::Finding>& keep) {
        cfg.root = dir;
        cfg.jobs = 2;
        cfg.pir_kernel_api = "linux";
        keep = pp::run_pir({dir / file}, cfg);
        int n = 0;
        for (auto& f : keep)
            if (f.cls == "LOCK-ORDER-INVERSION") ++n;
        return n;
    };
    std::vector<prism::Finding> t, f;
    CHECK(count_inversions("kernel_lock_order_true.c", t) == 1);
    for (auto& x : t)
        if (x.cls == "LOCK-ORDER-INVERSION") {
            CHECK(x.status == prism::laws::FAILED);
            CHECK(x.extra.count("lock_classes"));
            if (x.extra.count("lock_classes")) CHECK(x.extra.at("lock_classes") == "lock_a,lock_b");
            CHECK(x.function.has_value());
        }
    CHECK(count_inversions("kernel_lock_order_false.c", f) == 0);
}

// LOCK-ORDER-INVERSION over three classes (docs/KERNEL_CONCURRENCY.md section 14, W7c item 2): a cycle of length
// three is one finding on the function that closes it; a DAG is none
TEST_CASE("pir kernel: lock order: a cycle of three lock classes is one LOCK-ORDER-INVERSION, a DAG is none") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    auto cycles_in = [&](const std::string& file, std::vector<prism::Finding>& keep) {
        cfg.root = dir;
        cfg.jobs = 2;
        cfg.pir_kernel_api = "linux";
        keep = pp::run_pir({dir / file}, cfg);
        std::vector<prism::Finding> hits;
        for (auto& f : keep)
            if (f.cls == "LOCK-ORDER-INVERSION" && f.extra.count("cycle")) hits.push_back(f);
        return hits;
    };
    std::vector<prism::Finding> t, f;
    auto cyc = cycles_in("kernel_lock_cycle_true.c", t);
    REQUIRE(cyc.size() == 1);
    CHECK(cyc[0].status == prism::laws::FAILED);
    CHECK(cyc[0].extra.at("cycle") == "lock_a,lock_b,lock_c");
    CHECK(cyc[0].function.has_value());
    if (cyc[0].function) CHECK(*cyc[0].function == "f3");
    CHECK(cycles_in("kernel_lock_cycle_false.c", f).empty());
}

// Interrupt context (docs/KERNEL_CONCURRENCY.md section 15, W7c item 1): a function passed to request_irq runs in
// hardirq; a sleeping primitive there is SLEEP-IN-ATOMIC; a spinlock it takes that a process-context function
// takes without irq disabling is LOCK-CTX-MISMATCH. The thread function of request_threaded_irq is not hardirq.
TEST_CASE("pir kernel: interrupt context: hardirq sleeps and LOCK-CTX-MISMATCH, consistent handlers are clean") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    auto run_irq = [&](const std::string& file) {
        cfg.root = dir;
        cfg.jobs = 2;
        cfg.pir_kernel_api = "linux";
        return pp::run_pir({dir / file}, cfg);
    };
    auto t = run_irq("kernel_lock_irq_true.c");
    std::map<std::string, std::string> sleeps;  // function -> handler
    int mismatches = 0;
    for (auto& x : t) {
        if (x.cls == "SLEEP-IN-ATOMIC" && x.extra.count("context") && x.extra.at("context") == "hardirq" && x.function)
            sleeps[*x.function] = x.extra.count("other_function") ? x.extra.at("other_function") : "";
        if (x.cls == "LOCK-CTX-MISMATCH") {
            ++mismatches;
            CHECK(x.status == prism::laws::FAILED);
            if (x.function) CHECK(*x.function == "proc_dev_plain");
            if (x.extra.count("other_function")) CHECK(x.extra.at("other_function") == "irq_spin_handler");
        }
    }
    CHECK(mismatches == 1);
    CHECK(sleeps.size() == 2);
    CHECK(sleeps.count("irq_sleep_handler") == 1);
    CHECK(sleeps.count("irq_gfp_handler") == 1);
    auto f = run_irq("kernel_lock_irq_false.c");
    for (auto& x : f) {
        CHECK(x.cls != "LOCK-CTX-MISMATCH");
        CHECK(!(x.cls == "SLEEP-IN-ATOMIC" && x.extra.count("context")));
    }
}

// Struct-field lock classes (docs/KERNEL_CONCURRENCY.md section 16)
TEST_CASE("pir kernel: field lock classes: an inversion between two fields of one struct is LOCK-ORDER-INVERSION") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    cfg.pir_kernel_api = "linux";
    auto out = pp::run_pir({dir / "kernel_lock_field_order_true.c"}, cfg);
    std::vector<prism::Finding> hits;
    for (auto& x : out)
        if (x.cls == "LOCK-ORDER-INVERSION") hits.push_back(x);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].extra.count("cycle") == 0);
    CHECK(hits[0].status == prism::laws::FAILED);
    REQUIRE(hits[0].extra.count("lock_classes"));
    CHECK(hits[0].extra.at("lock_classes") == "dev.#0,dev.#1");
}

TEST_CASE("pir kernel: field lock classes: a consistent field order is not an inversion") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    cfg.pir_kernel_api = "linux";
    auto out = pp::run_pir({dir / "kernel_lock_field_order_false.c"}, cfg);
    int n = 0;
    for (auto& x : out)
        if (x.cls == "LOCK-ORDER-INVERSION") ++n;
    CHECK(n == 0);
}

TEST_CASE("pir kernel: field lock classes: a field lock taken in an irq handler and plainly in process context is LOCK-CTX-MISMATCH") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    cfg.pir_kernel_api = "linux";
    auto out = pp::run_pir({dir / "kernel_lock_field_irq_true.c"}, cfg);
    std::vector<prism::Finding> hits;
    for (auto& x : out)
        if (x.cls == "LOCK-CTX-MISMATCH") hits.push_back(x);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].status == prism::laws::FAILED);
    REQUIRE(hits[0].function.has_value());
    CHECK(*hits[0].function == "proc_plain");
    REQUIRE(hits[0].extra.count("other_function"));
    CHECK(hits[0].extra.at("other_function") == "irq_handler");
    REQUIRE(hits[0].extra.count("lock_classes"));
    CHECK(hits[0].extra.at("lock_classes") == "dev.#0");
}

TEST_CASE("pir kernel: field lock classes: a consistent irq discipline on a field lock is clean") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    cfg.pir_kernel_api = "linux";
    auto out = pp::run_pir({dir / "kernel_lock_field_irq_false.c"}, cfg);
    int n = 0;
    for (auto& x : out)
        if (x.cls == "LOCK-CTX-MISMATCH") ++n;
    CHECK(n == 0);
}

TEST_CASE("pir kernel: field lock classes: nesting two locks of the same field class is not an inversion") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    cfg.pir_kernel_api = "linux";
    auto out = pp::run_pir({dir / "kernel_lock_field_nest.c"}, cfg);
    int n = 0;
    for (auto& x : out)
        if (x.cls == "LOCK-ORDER-INVERSION") ++n;
    CHECK(n == 0);
}

TEST_CASE("pir kernel: field lock classes: the lock-contract table gives field classes to acquire and release halves") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    cfg.pir_kernel_api = "linux";
    auto count_inversions = [&](std::vector<prism::Finding>& keep) {
        keep = pp::run_pir({dir / "kernel_lock_field_table.c"}, cfg);
        int n = 0;
        for (auto& x : keep)
            if (x.cls == "LOCK-ORDER-INVERSION") ++n;
        return n;
    };
    std::vector<prism::Finding> without, with;
    cfg.pir_lock_contracts = "";
    CHECK(count_inversions(without) == 0);
    cfg.pir_lock_contracts = (dir / "kernel_lock_field_table.tsv").string();
    CHECK(count_inversions(with) == 1);
    for (auto& x : with)
        if (x.cls == "LOCK-ORDER-INVERSION") {
            REQUIRE(x.extra.count("lock_classes"));
            CHECK(x.extra.at("lock_classes") == "dev.#0,dev.#1");
        }
}

TEST_CASE("pir kernel: field lock classes: TSV parse of field:<struct>.<index> lock names") {
    std::string err;
    auto ok = pp::parse_lock_contracts("f\tfield:dev.0\tacquires\n", &err);
    CHECK(err.empty());
    REQUIRE(ok.size() == 1);
    CHECK(ok[0].lock == "field:dev.#0");
    err.clear();
    auto bad = pp::parse_lock_contracts("f\tfield:dev\tacquires\n", &err);
    CHECK(bad.empty());
    CHECK(err.find("field") != std::string::npos);
    err.clear();
    bad = pp::parse_lock_contracts("f\tfield:dev.name\tacquires\n", &err);
    CHECK(bad.empty());
    CHECK(err.find("index") != std::string::npos);
}
