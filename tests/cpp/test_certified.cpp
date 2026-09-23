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

    auto v = prism::pir::check_function(*t.fn, cert_opts(true));
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
    auto v = prism::pir::check_function(*t.fn, cert_opts(true));
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
    // A second run answers from the cache; certified entries are re-checked.
    auto again = prism::pir::run_pir({dir / "overflow.c"}, cfg);
    for (auto& f : again)
        if (f.function && *f.function == "add_ok") {
            CHECK(f.status == by["add_ok"]->status);
            CHECK(f.extra.at("solver").find("cache hits 0") == std::string::npos);
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
TEST_CASE("certified: a function with no VC is certified vacuously and says so") {
    auto t = cert_pir("define i32 @u(i32 %a, i32 %b) {\nentry:\n  %s = add i32 %a, %b\n  ret i32 %s\n}\n", "u");
    REQUIRE(t.fn.has_value());
    auto v = prism::pir::check_function(*t.fn, cert_opts(true));
    CHECK(v.extra.at("properties") == "0");
    CHECK(v.status == prism::laws::PROVED_CERTIFIED);
    CHECK(v.extra.at("certificate") == "checked");
    CHECK(v.extra.at("certificate_vcs") == "0");
    CHECK(v.extra.at("certificate_info").rfind("0 VCs:", 0) == 0);
    // plain mode never certifies, even vacuously
    CHECK(prism::pir::check_function(*t.fn, cert_opts(false)).status == prism::laws::PROVED);
}
#endif
