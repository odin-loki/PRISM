// Cube-and-conquer (src/prism/solver/cube.cpp, docs/SOLVERS.md
// "Cube-and-conquer"): the cover check, the soundness rules (a dropped or open
// cube is never UNSAT), answers against Z3 alone, and the GPU walker.
#include <doctest/doctest.h>

#include "prism/solver.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace ps = prism::solver;

TEST_CASE("cube: the cover check") {
    CHECK(ps::cubes_cover({{1}, {-1}}));
    CHECK(ps::cubes_cover({{1, 2}, {1, -2}, {-1}}));
    CHECK(ps::cubes_cover({{}}));
    std::string why;
    CHECK_FALSE(ps::cubes_cover({{1, 2}, {-1}}, &why));  // 1 & -2 is missing
    CHECK(why.find("do not cover") != std::string::npos);
    CHECK_FALSE(ps::cubes_cover({}));
    CHECK_FALSE(ps::cubes_cover({{1, 0}}));
    // a complete binary tree of depth 6 covers; without any one leaf it does not
    std::vector<std::vector<int>> leaves;
    for (int m = 0; m < 64; ++m) {
        std::vector<int> c;
        for (int b = 0; b < 6; ++b) c.push_back((m >> b) & 1 ? b + 1 : -(b + 1));
        leaves.push_back(c);
    }
    CHECK(ps::cubes_cover(leaves));
    for (std::size_t k = 0; k < leaves.size(); k += 7) {
        auto drop = leaves;
        drop.erase(drop.begin() + std::ptrdiff_t(k));
        CHECK_FALSE(ps::cubes_cover(drop));
    }
}

TEST_CASE("cube: options from the environment fill only unset fields") {
    ps::CubeOptions o;
    o.enabled = 0;
    o.threads = 3;
    auto r = ps::resolve_cube_options(o);
    CHECK(r.enabled == 0);
    CHECK(r.threads == 3);
    CHECK(r.conflicts > 0);
    CHECK((r.split == "lookahead" || r.split == "inputs"));
}

#ifdef PRISM_HAS_Z3
namespace {

// x * y == p over 32 bits with 1 < x, y < 2^16 (no overflow): unsat for a prime p.
z3::expr factor(z3::context& c, std::uint64_t p) {
    z3::expr x = c.bv_const("x", 32), y = c.bv_const("y", 32);
    return x * y == c.bv_val(p, 32) && z3::ugt(x, c.bv_val(1, 32)) && z3::ugt(y, c.bv_val(1, 32)) &&
           z3::ult(x, c.bv_val(65536, 32)) && z3::ult(y, c.bv_val(65536, 32));
}

ps::SolveOptions cube_opts(const std::filesystem::path& dir) {
    ps::SolveOptions o;
    o.cache_dir = (dir / "cache").string();
    o.use_cache = false;
    o.timeout_s = 60;
    o.cube.enabled = 1;
    o.cube.alone = true;
    o.cube.threads = 4;
    o.cube.conflicts = 20;  // small: forces splits and re-splits
    o.cube.after_s = 0;
    o.cube.gpu = 0;
    return o;
}

std::filesystem::path tmpdir(const char* tag) {
    auto d = std::filesystem::temp_directory_path() / (std::string("prism-cube-test-") + tag);
    std::filesystem::create_directories(d);
    return d;
}

}  // namespace

TEST_CASE("cube: unsat by a closed tree whose leaves cover the space") {
    z3::context c;
    auto o = cube_opts(tmpdir("unsat"));
    auto r = ps::cube_and_conquer(c, factor(c, 65521), o, o.timeout_s);
    if (!ps::cube_engine_available()) {
        CHECK(r.kind == ps::SolveResult::Unknown);
        CHECK(r.note.find("NOTRUN") != std::string::npos);
        return;
    }
    MESSAGE(r.note);
    REQUIRE(r.kind == ps::SolveResult::Unsat);
    CHECK(r.cube.cover_checked);
    // Two ways to UNSAT, each checked in full; any other way is a failure:
    //   tree:       every leaf is closed and the leaves cover the space
    //   root_unsat: a leaf's failed assumptions were empty, so the CNF alone is unsat (leaves may still be open,
    //               depending on thread timing); that the formula alone is unsat is checked by Z3 independently
    CAPTURE(r.cube.unsat_by);
    if (r.cube.unsat_by == "tree") {
        CHECK(r.cube.open == 0);
        CHECK(r.cube.leaves > 1);  // it really split
        CHECK(r.cube.closed_solver + r.cube.closed_core == r.cube.leaves);
        CHECK(r.note.find("by=tree") != std::string::npos);
    } else if (r.cube.unsat_by == "root_unsat") {
        CHECK(r.note.find("by=root_unsat") != std::string::npos);
        CHECK(r.note.find("empty core") != std::string::npos);
        z3::context c2;
        z3::solver alone(c2);
        alone.add(factor(c2, 65521));
        CHECK(alone.check() == z3::unsat);
    } else {
        FAIL("UNSAT reached neither by a closed tree nor by an empty core: " << r.cube.unsat_by);
    }
}

TEST_CASE("cube: a dropped cube never gives unsat") {
    if (!ps::cube_engine_available()) return;
    z3::context c;
    for (long long drop : {0LL, 1LL, 3LL}) {
        auto o = cube_opts(tmpdir("drop"));
        o.cube.debug_drop_leaf = drop;
        auto r = ps::cube_and_conquer(c, factor(c, 65521), o, o.timeout_s);
        MESSAGE(r.note);
        if (r.cube.leaves <= std::size_t(drop)) continue;  // nothing dropped
        CHECK(r.kind != ps::SolveResult::Unsat);
        CHECK_FALSE(r.cube.cover_checked);
        CHECK(r.note.find("do not cover") != std::string::npos);
    }
}

TEST_CASE("cube: an open cube at the deadline never gives unsat") {
    if (!ps::cube_engine_available()) return;
    z3::context c;
    // a 64-bit multiplication with a prime: far beyond 0.5 s
    z3::expr x = c.bv_const("x", 64), y = c.bv_const("y", 64);
    const std::uint64_t p = 18446744073709551557ull;  // the largest 64-bit prime
    z3::expr f = x * y == c.bv_val(p, 64) && z3::ugt(x, c.bv_val(1, 64)) && z3::ugt(y, c.bv_val(1, 64)) &&
                 z3::bvmul_no_overflow(x, y, false);
    auto o = cube_opts(tmpdir("open"));
    auto r = ps::cube_and_conquer(c, f, o, 0.5);
    MESSAGE(r.note);
    CHECK(r.kind != ps::SolveResult::Unsat);
    CHECK(r.kind != ps::SolveResult::Sat);
}

TEST_CASE("cube: sat answers carry a validated model") {
    if (!ps::cube_engine_available()) return;
    z3::context c;
    auto o = cube_opts(tmpdir("sat"));
    const z3::expr f = factor(c, 65521ull * 65519ull);
    auto r = ps::cube_and_conquer(c, f, o, o.timeout_s);
    MESSAGE(r.note);
    REQUIRE(r.kind == ps::SolveResult::Sat);
    CHECK(ps::validate_model(c, f, r.model));
}

TEST_CASE("cube: answers agree with Z3 alone on random bitvector formulas") {
    if (!ps::cube_engine_available()) return;
    std::mt19937_64 rng(20261120);
    int sat = 0, unsat = 0;
    for (int i = 0; i < 40; ++i) {
        z3::context c;
        const unsigned w = 8 + unsigned(rng() % 9);
        z3::expr a = c.bv_const("a", w), b = c.bv_const("b", w), d = c.bv_const("d", w);
        std::vector<z3::expr> terms{a, b, d, a * b, a + d, b - d, a & d, a | b, a ^ d, z3::shl(a, z3::urem(b, c.bv_val(w, w))),
                                    z3::udiv(a, b), z3::urem(d, a), a * d * b};
        auto t = [&] { return terms[rng() % terms.size()]; };
        z3::expr f = c.bool_val(true);
        for (int k = 0; k < 3; ++k) {
            const auto rel = rng() % 3;
            z3::expr e = rel == 0 ? t() == t() : rel == 1 ? z3::ult(t(), t()) : z3::sle(t(), t());
            f = f && ((rng() & 1) ? e : !e);
        }
        z3::solver s(c);
        s.add(f);
        const auto want = s.check();
        auto o = cube_opts(tmpdir("rand"));
        o.cube.conflicts = 5;
        auto r = ps::cube_and_conquer(c, f, o, 30);
        INFO(f.to_string());
        INFO(r.note);
        if (want == z3::sat) {
            CHECK(r.kind == ps::SolveResult::Sat);
            ++sat;
        } else if (want == z3::unsat) {
            CHECK(r.kind == ps::SolveResult::Unsat);
            ++unsat;
        }
        if (r.kind == ps::SolveResult::Sat) CHECK(ps::validate_model(c, f, r.model));
    }
    MESSAGE("random formulas: " << sat << " sat, " << unsat << " unsat");
}

TEST_CASE("cube: solve() with the cube engine next to the portfolio") {
    z3::context c;
    auto o = cube_opts(tmpdir("solve"));
    o.cube.alone = false;
    o.timeout_s = 30;
    for (std::uint64_t p : {65521ull, 65521ull * 65519ull}) {
        const z3::expr f = factor(c, p);
        auto r = ps::solve(c, f, o);
        MESSAGE(r.winner << ": " << r.note);
        if (p == 65521ull) CHECK(r.kind == ps::SolveResult::Unsat);
        else CHECK(r.kind == ps::SolveResult::Sat);
        if (r.kind == ps::SolveResult::Sat) CHECK(ps::validate_model(c, f, r.model));
    }
}
#endif  // PRISM_HAS_Z3

#ifdef PRISM_HAS_CUDA
TEST_CASE("cube: GPU ProbSAT with statistics") {
    // a planted 3-CNF (as in "solver: GPU ProbSAT finds a planted assignment")
    constexpr int nv = 2000, nc = 8400;
    ps::Cnf cnf;
    cnf.num_vars = nv;
    std::mt19937_64 rng(7);
    std::vector<int> plant(nv + 1);
    for (int v = 1; v <= nv; ++v) plant[v] = (rng() & 1) ? 1 : -1;
    for (int i = 0; i < nc; ++i) {
        std::vector<int> cl;
        const int keep = int(rng() % 3);
        for (int j = 0; j < 3; ++j) {
            const int v = int(rng() % nv) + 1;
            int lit = v * plant[v];
            if (j != keep && (rng() & 1)) lit = -lit;
            cl.push_back(lit);
        }
        cnf.clauses.push_back(cl);
    }
    std::vector<std::int8_t> a;
    ps::CudaProbsatInfo info;
    const bool ok = ps::cuda_probsat_ex(cnf, 3, 200000, 2048, a, &info);
    MESSAGE("gpu " << info.device << " walkers " << info.walkers << " kernel " << info.kernel_s << "s error '"
                   << info.error << "'");
    if (!info.error.empty() && info.error.find("no CUDA device") != std::string::npos) return;  // no GPU here
    REQUIRE(ok);
    CHECK(ps::assignment_satisfies(cnf, a));
    // an unsatisfiable CNF: never "found"
    ps::Cnf u;
    u.num_vars = 2;
    u.clauses = {{1, 2}, {-1, 2}, {1, -2}, {-1, -2}};
    std::vector<std::int8_t> b;
    CHECK_FALSE(ps::cuda_probsat_ex(u, 1, 10000, 64, b, nullptr));
}
#endif
