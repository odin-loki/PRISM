// Certified mode end to end (roadmap 3.2): the pir stage routes every VC
// through prism::solver::solve; with --certified a function whose VCs all
// carry a CaDiCaL LRAT proof accepted by cake_lpr is PROVED-CERTIFIED.
// Also: taxonomy/confidence credit pir like bmc, the shipped trusted base and
// verdict links in report.md, and the SARIF trustedBase property.
//
// Tests that need CaDiCaL / cake_lpr find them the way solve() does
// (~/.prism/tools/<name>/<sha>/ then PATH); without them they assert the
// honest fallback (PROVED plus a certify_note) instead.

#include <doctest/doctest.h>

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/models.hpp"
#include "prism/pipeline.hpp"
#include "prism/pir.hpp"
#include "prism/shipdocs.hpp"
#include "prism/taxonomy.hpp"
#include "prism/verdict.hpp"

#ifdef PRISM_HAS_Z3
#  include "prism/solver.hpp"
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;

struct CertTmp {
    fs::path dir;
    CertTmp() {
        static int n = 0;
        dir = fs::temp_directory_path() /
              ("prism-cert-test-" + std::to_string(std::random_device{}()) + "-" + std::to_string(n++));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~CertTmp() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

fs::path repo_root() { return fs::path(__FILE__).parent_path().parent_path().parent_path(); }

prism::pir::Translation cert_pir(const std::string& ir, const std::string& fn) {
    auto m = prism::pir::ir::parse_module(ir);
    const auto* f = m.find(fn);
    REQUIRE(f != nullptr);
    return prism::pir::translate(m, *f);
}

// Loop-free, three properties (add nsw, sdiv by zero, sdiv INT_MIN/-1), all safe.
const char* kSafeDiv = R"IR(
define i32 @g(i32 %a, i32 %b) {
entry:
  %m = and i32 %b, 7
  %d = add nsw i32 %m, 1
  %q = sdiv i32 %a, %d
  ret i32 %q
}
)IR";

// Signed overflow reachable: FAILED, whatever the mode.
const char* kOvf = "define i32 @f(i32 %a, i32 %b) {\nentry:\n  %s = add nsw i32 %a, %b\n  ret i32 %s\n}\n";

// A loop that closes within unwind 8 (3 iterations): property VCs + unwinding assertion.
const char* kThree = R"IR(
define i32 @three(i32 %x) {
entry:
  br label %for.cond

for.cond:
  %i.0 = phi i32 [ 0, %entry ], [ %inc, %for.body ]
  %acc = phi i32 [ 0, %entry ], [ %a2, %for.body ]
  %cmp = icmp slt i32 %i.0, 3
  br i1 %cmp, label %for.body, label %for.end

for.body:
  %a2 = add nsw i32 %acc, 1
  %inc = add nsw i32 %i.0, 1
  br label %for.cond

for.end:
  %acc.lcssa = phi i32 [ %acc, %for.cond ]
  ret i32 %acc.lcssa
}
)IR";

#ifdef PRISM_HAS_Z3
bool have_cert_chain() {
    prism::solver::SolveOptions o;
    return prism::solver::find_tool("cadical", o) && prism::solver::find_tool("cake_lpr", o);
}

prism::pir::CheckOptions cert_opts(bool certified) {
    prism::pir::CheckOptions o;
    o.unwind = 8;
    o.timeout_s = 30;
    o.certified = certified;
    o.use_cache = false;
    return o;
}

std::size_t count_char(const std::string& s, char c) { return static_cast<std::size_t>(std::count(s.begin(), s.end(), c)); }
#endif

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("certified: loop-free safe function is PROVED-CERTIFIED with one certificate per VC") {
    auto t = cert_pir(kSafeDiv, "g");
    REQUIRE(t.fn.has_value());
    auto plain = prism::pir::check_function(*t.fn, cert_opts(false));
    CHECK(plain.status == prism::laws::PROVED);
    CHECK(plain.extra.at("certified_mode") == "off");
    CHECK_FALSE(plain.extra.contains("certificate"));
    CHECK(plain.extra.at("solver").find("VCs") != std::string::npos);

    auto po = cert_opts(true);
    po.certify_combined = false;  // the per-VC path (the combined one is tested below)
    auto v = prism::pir::check_function(*t.fn, po);
    const auto nprops = std::stoul(v.extra.at("properties"));
    REQUIRE(nprops >= 2);
    if (!have_cert_chain()) {
        MESSAGE("cadical/cake_lpr not installed: asserting the fallback");
        CHECK(v.status == prism::laws::PROVED);
        CHECK(v.extra.at("certify_note").find("not certified") != std::string::npos);
        CHECK_FALSE(v.extra.contains("certificate"));
        return;
    }
    CHECK(v.status == prism::laws::PROVED_CERTIFIED);
    CHECK(v.extra.at("certificate") == "checked");
    CHECK(v.extra.at("certificate_info").find("cake_lpr") != std::string::npos);
    CHECK(v.extra.at("certificate_info").find(std::to_string(nprops) + " VCs") == 0);
    // one CNF hash per VC (loop-free: property VCs only)
    const auto& shas = v.extra.at("cnf_sha256");
    CHECK(count_char(shas, ',') + 1 == nprops);
    CHECK(shas.size() == nprops * 64 + (nprops - 1));
    CHECK_FALSE(v.extra.contains("certify_note"));
    CHECK(v.extra.at("certificate_scope") == "per-vc");
    CHECK(v.extra.at("certificate_proofs") == std::to_string(nprops));
    // The verdict audit admits it from pir (a solver stage) with the certificate...
    auto admitted = prism::verdict::audit("pir", prism::verdict::Verdict::ProvedCertified, true);
    CHECK_FALSE(admitted.violation);
    // ... and never without one, nor from a stage that is not a solver.
    CHECK(prism::verdict::audit("pir", prism::verdict::Verdict::ProvedCertified, false).violation);
    CHECK(prism::verdict::audit("repair", prism::verdict::Verdict::ProvedCertified, true).violation);
}

TEST_CASE("certified: a closing loop needs the unwinding assertion certified too") {
    auto t = cert_pir(kThree, "three");
    REQUIRE(t.fn.has_value());
    auto po = cert_opts(true);
    po.certify_combined = false;
    auto v = prism::pir::check_function(*t.fn, po);
    CHECK(v.extra.at("loops") == "1");
    if (!have_cert_chain()) {
        CHECK(v.status == prism::laws::PROVED);
        return;
    }
    REQUIRE(v.status == prism::laws::PROVED_CERTIFIED);
    const auto nprops = std::stoul(v.extra.at("properties"));
    // properties + the unwinding assertion, each with its own CNF
    CHECK(count_char(v.extra.at("cnf_sha256"), ',') + 1 == nprops + 1);
    CHECK(v.extra.at("certificate_info").find("unwind: ") != std::string::npos);
}

TEST_CASE("certified: one combined certificate covers every VC of a function") {
    auto t = cert_pir(kSafeDiv, "g");
    REQUIRE(t.fn.has_value());
    auto v = prism::pir::check_function(*t.fn, cert_opts(true));
    const auto nprops = std::stoul(v.extra.at("properties"));
    REQUIRE(nprops >= 2);
    if (!have_cert_chain()) {
        CHECK(v.status == prism::laws::PROVED);
        CHECK(v.extra.at("certify_note").find("not certified") != std::string::npos);
        CHECK_FALSE(v.extra.contains("certificate"));
        return;
    }
    CAPTURE(v.extra.count("certificate_combined") ? v.extra.at("certificate_combined") : std::string());
    REQUIRE(v.status == prism::laws::PROVED_CERTIFIED);
    CHECK(v.extra.at("certificate") == "checked");
    CHECK(v.extra.at("certificate_scope") == "combined");
    CHECK(v.extra.at("certificate_proofs") == "1");
    // the one certificate says how many VCs it covers, and which
    CHECK(v.extra.at("certificate_vcs") == std::to_string(nprops));
    CHECK(count_char(v.extra.at("certificate_covers"), ',') + 1 == nprops);
    CHECK(v.extra.at("certificate_info").find(std::to_string(nprops) + " VCs, one LRAT proof") == 0);
    CHECK(v.extra.at("certificate_info").find("cake_lpr") != std::string::npos);
    CHECK(v.extra.at("cnf_sha256").size() == 64);  // one CNF
    CHECK(v.extra.at("certificate_bitblast").find("/1 lean-proved") == 1);
    CHECK_FALSE(v.extra.contains("certify_note"));
    CHECK(v.extra.at("solver").find("1 VCs") == 0);  // one query answered the function

    // A loop that closes: the unwinding assertion is one of the covered VCs.
    auto l = cert_pir(kThree, "three");
    auto vl = prism::pir::check_function(*l.fn, cert_opts(true));
    REQUIRE(vl.status == prism::laws::PROVED_CERTIFIED);
    CHECK(vl.extra.at("unwind_closed") == "true");
    CHECK(vl.extra.at("certificate_covers").find("unwind") != std::string::npos);
    CHECK(vl.extra.at("certificate_vcs") == std::to_string(std::stoul(vl.extra.at("properties")) + 1));
}

TEST_CASE("certified: a combined query that is not certified falls back to one certificate per VC") {
    // SAT: the combined query is violated, the per-VC queries find which
    // property (and its counterexample); nothing is certified.
    auto o = cert_pir(kOvf, "f");
    auto v = prism::pir::check_function(*o.fn, cert_opts(true));
    CHECK(v.status == prism::laws::FAILED);
    CHECK_FALSE(v.extra.contains("certificate"));
    if (std::stoul(v.extra.at("properties")) >= 2) {
        REQUIRE(v.extra.count("certificate_combined") == 1);
        CHECK(v.extra.at("certificate_combined").find("not obtained (sat)") != std::string::npos);
    }
    // BOUNDED (the loop does not close at unwind 2): the combined query is
    // SAT through the unwinding assertion; the per-VC path decides BOUNDED.
    auto t = cert_pir(kThree, "three");
    auto co = cert_opts(true);
    co.unwind = 2;
    auto b = prism::pir::check_function(*t.fn, co);
    CHECK((b.status == prism::laws::BOUNDED || b.status == prism::laws::PROVED_UNBOUNDED));
    CHECK_FALSE(b.extra.contains("certificate"));
    REQUIRE(b.extra.count("certificate_combined") == 1);
    CHECK(b.extra.at("certificate_combined").find("one certificate per VC instead") != std::string::npos);
}

TEST_CASE("certified: a combined proof the checker cannot finish is split into certified batches") {
    prism::solver::SolveOptions so;
    auto cad = prism::solver::find_tool("cadical", so);
    auto cake = prism::solver::find_tool("cake_lpr", so);
    if (!cad || !cake) {
        MESSAGE("cadical/cake_lpr not installed: skipped");
        return;
    }
    CertTmp tmp;
    // cake_lpr that runs out of time on its first proof (the combined one)
    // and is the real checker afterwards.
    auto cdir = tmp.dir / "tools" / "cadical" / "test" / "bin";
    auto kdir = tmp.dir / "tools" / "cake_lpr" / "test" / "bin";
    fs::create_directories(cdir);
    fs::create_directories(kdir);
    fs::create_symlink(cad->path, cdir / "cadical");
    const auto mark = tmp.dir / "first-call-done";
    {
        std::ofstream(kdir / "cake_lpr") << "#!/bin/sh\nif [ ! -e '" << mark.string() << "' ]; then touch '"
                                         << mark.string() << "'; exec sleep 30; fi\nexec '" << cake->path.string()
                                         << "' \"$@\"\n";
    }
    fs::permissions(kdir / "cake_lpr", fs::perms::owner_all);
    const std::string ir = R"IR(
define i32 @h(i32 %a, i32 %b) {
entry:
  %m = and i32 %b, 7
  %d = add nsw i32 %m, 1
  %q = sdiv i32 %a, %d
  %r = srem i32 %a, %d
  %x = xor i32 %q, %r
  ret i32 %x
}
)IR";
    auto t = cert_pir(ir, "h");
    REQUIRE(t.fn.has_value());
    auto o = cert_opts(true);
    o.tool_dirs = {(tmp.dir / "tools").string()};
    o.search_default_tools = false;
    o.check_timeout_s = 3;
    auto v = prism::pir::check_function(*t.fn, o);
    const auto nvc = std::stoul(v.extra.at("properties"));
    REQUIRE(nvc >= 4);
    CAPTURE(v.extra.count("certificate_combined") ? v.extra.at("certificate_combined") : std::string());
    REQUIRE(v.status == prism::laws::PROVED_CERTIFIED);
    CHECK(fs::exists(mark));
    CHECK(v.extra.at("certificate_scope") == "batched");
    const auto np = std::stoul(v.extra.at("certificate_proofs"));
    CHECK(np >= 2);
    CHECK(np < nvc);
    // the batches cover every VC, each exactly once
    CHECK(v.extra.at("certificate_vcs") == std::to_string(nvc));
    const auto& cov = v.extra.at("certificate_covers");
    CHECK(count_char(cov, '[') == np);
    CHECK(count_char(cov, ',') + np == nvc);
    CHECK(count_char(v.extra.at("cnf_sha256"), ',') + 1 == np);
    CHECK(v.extra.at("certificate_info").find(std::to_string(np) + " LRAT proofs") != std::string::npos);
}

TEST_CASE("certified: FAILED keeps a replayable counterexample; BOUNDED and PROVED-UNBOUNDED are not certified") {
    auto o = cert_pir(kOvf, "f");
    auto v = prism::pir::check_function(*o.fn, cert_opts(true));
    CHECK(v.status == prism::laws::FAILED);
    CHECK(v.cls == "INT-SIGNED-OVF");
    REQUIRE(v.cex_args.size() == 2);
    auto r = prism::pir::interpret(*o.fn, v.cex_args);  // the model is the solver library's, validated
    CHECK(r.status == prism::pir::InterpResult::Violation);
    CHECK_FALSE(v.extra.contains("certificate"));

    // unwind 2 cannot close a 3-iteration loop: never PROVED-CERTIFIED
    auto t = cert_pir(kThree, "three");
    auto co = cert_opts(true);
    co.unwind = 2;
    auto b = prism::pir::check_function(*t.fn, co);
    CHECK((b.status == prism::laws::BOUNDED || b.status == prism::laws::PROVED_UNBOUNDED));
    CHECK(b.extra.at("certify_note").find("not certified") != std::string::npos);
    CHECK_FALSE(b.extra.contains("certificate"));
}

TEST_CASE("certified: a checker that rejects leaves PROVED with the reason") {
    prism::solver::SolveOptions so;
    auto cad = prism::solver::find_tool("cadical", so);
    if (!cad) {
        MESSAGE("cadical not installed: skipped");
        return;
    }
    CertTmp tmp;
    // tool dir with the real CaDiCaL and a cake_lpr that rejects every proof
    auto cdir = tmp.dir / "tools" / "cadical" / "test" / "bin";
    auto kdir = tmp.dir / "tools" / "cake_lpr" / "test" / "bin";
    fs::create_directories(cdir);
    fs::create_directories(kdir);
    fs::create_symlink(cad->path, cdir / "cadical");
    {
        std::ofstream(kdir / "cake_lpr") << "#!/bin/sh\necho 's REJECTED'\nexit 0\n";
    }
    fs::permissions(kdir / "cake_lpr", fs::perms::owner_all);
    auto t = cert_pir(kSafeDiv, "g");
    auto o = cert_opts(true);
    o.tool_dirs = {(tmp.dir / "tools").string()};
    o.search_default_tools = false;
    auto v = prism::pir::check_function(*t.fn, o);
    CHECK(v.status == prism::laws::PROVED);
    CHECK_FALSE(v.extra.contains("certificate"));
    CHECK(v.extra.at("certify_note").find("cake_lpr") != std::string::npos);
    CHECK(v.extra.at("certify_note").find("verdict stays PROVED") != std::string::npos);
    // the combined certificate was rejected too, and the record says so
    REQUIRE(v.extra.count("certificate_combined") == 1);
    CHECK(v.extra.at("certificate_combined").find("cake_lpr") != std::string::npos);
    CHECK(v.extra.at("certificate_combined").find("one certificate per VC instead") != std::string::npos);
}

TEST_CASE("certified: the pir stage end to end on tests/pir (skips without clang/opt)") {
    auto cfg = prism::default_config();
    auto fe = prism::pir::find_frontend(cfg);
    if (!fe.clang || !fe.opt) {
        MESSAGE("clang/opt not on PATH: skipped");
        return;
    }
    CertTmp tmp;
    auto dir = repo_root() / "tests" / "pir";
    cfg.root = dir;
    cfg.jobs = 1;
    cfg.certified = true;
    cfg.solver_cache = tmp.dir / "cache";
    auto out = prism::pir::run_pir({dir / "overflow.c"}, cfg);
    std::map<std::string, const prism::Finding*> by;
    for (auto& f : out)
        if (f.function) by[*f.function] = &f;
    REQUIRE(by.contains("add_ok"));
    REQUIRE(by.contains("add_bad"));
    CHECK(by["add_bad"]->status == prism::laws::FAILED);
    if (have_cert_chain()) {
        CHECK(by["add_ok"]->status == prism::laws::PROVED_CERTIFIED);
        CHECK(by["add_ok"]->extra.at("certificate") == "checked");
    } else {
        CHECK(by["add_ok"]->status == prism::laws::PROVED);
    }
    // --solver-cache: the cache (and the solve-time history) lives there
    CHECK(fs::exists(tmp.dir / "cache" / "solve_times.json"));
    // A second run: with the certificate chain it answers from the cache
    // (certified entries are re-checked by cake_lpr). Without it the cached
    // answers are plain unsats, which a certified request never takes as
    // they are (portfolio.cpp: "solving again for a certificate"), so it
    // solves again and still never claims a certificate.
    auto again = prism::pir::run_pir({dir / "overflow.c"}, cfg);
    for (auto& f : again)
        if (f.function && *f.function == "add_ok") {
            CHECK(f.status == by["add_ok"]->status);
            if (have_cert_chain())
                CHECK(f.extra.at("solver").find("cache hits 0") == std::string::npos);
            else
                CHECK(f.status != std::string(prism::laws::PROVED_CERTIFIED));
        }
}
#endif

TEST_CASE("taxonomy and confidence credit pir like bmc") {
    prism::RunReport r;
    prism::FunctionInfo fi;
    fi.name = "f";
    fi.file = "a.c";
    fi.kind = "SCALAR";
    r.functions = {fi};
    prism::StageResult pir;
    pir.name = "pir";
    pir.status = "ok";
    prism::Finding f{"pir", std::string(prism::laws::PROVED_CERTIFIED), "a.c", std::string("f"), 1, "", "",
                     std::string(prism::laws::STRENGTH_PROVES)};
    f.extra["certificate"] = "checked";
    pir.findings = {f};
    r.stages = {pir};
    auto rows = prism::coverage_from_report(r);
    std::map<std::string, std::string> v;
    for (auto& row : rows) v[row.id] = row.verdict;
    for (auto* id : {"INT-SIGNED-OVF", "INT-DIV-ZERO", "INT-SHIFT-UB", "UNINIT-READ", "INT-CLZ-ZERO"})
        CHECK(v[id] == "COVERED");
    CHECK(v["MEM-OOB-READ"] != "COVERED");  // no memory model in PIR
    bool pir_seen = false;
    for (auto& c : prism::taxonomy_classes())
        if (c.id == "INT-SIGNED-OVF")
            for (auto& [inst, how] : c.seen)
                if (inst == "pir" && how == "PROVES") pir_seen = true;
    CHECK(pir_seen);
    prism::apply_confidence(r);
    CHECK(r.answer == doctest::Approx(1.0));
    CHECK(r.resolution == doctest::Approx(1.0));
    CHECK(r.confidence == doctest::Approx(1.0));
}

TEST_CASE("reports ship the trusted base and link every verdict to its definition") {
    CertTmp tmp;
    // every lattice verdict has its anchor in the shipped VERDICTS.md
    std::string_view verdicts;
    for (auto& d : prism::shipped_docs())
        if (d.name == prism::VERDICTS_FILE) verdicts = d.text;
    REQUIRE_FALSE(verdicts.empty());
    for (auto v : prism::verdict::all_verdicts()) {
        auto a = prism::verdict_anchor(prism::verdict::name(v));
        REQUIRE_FALSE(a.empty());
        CHECK(verdicts.find("id=\"" + a + "\"") != std::string_view::npos);
    }
    CHECK(prism::verdict_anchor("ok").empty());
    // the embedded copies are byte-identical to docs/
    CHECK(prism::trusted_base_text() == slurp(repo_root() / "docs" / "TRUSTED_BASE.md"));
    CHECK(verdicts == slurp(repo_root() / "docs" / "VERDICTS.md"));

    prism::RunReport r;
    r.root = tmp.dir.string();
    prism::StageResult s;
    s.name = "pir";
    s.status = "ok";
    s.findings = {{"pir", std::string(prism::laws::PROVED), "a.c", std::string("f"), 1, "", "ok",
                   std::string(prism::laws::STRENGTH_PROVES)}};
    r.stages = {s};
    prism::write_report_md(r, tmp.dir / "report.md");
    prism::write_sarif(r, tmp.dir / "report.sarif");
    auto md = slurp(tmp.dir / "report.md");
    CHECK(md.find("[TRUSTED_BASE.md](TRUSTED_BASE.md)") != std::string::npos);
    CHECK(md.find("([PROVED](VERDICTS.md#verdict-proved))") != std::string::npos);
    CHECK(slurp(tmp.dir / "TRUSTED_BASE.md") == prism::trusted_base_text());
    CHECK(fs::exists(tmp.dir / "VERDICTS.md"));
    auto sarif = nlohmann::json::parse(slurp(tmp.dir / "report.sarif"));
    auto& tb = sarif["runs"][0]["properties"]["trustedBase"];
    CHECK(tb["file"] == "TRUSTED_BASE.md");
    CHECK(tb["sha256"] == prism::sha256_hex(prism::trusted_base_text()));
}

#ifdef PRISM_HAS_Z3
TEST_CASE("certified: a function with no VC stays PROVED (nothing to certify)") {
    auto t = cert_pir("define i32 @u(i32 %a, i32 %b) {\nentry:\n  %s = add i32 %a, %b\n  ret i32 %s\n}\n", "u");
    REQUIRE(t.fn.has_value());
    auto v = prism::pir::check_function(*t.fn, cert_opts(true));
    CHECK(v.extra.at("properties") == "0");
    // A certificate that checks nothing is not labelled certified.
    CHECK(v.status == prism::laws::PROVED);
    CHECK(v.extra.count("certificate") == 0);
    CHECK(v.extra.at("certificate_vcs") == "0");
    CHECK(v.extra.at("certify_note") == "no verification conditions (nothing to certify)");
    // plain mode never certifies either
    CHECK(prism::pir::check_function(*t.fn, cert_opts(false)).status == prism::laws::PROVED);
}

TEST_CASE("pir: C23 units that do not compile as C17 are lowered as C23 (skips without clang/opt)") {
    auto cfg = prism::default_config();
    auto fe = prism::pir::find_frontend(cfg);
    if (!fe.clang || !fe.opt) return;
    CertTmp tmp;
    auto src = tmp.dir / "c23.c";
    std::ofstream(src) << "bool c23_ok(int a) {\n    typeof(a) b = a & 7;\n    return b < 8 && nullptr == (void *)0;\n}\n";
    cfg.root = tmp.dir;
    cfg.jobs = 1;
    cfg.solver_cache = tmp.dir / "cache";
    auto out = prism::pir::run_pir({src}, cfg);
    bool seen = false;
    for (auto& f : out)
        if (f.function && *f.function == "c23_ok") {
            seen = true;
            CHECK(prism::laws::is_proof(f.status));
        }
    CHECK(seen);
}

TEST_CASE("certified: a certified request never loses a cached plain answer") {
    CertTmp tmp;
    z3::context c;
    auto x = c.bv_const("x", 16);
    auto f = (x * 3) != (x + x + x);  // unsat
    prism::solver::SolveOptions o;
    o.cache_dir = (tmp.dir / "cache").string();
    o.timeout_s = 20;
    auto plain = prism::solver::solve(c, f, o);
    REQUIRE(plain.kind == prism::solver::SolveResult::Unsat);
    // No member can run now (Z3 off, no tools): the certified request gets
    // the cached plain unsat, never certified.
    o.certified = true;
    o.z3_in_process = false;
    o.search_default_tools = false;
    o.sls = false;
    auto r = prism::solver::solve(c, f, o);
    CHECK(r.kind == prism::solver::SolveResult::Unsat);
    CHECK_FALSE(r.certified);
    CHECK(r.cache_hit);
    CHECK(prism::solver::verdict_status(r) == prism::laws::PROVED);
    CHECK(r.note.find("cached plain unsat stands") != std::string::npos);
}

// Memory-model VCs (docs/PIR.md "Memory model") are QF_BV in the default
// Bv encoding, so they are certified like any other VC; the Array encoding
// is not certifiable and says so.
TEST_CASE("certified: memory-model VCs are certified in the Bv encoding, not in the Array encoding") {
    const std::string ir = R"IR(define i32 @arr_ok(i32 %i) {
entry:
  %a = alloca [4 x i32], align 16
  call void @llvm.memset.p0.i64(ptr align 16 %a, i8 0, i64 16, i1 false)
  %m = and i32 %i, 3
  %x = sext i32 %m to i64
  %p = getelementptr inbounds [4 x i32], ptr %a, i64 0, i64 %x
  %v = load i32, ptr %p, align 4
  ret i32 %v
}
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)
)IR";
    auto t = cert_pir(ir, "arr_ok");
    REQUIRE(t.fn.has_value());
    REQUIRE(t.fn->uses_memory);
    auto o = cert_opts(true);
    auto v = prism::pir::check_function(*t.fn, o);
    CAPTURE(v.message);
    CHECK(v.extra.at("memory") == "bv");
    CHECK(std::stoi(v.extra.at("certificate_vcs")) > 0);
    if (have_cert_chain()) {
        CHECK(v.status == prism::laws::PROVED_CERTIFIED);
        CHECK(v.extra.at("certificate") == "checked");
        CHECK(v.extra.count("certificate_bitblast") == 1);
    } else {
        CHECK(v.status == prism::laws::PROVED);
        CHECK(v.extra.count("certify_note") == 1);
    }
    o.encode.memory = prism::pir::MemEncoding::Array;
    auto va = prism::pir::check_function(*t.fn, o);
    CAPTURE(va.message);
    CHECK(va.extra.at("memory") == "array");
    CHECK(va.status == prism::laws::PROVED);
    REQUIRE(va.extra.count("certify_note") == 1);
    CHECK(va.extra.at("certify_note").find("not certif") != std::string::npos);

    // An out-of-bounds read is FAILED through the solver library, with a
    // counterexample the PIR interpreter replays.
    std::string bad = ir;
    bad.replace(bad.find("and i32 %i, 3"), 13, "and i32 %i, 4");
    auto tb = cert_pir(bad, "arr_ok");
    REQUIRE(tb.fn.has_value());
    auto vb = prism::pir::check_function(*tb.fn, cert_opts(true));
    CHECK(vb.status == prism::laws::FAILED);
    CHECK(vb.cls == "MEM-OOB-READ");
    REQUIRE(!vb.cex_args.empty());
    CHECK(prism::pir::interpret(*tb.fn, vb.cex_args).status == prism::pir::InterpResult::Violation);
}
#endif
