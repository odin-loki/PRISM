// PIR bytecode VM and input sweep (docs/GPU_PLAN.md G1): the VM against
// pir::eval_op and pir::interpret, sweep refutation and exhaustion, and the
// GPU against the CPU twin when a GPU is there.
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir_vm.hpp"

#include <doctest/doctest.h>

#include <cstdio>
#include <random>

using namespace prism::pir;

namespace {

int var(Function& f, const char* name, unsigned w) {
    f.vars.push_back(Var{name, w, false});
    return static_cast<int>(f.vars.size()) - 1;
}
Stmt assign(int dst, Op op, std::vector<Arg> args) {
    Stmt s;
    s.kind = Stmt::Assign;
    s.dst = dst;
    s.op = op;
    s.args = std::move(args);
    return s;
}
Stmt check(int cond, const char* prop, int line) {
    Stmt s;
    s.kind = Stmt::Check;
    s.args = {Arg::v(cond, 1)};
    s.prop = prop;
    s.cls = "TEST-CLASS";
    s.msg = "test property";
    s.line = line;
    return s;
}
Stmt assume(int cond) {
    Stmt s;
    s.kind = Stmt::Assume;
    s.args = {Arg::v(cond, 1)};
    return s;
}
Term ret() {
    Term t;
    t.kind = Term::Ret;
    return t;
}

uint32_t status_of(const InterpResult& r) {
    switch (r.status) {
        case InterpResult::Returned: return 0;
        case InterpResult::Stopped: return 1;
        case InterpResult::Violation: return 2;
        case InterpResult::AssumeFailed: return 3;
        default: return 4;
    }
}

// f(x, y) = op(x, y) at width w, then check(result == expected): a violation
// exactly when the VM computes what eval_op computes.
Function op_probe(Op op, unsigned w, unsigned rw, uint64_t expected) {
    Function f;
    f.name = "probe";
    int x = var(f, "x", w), y = var(f, "y", w), r = var(f, "r", rw), c = var(f, "c", 1);
    f.params = {x, y};
    Block b;
    b.stmts.push_back(assign(r, op, {Arg::v(x, w), Arg::v(y, w)}));
    b.stmts.push_back(assign(c, Op::Eq, {Arg::v(r, rw), Arg::c(rw, expected)}));
    b.stmts.push_back(check(c, "probe", 1));
    b.term = ret();
    f.blocks.push_back(b);
    return f;
}

}  // namespace

TEST_CASE("pir vm: every integer operator agrees with eval_op") {
    const Op bin[] = {Op::Add, Op::Sub, Op::Mul, Op::UDiv, Op::SDiv, Op::URem, Op::SRem, Op::Shl, Op::LShr,
                      Op::AShr, Op::And, Op::Or, Op::Xor, Op::SMax, Op::SMin, Op::UMax, Op::UMin, Op::Abs,
                      Op::Ctlz, Op::Cttz, Op::Ctpop, Op::Copy, Op::Trunc};
    const Op pred[] = {Op::Eq, Op::Ne, Op::Ult, Op::Ule, Op::Ugt, Op::Uge, Op::Slt, Op::Sle, Op::Sgt, Op::Sge,
                       Op::SAddOvf, Op::SSubOvf, Op::SMulOvf, Op::UAddOvf, Op::USubOvf, Op::UMulOvf,
                       Op::SDivOvf, Op::ShiftOob, Op::ShlSOvf, Op::ShlNswOvf, Op::ShlNuwOvf, Op::LostBitsL,
                       Op::LostBitsA, Op::InexactU, Op::InexactS};
    std::mt19937_64 rng(7);
    int checked = 0;
    for (unsigned w : {1u, 8u, 13u, 16u, 32u, 33u, 63u, 64u}) {
        const uint64_t m = w >= 64 ? ~uint64_t{0} : (uint64_t{1} << w) - 1;
        const uint64_t smin = (uint64_t{1} << (w - 1)) & m;
        std::vector<uint64_t> vals = {0, 1, 2, m, m - 1, smin, smin - 1, smin + 1};
        for (int k = 0; k < 6; ++k) vals.push_back(rng());
        for (auto& v : vals) v &= m;
        auto run = [&](Op op, unsigned rw) {
            for (uint64_t x : vals)
                for (uint64_t y : vals) {
                    if ((op == Op::InexactU || op == Op::InexactS) && y == 0) continue;  // eval_op divides first
                    const uint64_t want = eval_op(op, rw, {x, y}, {w, w}) & (rw >= 64 ? ~0ull : (1ull << rw) - 1);
                    auto f = op_probe(op, w, rw, want);
                    std::string why;
                    auto p = vm::compile(f, {}, &why);
                    REQUIRE_MESSAGE(p.has_value(), why);
                    uint32_t prop = 9;
                    const auto st = vm::run_one(*p, {x, y}, 1000, &prop);
                    INFO(op_name(op), " w=", w, " x=", x, " y=", y, " want=", want);
                    CHECK(st == 2u);
                    // and the interpreter agrees on the same program
                    CHECK(status_of(interpret(f, {x, y})) == st);
                    ++checked;
                }
        };
        for (Op op : bin) run(op, w);
        for (Op op : pred) run(op, 1);
    }
    CHECK(checked > 10000);
}

namespace {

// sum = 0; for (i = 0; i != n; ++i) { check(sum + i overflows u8-signed); sum += i; } return
// with an assume n <= 40: a loop with phis, a back edge and a branch.
Function loop_fn() {
    Function f;
    f.name = "loop";
    int n = var(f, "n", 8), lim = var(f, "lim", 1);
    int i = var(f, "i", 8), s = var(f, "s", 8), i2 = var(f, "i2", 8), s2 = var(f, "s2", 8);
    int done = var(f, "done", 1), ov = var(f, "ov", 1);
    f.params = {n};
    Block entry;  // 0
    entry.stmts.push_back(assign(lim, Op::Ule, {Arg::v(n, 8), Arg::c(8, 40)}));
    entry.stmts.push_back(assume(lim));
    entry.term.kind = Term::Jmp;
    entry.term.t = 1;
    Block head;  // 1
    Phi pi;
    pi.dst = i;
    pi.in = {{0, Arg::c(8, 0)}, {2, Arg::v(i2, 8)}};
    Phi ps;
    ps.dst = s;
    ps.in = {{0, Arg::c(8, 0)}, {2, Arg::v(s2, 8)}};
    head.phis = {pi, ps};
    head.stmts.push_back(assign(done, Op::Eq, {Arg::v(i, 8), Arg::v(n, 8)}));
    head.term.kind = Term::Br;
    head.term.cond = Arg::v(done, 1);
    head.term.t = 3;
    head.term.f = 2;
    Block body;  // 2
    body.stmts.push_back(assign(ov, Op::SAddOvf, {Arg::v(s, 8), Arg::v(i, 8)}));
    body.stmts.push_back(check(ov, "ovf+", 7));
    body.stmts.push_back(assign(s2, Op::Add, {Arg::v(s, 8), Arg::v(i, 8)}));
    body.stmts.push_back(assign(i2, Op::Add, {Arg::v(i, 8), Arg::c(8, 1)}));
    body.term.kind = Term::Jmp;
    body.term.t = 1;
    Block exit;  // 3
    exit.term = ret();
    f.blocks = {entry, head, body, exit};
    return f;
}

}  // namespace

TEST_CASE("pir vm: a loop with phis matches the interpreter on every input") {
    auto f = loop_fn();
    std::string why;
    auto p = vm::compile(f, {}, &why);
    REQUIRE_MESSAGE(p.has_value(), why);
    int viol = 0;
    for (uint64_t n = 0; n < 256; ++n) {
        uint32_t prop = 0;
        const auto st = vm::run_one(*p, {n}, 100000, &prop);
        auto ir = interpret(f, {n});
        INFO("n=", n);
        CHECK(st == status_of(ir));
        viol += st == 2;
    }
    CHECK(viol > 0);  // sum 0..15 = 120 fits, 0..16 overflows i8: n in [17, 40] violate
}

TEST_CASE("pir vm: the sweep refutes, replays, and exhausts small domains") {
    auto f = loop_fn();
    std::string why;
    auto p = vm::compile(f, {}, &why);
    REQUIRE(p.has_value());
    vm::SweepOptions o;
    o.device = vm::Device::Cpu;
    auto r = vm::sweep(*p, o);
    CHECK(r.ran);
    REQUIRE(r.first_violation.has_value());
    CHECK(*r.first_violation == 17);  // exhaustive over 8 bits: the smallest failing n
    CHECK(r.args == std::vector<uint64_t>{17});
    CHECK(vm::confirm(f, {}, *p, r));
    // filtered out by --pir-props: nothing to check
    auto q = vm::compile(f, {"div0"}, &why);
    REQUIRE(q.has_value());
    CHECK(q->props.empty());

    // a 16-bit function no input violates: every input runs, none fails
    Function g;
    g.name = "safe";
    int x = var(g, "x", 16), z = var(g, "z", 32), c = var(g, "c", 1);
    g.params = {x};
    Block b;
    b.stmts.push_back(assign(z, Op::ZExt, {Arg::v(x, 16)}));
    b.stmts.push_back(assign(c, Op::UAddOvf, {Arg::v(z, 32), Arg::c(32, 1)}));
    b.stmts.push_back(check(c, "ovf+", 3));
    b.term = ret();
    g.blocks.push_back(b);
    auto pg = vm::compile(g, {}, &why);
    REQUIRE(pg.has_value());
    auto rg = vm::sweep(*pg, o);
    CHECK(rg.exhaustive);
    CHECK_FALSE(rg.first_violation.has_value());
    CHECK(rg.lanes == 65536);
    CHECK(rg.counts[0] == 65536);
    CHECK(rg.summary().find("never a proof") != std::string::npos);
}

TEST_CASE("pir vm: functions outside the fragment are refused with a reason") {
    Function f = loop_fn();
    f.uses_memory = true;
    std::string why;
    CHECK_FALSE(vm::compile(f, {}, &why).has_value());
    CHECK(why.find("memory") != std::string::npos);
    Function h = loop_fn();
    h.blocks[2].stmts.insert(h.blocks[2].stmts.begin(), assign(4, Op::Havoc, {}));
    CHECK_FALSE(vm::compile(h, {}, &why).has_value());
    CHECK(why.find("havoc") != std::string::npos);
    CHECK(vm::device_from_string("off") == vm::Device::Off);
    CHECK(vm::device_from_string("cuda") == vm::Device::Gpu);
}

TEST_CASE("pir vm: the GPU kernel equals the CPU twin lane for lane") {
    const auto gi = vm::gpu_info();
    if (!gi.available) {
        MESSAGE("GPU not available (", gi.detail, "): GPU equivalence not checked");
        return;
    }
    MESSAGE("GPU: ", gi.detail);
    // exhaustive and sampled, a refuting and a clean program
    auto f = loop_fn();
    std::string why;
    auto p = vm::compile(f, {}, &why);
    REQUIRE(p.has_value());
    for (unsigned ex : {32u, 1u}) {  // 1: 8 input bits > 1, so the sweep samples
        vm::SweepOptions o;
        o.exhaustive_bits = ex;
        o.samples = 1u << 20;
        o.device = vm::Device::Gpu;
        auto g = vm::sweep(*p, o);
        o.device = vm::Device::Cpu;
        auto c = vm::sweep(*p, o);
        CHECK(g.engine.rfind("gpu", 0) == 0);
        CHECK(g.first_violation == c.first_violation);
        CHECK(g.args == c.args);
        CHECK(g.prop == c.prop);
        CHECK(g.exhaustive == c.exhaustive);
    }
    // 2^24 exhaustive inputs, every one clean: GPU and CPU counts agree
    Function g;
    g.name = "wide";
    int x = var(g, "x", 24), z = var(g, "z", 64), m = var(g, "m", 64), c = var(g, "c", 1);
    g.params = {x};
    Block b;
    b.stmts.push_back(assign(z, Op::ZExt, {Arg::v(x, 24)}));
    b.stmts.push_back(assign(m, Op::Mul, {Arg::v(z, 64), Arg::v(z, 64)}));
    b.stmts.push_back(assign(c, Op::UMulOvf, {Arg::v(m, 64), Arg::c(64, 4096)}));
    b.stmts.push_back(check(c, "ovf*", 4));
    b.term = ret();
    g.blocks.push_back(b);
    auto pg = vm::compile(g, {}, &why);
    REQUIRE(pg.has_value());
    vm::SweepOptions o;
    o.budget_s = 60;
    o.exhaustive_bits = 24;  // the CPU twin's default cap is 20 bits
    o.device = vm::Device::Gpu;
    auto rg = vm::sweep(*pg, o);
    o.device = vm::Device::Cpu;
    auto rc = vm::sweep(*pg, o);
    CHECK(rg.exhaustive);
    CHECK(rc.exhaustive);
    CHECK(rg.first_violation == rc.first_violation);
    for (int s = 0; s < 6; ++s) CHECK(rg.counts[s] == rc.counts[s]);
    std::printf("pir vm 2^24 lanes: %s | %s\n", rg.summary().c_str(), rc.summary().c_str());
}
