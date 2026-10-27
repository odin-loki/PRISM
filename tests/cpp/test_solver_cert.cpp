// SolveResult::certified is true only after a solver said UNSAT and cake_lpr
// (and, on the Lean bit-blaster path, Lean's LRAT checker) accepted the proof.
// These cases hide every certificate binary. solve() must return without
// starting a member: a missing cake_lpr leaves certified false (the note may
// say NOTRUN), and a cached plain Unsat is not a certificate.

#include <doctest/doctest.h>

#include "prism/laws.hpp"
#include "prism/solver.hpp"
#include "prism/solver_certs.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#if defined(PRISM_HAS_Z3)

namespace {

namespace fs = std::filesystem;
namespace ps = prism::solver;

struct CertFlagTmp {
    fs::path dir;
    CertFlagTmp() {
        static int n = 0;
        dir = fs::temp_directory_path() /
              ("prism-solver-cert-" + std::to_string(std::random_device{}()) + "-" + std::to_string(n++));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~CertFlagTmp() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// No default tool roots, no PATH, no portfolio, no in-process Z3. find_tool
// returns before any binary is executed, and no member is added to run.
ps::SolveOptions hidden_cert_tools(const fs::path& cache) {
    ps::SolveOptions o;
    o.cache_dir = cache.string();
    o.timeout_s = 1;
    o.cert_timeout_s = 1;
    o.check_timeout_s = 1;
    o.certified = true;
    o.portfolio = false;
    o.sls = false;
    o.z3_in_process = false;
    o.search_default_tools = false;
    o.bitblaster = ps::Bitblaster::Z3;
    o.use_cache = false;
    o.cache_certificates = false;
    o.known_unsat = false;
    return o;
}

void require_tools_hidden(const ps::SolveOptions& o) {
    for (const char* name : {"cake_lpr", "cadical", "kissat", "bitwuzla", "prism-bitblast", "prism-lrat-check",
                             "lrat-check"})
        REQUIRE_MESSAGE(!ps::find_tool(name, o).has_value(), name);
}

void not_a_certified_unsat(const ps::SolveResult& r) {
    const bool fake_certified_unsat = r.kind == ps::SolveResult::Unsat && r.certified;
    CHECK_MESSAGE(!fake_certified_unsat, r.note);
    CHECK_FALSE(r.certified);
    CHECK(r.certificate_info.empty());
    CHECK(r.ran.empty());
    CHECK(ps::verdict_status(r) != ps::kProvedCertified);
}

void write_unsat_cache(const fs::path& root, const std::string& hash, bool certified_flag,
                       const std::string& cnf_sha) {
    REQUIRE(hash.size() >= 2);
    const fs::path dir = root / "queries" / hash.substr(0, 2);
    fs::create_directories(dir);
    std::ofstream out(dir / (hash + ".json"), std::ios::binary | std::ios::trunc);
    REQUIRE(out);
    out << "{\"schema\":1,\"hash\":\"" << hash << "\",\"kind\":\"unsat\",\"winner\":\"z3\","
        << "\"certified\":" << (certified_flag ? "true" : "false") << ",\"bitblaster\":\"z3\","
        << "\"cnf_sha256\":\"" << cnf_sha << "\",\"cert_solver\":\"cadical\"}\n";
}

}  // namespace

TEST_CASE("solver cert: absent certificate tools leave certified false") {
    CertFlagTmp t;
    auto o = hidden_cert_tools(t.dir / "cache");
    require_tools_hidden(o);
    z3::context c;
    auto x = c.bv_const("x", 8);
    auto r = ps::solve(c, x != x, o);
    not_a_certified_unsat(r);
    CHECK(r.kind == ps::SolveResult::Unknown);
    CHECK(ps::verdict_status(r) == prism::laws::UNKNOWN);
    CHECK(r.note.find("NOTRUN") != std::string::npos);
    CHECK(r.note.find("cadical not found (NOTRUN)") != std::string::npos);
}

TEST_CASE("solver cert: a caller's plain unsat is not certified without cake_lpr") {
    CertFlagTmp t;
    auto o = hidden_cert_tools(t.dir / "cache");
    o.known_unsat = true;
    require_tools_hidden(o);
    z3::context c;
    auto x = c.bv_const("x", 8);
    auto r = ps::solve(c, x != x, o);
    not_a_certified_unsat(r);
    CHECK(r.kind == ps::SolveResult::Unsat);
    CHECK(ps::verdict_status(r) == prism::laws::PROVED);
    CHECK(r.note.find("NOTRUN") != std::string::npos);
    CHECK(r.winner == "caller (plain unsat)");
}

TEST_CASE("solver cert: a cached plain unsat is not certified") {
    CertFlagTmp t;
    z3::context c;
    auto x = c.bv_const("x", 8);
    const auto f = x != x;
    const auto hash = ps::query_hash(c, f);
    const fs::path cache = t.dir / "cache";
    write_unsat_cache(cache, hash, /*certified_flag=*/false, "");
    auto o = hidden_cert_tools(cache);
    o.use_cache = true;
    require_tools_hidden(o);
    auto r = ps::solve(c, f, o);
    not_a_certified_unsat(r);
    CHECK(r.kind == ps::SolveResult::Unsat);
    CHECK(ps::verdict_status(r) == prism::laws::PROVED);
    CHECK(r.note.find("uncertified") != std::string::npos);
    CHECK(r.note.find("not certified") != std::string::npos);
    CHECK(r.note.find("NOTRUN") != std::string::npos);
}

TEST_CASE("solver cert: a cached certified flag is re-checked and cake_lpr missing stays false") {
    CertFlagTmp t;
    z3::context c;
    auto x = c.bv_const("x", 8);
    const auto f = x != x;
    std::string why;
    auto cnf = ps::bitblast(c, f, &why);
    REQUIRE_MESSAGE(cnf, why);
    const auto hash = ps::query_hash(c, f);
    const auto sha = ps::sha256_hex(ps::to_dimacs(*cnf));
    const fs::path cache = t.dir / "cache";
    write_unsat_cache(cache, hash, /*certified_flag=*/true, sha);
    const fs::path lrat = t.dir / "proof.lrat";
    {
        std::ofstream out(lrat, std::ios::binary | std::ios::trunc);
        REQUIRE(out);
        out << "1 0 0\n";
    }
    REQUIRE_MESSAGE(ps::certs::store(cache, hash, lrat, std::uint64_t{1} << 20, &why), why);
    auto o = hidden_cert_tools(cache);
    o.use_cache = true;
    require_tools_hidden(o);
    auto r = ps::solve(c, f, o);
    not_a_certified_unsat(r);
    CHECK(r.kind == ps::SolveResult::Unsat);
    CHECK(ps::verdict_status(r) == prism::laws::PROVED);
    CHECK(r.note.find("not certified: cake_lpr not found (NOTRUN)") != std::string::npos);
}

#endif  // PRISM_HAS_Z3
