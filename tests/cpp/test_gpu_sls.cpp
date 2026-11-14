// GPU ProbSAT (docs/GPU_PLAN.md G7): src/cuda/probsat.cu through the shared
// CUDA driver loader. A found assignment must satisfy every clause; without a
// GPU the walker must say why it did not run (Law 1, 7), never report clean.
#include "prism/cuda_driver.hpp"
#include "prism/solver.hpp"

#include <doctest/doctest.h>

#include <random>
#include <string>

namespace ps = prism::solver;

namespace {

// Planted random 3-SAT (as tests/cpp/test_main.cpp "GPU ProbSAT finds a planted
// assignment"): one literal of each clause agrees with the plant.
ps::Cnf planted_3sat(int nv, int nc, unsigned long long seed) {
    ps::Cnf cnf;
    cnf.num_vars = nv;
    std::mt19937_64 rng(seed);
    std::vector<int> plant(std::size_t(nv) + 1);
    for (int v = 1; v <= nv; ++v) plant[std::size_t(v)] = (rng() & 1) ? 1 : -1;
    cnf.clauses.reserve(std::size_t(nc));
    for (int i = 0; i < nc; ++i) {
        int vs[3];
        vs[0] = int(rng() % unsigned(nv)) + 1;
        do { vs[1] = int(rng() % unsigned(nv)) + 1; } while (vs[1] == vs[0]);
        do { vs[2] = int(rng() % unsigned(nv)) + 1; } while (vs[2] == vs[0] || vs[2] == vs[1]);
        const int keep = int(rng() % 3);
        std::vector<int> clause;
        for (int j = 0; j < 3; ++j) {
            int lit = vs[j] * plant[std::size_t(vs[j])];
            if (j != keep && (rng() & 1)) lit = -lit;
            clause.push_back(lit);
        }
        cnf.clauses.push_back(std::move(clause));
    }
    return cnf;
}

bool have_gpu() {
    std::string why;
    if (ps::gpu_sls_available(&why)) return true;
    MESSAGE("GPU ProbSAT not run: " << why);
    return false;
}

}  // namespace

TEST_CASE("gpu sls: no GPU is reported, never a clean result") {
    std::string why;
    const bool ok = ps::gpu_sls_available(&why);
    if (!ok) CHECK_FALSE(why.empty());
    auto cnf = planted_3sat(50, 200, 7);
    auto r = ps::gpu_probsat(cnf, ps::GpuSlsOptions{});
    if (!ok) {
        CHECK_FALSE(r.ran);
        CHECK_FALSE(r.found);
        CHECK(r.note.find("not run") != std::string::npos);
    }
    if (r.found) CHECK(ps::assignment_satisfies(cnf, r.assignment));
}

TEST_CASE("gpu sls: walkers find a planted 3-SAT assignment that satisfies every clause") {
    if (!have_gpu()) return;
    for (unsigned lanes : {32u, 1u}) {
        CAPTURE(lanes);
        auto cnf = planted_3sat(800, 3200, 0xC0FFEEULL);
        ps::GpuSlsOptions o;
        o.seed = 1;
        o.max_flips = 200000;
        o.lanes_per_walker = lanes;
        auto r = ps::gpu_probsat(cnf, o);
        INFO(r.note);
        REQUIRE(r.ran);
        REQUIRE(r.found);
        CHECK(r.assignment.size() == std::size_t(cnf.num_vars) + 1);
        CHECK(ps::assignment_satisfies(cnf, r.assignment));
        CHECK(r.flips > 0);
        CHECK(r.walkers > 0);
        CHECK(r.device_bytes > 0);
    }
    // Short form and the first walker's signature.
    auto cnf = planted_3sat(2000, 8000, 42);
    std::string why;
    auto a = ps::gpu_probsat(cnf, 3, 500000, 256, &why);
    INFO(why);
    REQUIRE(a.has_value());
    CHECK(ps::assignment_satisfies(cnf, *a));
    std::vector<std::int8_t> b;
    REQUIRE(ps::cuda_probsat(cnf, 5, 500000, 64, b));
    CHECK(ps::assignment_satisfies(cnf, b));
}

TEST_CASE("gpu sls: unsatisfiable or unsupported input finds nothing and says why") {
    if (!have_gpu()) return;
    ps::Cnf unsat;
    unsat.num_vars = 2;
    unsat.clauses = {{1, 2}, {-1, 2}, {1, -2}, {-1, -2}};
    ps::GpuSlsOptions o;
    o.max_flips = 5000;
    o.walkers = 64;
    auto r = ps::gpu_probsat(unsat, o);
    CHECK(r.ran);
    CHECK_FALSE(r.found);  // "not found" is all it can say
    CHECK(r.note.find("not a proof") != std::string::npos);

    ps::Cnf empty_clause;
    empty_clause.num_vars = 1;
    empty_clause.clauses = {{}};
    auto e = ps::gpu_probsat(empty_clause, o);
    CHECK_FALSE(e.ran);
    CHECK_FALSE(e.found);
    CHECK_FALSE(e.note.empty());

    ps::Cnf taut;  // tautologies and duplicate literals are dropped, not misread
    taut.num_vars = 3;
    taut.clauses = {{1, -1, 2}, {3, 3, -2}, {-3, -3}};
    auto t = ps::gpu_probsat(taut, o);
    REQUIRE(t.found);
    CHECK(ps::assignment_satisfies(taut, t.assignment));

    // A budget too small for one walker is NOTRUN with the reason, not a silent skip.
    auto big = planted_3sat(1000, 4000, 9);
    ps::GpuSlsOptions tiny;
    tiny.max_device_bytes = 1024;
    auto s = ps::gpu_probsat(big, tiny);
    CHECK_FALSE(s.ran);
    CHECK(s.note.find("does not fit") != std::string::npos);
}
