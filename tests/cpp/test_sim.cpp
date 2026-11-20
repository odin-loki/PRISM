// Symbolic similarity (prism::sim): canonical form, pseudo-metric laws,
// Z3-checked semantic equivalence, HNSW recall, strategy hints, Lean goals.
// Opt-in feature; nothing here touches a verdict path.

#include <doctest/doctest.h>

#ifdef PRISM_HAS_Z3

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <z3++.h>

#include <chrono>
#include <fstream>
#include <map>

#include <nlohmann/json.hpp>

#include "prism/config.hpp"
#include "prism/pir.hpp"
#include "prism/solver.hpp"
#include "prism/sim/index.hpp"
#include "prism/sim/lean_goal.hpp"
#include "prism/sim/symbolic_metric.hpp"

namespace sim = prism::sim;

namespace {

std::string bvdecl(const char* names, unsigned w = 32) {
    std::string out;
    std::istringstream is(names);
    std::string n;
    while (is >> n) out += "(declare-fun " + n + " () (_ BitVec " + std::to_string(w) + "))\n";
    return out;
}
sim::sym_vec embed_text(const std::string& smt) {
    std::string why;
    auto v = sim::embed_smt2(smt, &why);
    REQUIRE_MESSAGE(v.has_value(), why);
    return *v;
}
const sim::symbolic_metric kMetric{0.5f};

// Embed the term text T (over x y z, 32-bit) itself, and return it with the z3 expr.
struct pair_terms {
    z3::context c;
    z3::expr a, b;
    pair_terms(const std::string& ta, const std::string& tb, unsigned w = 32)
        : a(c), b(c) {
        std::string smt = bvdecl("x y z", w) + "(declare-fun o1 () (_ BitVec " + std::to_string(w) + "))\n(declare-fun o2 () (_ BitVec " +
                          std::to_string(w) + "))\n(assert (= o1 " + ta + "))\n(assert (= o2 " + tb + "))\n";
        auto v = c.parse_string(smt.c_str());
        REQUIRE(v.size() == 2);
        auto pick = [&](const z3::expr& e) { return e.arg(0).is_const() && e.arg(0).decl().name().str().rfind("o", 0) == 0 ? e.arg(1) : e.arg(0); };
        a = pick(v[0]);
        b = pick(v[1]);
    }
    bool z3_equiv() {
        z3::solver s(c);
        s.add(a != b);
        return s.check() == z3::unsat;
    }
    float sem_distance() {
        auto ta = sim::from_z3(a);
        auto tb = sim::from_z3(b);
        REQUIRE(ta.has_value());
        REQUIRE(tb.has_value());
        const auto va = sim::embed(*ta), vb = sim::embed(*tb);
        return static_cast<float>(sim::semantic_distance(va, vb));
    }
};

}  // namespace

TEST_CASE("sim: pseudo-metric laws on a formula set") {
    const std::vector<std::string> fs = {
        bvdecl("a b") + "(assert (bvult (bvadd a b) (bvmul a (_ bv3 32))))",
        bvdecl("a b") + "(assert (= (bvand a b) (bvor a b)))",
        bvdecl("a") + "(assert (= (bvshl a (_ bv1 32)) (bvmul a (_ bv2 32))))",
        bvdecl("a b c") + "(assert (and (bvslt a b) (bvule b c) (not (= a c))))",
        bvdecl("a b", 8) + "(assert (= ((_ extract 3 0) a) ((_ extract 7 4) b)))",
        "(declare-fun p () Bool)(declare-fun q () Bool)(assert (or (and p q) (not p)))",
        "(declare-fun i () Int)(declare-fun j () Int)(assert (<= (+ i (* 2 j)) 10))",
        "(declare-fun m () (Array (_ BitVec 32) (_ BitVec 8)))" + bvdecl("a") + "(assert (= (select (store m a #x07) a) #x07))",
    };
    std::vector<sim::sym_vec> v;
    for (const auto& f : fs) v.push_back(embed_text(f));
    for (std::size_t i = 0; i < v.size(); ++i) {
        CHECK(kMetric.distance(v[i], v[i]) == 0.0f);
        for (std::size_t j = 0; j < v.size(); ++j) {
            const float d = kMetric.distance(v[i], v[j]);
            CHECK(d >= 0.0f);
            CHECK(d <= 1.0f);
            CHECK(d == kMetric.distance(v[j], v[i]));
            if (i != j) CHECK_MESSAGE(d > 0.0f, "distinct formulas " << i << " and " << j << " at distance 0");
        }
    }
}

TEST_CASE("sim: alpha- and AC-equivalent formulas have distance 0") {
    const auto base = embed_text(bvdecl("a b c") + "(assert (and (bvult (bvadd a b c) (bvmul a b)) (= (bvxor a c) b)))");
    // alpha: variables renamed; AC: operands permuted, nested n-ary flattened, equality sides swapped
    const auto alpha = embed_text(bvdecl("p q r") + "(assert (and (bvult (bvadd p q r) (bvmul p q)) (= (bvxor p r) q)))");
    const auto ac = embed_text(bvdecl("a b c") + "(assert (and (= b (bvxor c a)) (bvult (bvadd (bvadd c b) a) (bvmul b a))))");
    const auto both = embed_text(bvdecl("u v w") + "(assert (and (= v (bvxor w u)) (bvult (bvadd w (bvadd v u)) (bvmul v u))))");
    CHECK(kMetric.distance(base, alpha) == 0.0f);
    CHECK(kMetric.distance(base, ac) == 0.0f);
    CHECK(kMetric.distance(base, both) == 0.0f);
    // a different formula is not at distance 0
    const auto other = embed_text(bvdecl("a b c") + "(assert (and (bvult (bvadd a b c) (bvmul a b)) (= (bvor a c) b)))");
    CHECK(kMetric.distance(base, other) > 0.0f);
}

TEST_CASE("sim: canonical form keeps small literals and abstracts the rest") {
    const auto a = embed_text(bvdecl("a") + "(assert (bvult a (_ bv1000 32)))");
    const auto b = embed_text(bvdecl("a") + "(assert (bvult a (_ bv2000 32)))");
    const auto c = embed_text(bvdecl("a") + "(assert (bvult a (_ bv1024 32)))");  // a power of two: kept
    CHECK(sim::structural_distance(a, b) == 0.0);
    CHECK(sim::structural_distance(a, c) > 0.0);
}

TEST_CASE("sim: Z3-proved equivalent terms have semantic distance 0") {
    const std::vector<std::pair<std::string, std::string>> pairs = {
        {"(bvmul x #x00000002)", "(bvshl x #x00000001)"},
        {"(bvsub x y)", "(bvadd x (bvneg y))"},
        {"(bvnot (bvnot x))", "x"},
        {"(bvnot (bvand x y))", "(bvor (bvnot x) (bvnot y))"},
        {"(bvnot (bvor x y))", "(bvand (bvnot x) (bvnot y))"},
        {"(bvadd x x)", "(bvmul x #x00000002)"},
        {"(bvneg x)", "(bvadd (bvnot x) #x00000001)"},
        {"(bvsub (bvor x y) (bvand x y))", "(bvxor x y)"},
        {"(bvmul x #x00000003)", "(bvadd (bvshl x #x00000001) x)"},
        {"(bvxor x x)", "#x00000000"},
        {"(bvadd (bvadd x y) z)", "(bvadd x (bvadd z y))"},
        {"(bvand x (bvnot x))", "#x00000000"},
        {"(bvlshr x #x00000004)", "(concat #x0 ((_ extract 31 4) x))"},
        {"(ite (bvult x y) x y)", "(ite (bvuge x y) y x)"},
        {"(bvudiv x #x00000008)", "(bvlshr x #x00000003)"},
        {"(bvurem x #x00000008)", "(bvand x #x00000007)"},
        {"((_ zero_extend 16) ((_ extract 15 0) x))", "(bvand x #x0000ffff)"},
        {"((_ rotate_left 8) ((_ rotate_right 8) x))", "x"},
        {"(bvashr x #x0000001f)", "(ite (bvslt x #x00000000) #xffffffff #x00000000)"},
        {"(bvsub x x)", "(bvmul y #x00000000)"},
    };
    for (const auto& [ta, tb] : pairs) {
        pair_terms p(ta, tb);
        REQUIRE_MESSAGE(p.z3_equiv(), "Z3 does not prove " << ta << " == " << tb);
        CHECK_MESSAGE(p.sem_distance() == 0.0f, ta << " vs " << tb);
    }
}

TEST_CASE("sim: the evaluator agrees with Z3's simplifier on the supported operators") {
    // simplify() is Z3's own equivalence-preserving rewriting; each pair is built so the simplified
    // term is structurally different, and the signature must match
    const std::vector<std::string> ts = {
        "(bvsdiv x y)", "(bvsrem x y)", "(bvsmod x y)", "(bvudiv x y)", "(bvurem x y)", "(bvashr x y)", "(bvshl x y)", "(bvlshr x y)",
        "(concat ((_ extract 15 0) x) ((_ extract 31 16) y))", "((_ sign_extend 16) ((_ extract 15 0) x))", "((_ rotate_left 5) x)",
        "(bvnand x y)", "(bvnor x y)", "(bvxnor x y)", "(bvmul x (bvadd y z))", "(bvsub (bvmul x y) (bvudiv z y))",
        "(ite (bvslt x y) (bvsub x y) (bvsdiv y x))", "((_ repeat 2) ((_ extract 15 0) x))",
    };
    for (const auto& t : ts) {
        const bool bit = t == "(bvcomp x y)";
        pair_terms p(t, t);
        z3::expr simp = p.a.simplify();
        auto ta = sim::from_z3(p.a), ts2 = sim::from_z3(simp);
        REQUIRE(ta.has_value());
        REQUIRE(ts2.has_value());
        // the same function: the semantic part must be identical even when the structure differs
        const auto va = sim::embed(*ta), vb = sim::embed(*ts2);
        CHECK_MESSAGE(sim::semantic_distance(va, vb) == 0.0, t << (bit ? " (1-bit)" : ""));
    }
}


TEST_CASE("sim: x - y and y - x are alpha-equivalent; the name-keyed component tells them apart") {
    pair_terms p("(bvsub x y)", "(bvsub y x)");
    CHECK_FALSE(p.z3_equiv());  // different functions of the named (x, y), the same function up to renaming
    CHECK(p.sem_distance() == 0.0f);
    auto ta = sim::from_z3(p.a), tb = sim::from_z3(p.b);
    REQUIRE(ta.has_value());
    REQUIRE(tb.has_value());
    const auto va = sim::embed(*ta), vb = sim::embed(*tb);
    CHECK(kMetric.distance(va, vb) == 0.0f);  // the default metric keeps the alpha law
    CHECK(sim::named_distance(va, vb) > 0.0);
    sim::symbolic_metric named{0.5f, 1.0f};
    CHECK(named.distance(va, vb) > 0.0f);
    CHECK(named.distance(va, va) == 0.0f);
    // a renaming of both variables consistently keeps the name-keyed signature equal only when the name order is kept
    pair_terms q("(bvsub x (bvshl y #x00000001))", "(bvsub y (bvshl x #x00000001))");
    CHECK(q.sem_distance() == 0.0f);
}

TEST_CASE("sim: formulas that are false on every probe are separated by their sub-terms") {
    // verification-condition shaped: almost never true, so the root signature is constant
    const auto a = embed_text(bvdecl("a b") + "(assert (and (= (bvadd (bvmul a b) (_ bv5 32)) (_ bv123456789 32)) (= b (_ bv7 32))))");
    const auto b = embed_text(bvdecl("a b") + "(assert (and (= (bvadd (bvmul a b) (_ bv5 32)) (_ bv987654321 32)) (= b (_ bv7 32))))");
    const auto c = embed_text(bvdecl("a b") + "(assert (and (= (bvsub (bvxor a b) (_ bv9 32)) (_ bv123456789 32)) (= b (_ bv7 32))))");
    CHECK(a.sem_constant);
    CHECK(b.sem_constant);
    CHECK(sim::semantic_distance(a, a) == 0.0);
    CHECK(sim::semantic_distance(a, b) == 0.0);  // same sub-function (a*b + 5), different constant compared against
    CHECK(sim::semantic_distance(a, c) > 0.0);    // a different computed sub-function
}

TEST_CASE("sim: Z3-non-equivalent terms are told apart (probe-set miss rate recorded)") {
    const std::vector<std::pair<std::string, std::string>> pairs = {
        {"(bvadd x y)", "(bvsub x y)"}, {"(bvand x y)", "(bvor x y)"}, {"(bvshl x #x00000001)", "(bvlshr x #x00000001)"},
        {"(bvmul x #x00000002)", "(bvmul x #x00000003)"}, {"(bvudiv x y)", "(bvsdiv x y)"}, {"(bvurem x y)", "(bvsrem x y)"},
        {"(bvashr x #x00000001)", "(bvlshr x #x00000001)"}, {"(bvxor x y)", "(bvor x y)"},
        {"(bvnot x)", "(bvneg x)"}, {"(ite (bvult x y) x y)", "(ite (bvslt x y) x y)"}, {"(bvadd x #x00000001)", "(bvadd x #x00000002)"},
        {"(bvand x #x0000ffff)", "(bvand x #x000fffff)"}, {"(bvmul x y)", "(bvadd x y)"}, {"(bvsmod x y)", "(bvsrem x y)"},
        {"(bvlshr x #x00000010)", "(bvlshr x #x00000011)"},
        // differ only on an input pair a random probe set could miss: x == y
        {"(ite (= x y) #x00000001 #x00000000)", "#x00000000"},
        // differ only at x = 0x12345678 (a single point): expected misses
        {"(ite (= x #x12345678) #x00000001 #x00000000)", "#x00000000"},
    };
    unsigned miss = 0;
    std::string missed;
    for (const auto& [ta, tb] : pairs) {
        pair_terms p(ta, tb);
        REQUIRE_MESSAGE(!p.z3_equiv(), "Z3 says equivalent: " << ta << " vs " << tb);  // each has a counterexample
        if (p.sem_distance() == 0.0f) { ++miss; missed += "\n  " + ta + " vs " + tb; }
    }
    MESSAGE("probe-set misses (Z3 non-equivalent, semantic d = 0): " << miss << " of " << pairs.size() << missed);
    // the one-point difference is expected to be missed; anything else is a regression
    CHECK(miss <= 1);
}

namespace {
// A deterministic random corpus of small bit-vector formulas over a, b, c.
std::string rand_term(std::mt19937_64& g, int depth, unsigned w) {
    static const char* un[] = {"bvnot", "bvneg"};
    static const char* bin[] = {"bvadd", "bvsub", "bvmul", "bvand", "bvor", "bvxor", "bvshl", "bvlshr", "bvudiv", "bvurem"};
    const auto pick = g() % 10;
    if (depth == 0 || pick < 2) {
        static const char* vs[] = {"a", "b", "c"};
        if (g() % 4 == 0) {
            std::ostringstream o;
            o << "(_ bv" << (g() % 40) << " " << w << ")";
            return o.str();
        }
        return vs[g() % 3];
    }
    if (pick < 4) return std::string("(") + un[g() % 2] + " " + rand_term(g, depth - 1, w) + ")";
    return std::string("(") + bin[g() % 10] + " " + rand_term(g, depth - 1, w) + " " + rand_term(g, depth - 1, w) + ")";
}
std::string rand_formula(std::mt19937_64& g) {
    static const char* rel[] = {"=", "bvult", "bvule", "bvslt", "bvsle"};
    std::string f = rand_term(g, 2 + static_cast<int>(g() % 3), 16);
    std::string h = rand_term(g, 2 + static_cast<int>(g() % 3), 16);
    std::string s = bvdecl("a b c", 16) + "(assert (" + rel[g() % 5] + " " + f + " " + h + "))";
    if (g() % 3 == 0) s = bvdecl("a b c", 16) + "(assert (and (" + std::string(rel[g() % 5]) + " " + f + " " + h + ") (" + rel[g() % 5] + " " + h + " " + rand_term(g, 2, 16) + ")))";
    return s;
}
}  // namespace

TEST_CASE("sim: HNSW over symbolic_metric reaches recall@10 >= 0.9 against the exact index") {
    std::mt19937_64 g(12345);
    sim::sym_index ix(kMetric);
    std::set<std::string> seen;
    std::vector<sim::sym_vec> all;
    while (all.size() < 600) {
        const auto f = rand_formula(g);
        if (!seen.insert(f).second) continue;
        std::string why;
        auto v = sim::embed_smt2(f, &why);
        if (!v) continue;
        all.push_back(*v);
        ix.add("f" + std::to_string(all.size()), *v);
    }
    ix.build();
    std::size_t hit = 0, tot = 0;
    for (std::size_t q = 0; q < 60; ++q) {
        const auto ex = ix.search_exact(all[q * 7], 10);
        const auto an = ix.search(all[q * 7], 10, 128);
        std::set<std::uint64_t> truth;
        for (const auto& n : ex) truth.insert(n.id);
        for (const auto& n : an) hit += truth.count(n.id);
        tot += ex.size();
        REQUIRE(!ex.empty());
        CHECK(ex[0].dist == 0.0f);  // the entry itself
    }
    const double recall = static_cast<double>(hit) / static_cast<double>(tot);
    MESSAGE("recall@10 = " << recall << " on " << all.size() << " formulas");
    CHECK(recall >= 0.9);
}

TEST_CASE("sim: codec and index file round trip") {
    const auto v = embed_text(bvdecl("a b") + "(assert (bvult (bvadd a b) (bvmul a b)))");
    std::vector<std::byte> bytes;
    sim::encode_sym_vec(v, bytes);
    CHECK(sim::decode_sym_vec(bytes.data(), bytes.size()) == v);
    CHECK(sim::decode_sym_vec(bytes.data(), bytes.size() / 2).feat.empty());  // truncated input is rejected

    sim::sym_index ix(kMetric);
    ix.add("one", v, "qfbv", 0.25);
    ix.add("two", embed_text(bvdecl("a") + "(assert (= a (_ bv0 32)))"), "", 1.0);
    ix.build();
    const auto path = (std::filesystem::temp_directory_path() / "prism_sim_test.idx").string();
    REQUIRE(ix.save(path));
    auto back = sim::sym_index::load(path);
    REQUIRE(back.has_value());
    CHECK(back->size() == 2);
    CHECK(back->at(0).name == "one");
    CHECK(back->at(0).label == "qfbv");
    CHECK(back->at(0).vec == v);
    CHECK(back->search(v, 1)[0].id == 0);
    std::filesystem::remove(path);
}

TEST_CASE("sim: strategy hints: kNN regression of per-strategy time is the default, the vote is an option") {
    sim::sym_index ix(kMetric);
    auto fam_a = [&](unsigned k) {
        return bvdecl("a b") + "(assert (= (bvadd (bvshl a (_ bv" + std::to_string(1 + k % 3) + " 32)) b) (bvmul a (_ bv" + std::to_string(3 + k % 2) + " 32))))";
    };
    auto fam_b = [&](unsigned k) {
        return bvdecl("a b c") + "(assert (and (bvult a b) (bvult b c) (not (= a (_ bv" + std::to_string(k % 5) + " 32)))))";
    };
    // family C: "risky" is a hair faster on 8 of 12 formulas and 5 s slower on 4; "default" never loses big
    auto fam_c = [&](unsigned k) { return bvdecl("a b") + "(assert (bvult (bvxor a (_ bv" + std::to_string(k % 4) + " 32)) (bvadd b (_ bv1 32))))"; };
    using times = std::vector<std::pair<std::string, double>>;
    for (unsigned k = 0; k < 12; ++k) {
        ix.add("a" + std::to_string(k), embed_text(fam_a(k)), "bitblast", 0.5 + 0.01 * k, times{{"bitblast", 0.5 + 0.01 * k}, {"default", 2.0}, {"smt", 3.0}});
        ix.add("b" + std::to_string(k), embed_text(fam_b(k)), "smt", 0.2, times{{"smt", 0.2}, {"default", 0.5}, {"bitblast", 5.0}});
        const bool bad = k % 3 == 2;
        ix.add("c" + std::to_string(k), embed_text(fam_c(k)), bad ? "default" : "risky", 0.001,
               times{{"risky", bad ? 5.0 : 0.001}, {"default", bad ? 0.001 : 0.0011}});
    }
    ix.build();
    sim::hint_params hp;
    hp.k = 7;
    auto ha = sim::strategy_hint(ix, embed_text(fam_a(100)), hp);
    REQUIRE(!ha.empty());
    CHECK(ha[0].strategy == "bitblast");
    CHECK(ha[0].predicted_seconds == doctest::Approx(0.5).epsilon(0.1));
    auto hb = sim::strategy_hint(ix, embed_text(fam_b(101)), hp);
    REQUIRE(!hb.empty());
    CHECK(hb[0].strategy == "smt");
    // vote option: the family-C neighbours' winners are mostly "risky", which costs 5 s on a third of them
    hp.k = 12;
    sim::hint_params vote = hp;
    vote.how = sim::hint_params::method::vote;
    auto vc = sim::strategy_hint(ix, embed_text(fam_c(100)), vote);
    REQUIRE(!vc.empty());
    CHECK(vc[0].strategy == "risky");
    auto rc = sim::strategy_hint(ix, embed_text(fam_c(100)), hp);
    REQUIRE(!rc.empty());
    CHECK(rc[0].strategy == "default");  // the regression sees the 5 s losses
    // the guard keeps the default unless the best prediction is at least 20% faster
    sim::hint_params g = hp;
    auto near_tie = sim::strategy_hint(ix, embed_text(fam_a(100)), g);  // family A: default is 4x slower, so no guard effect
    CHECK(near_tie[0].strategy == "bitblast");
    // an index without labels gives no hint; one with only a best label falls back to the vote
    sim::sym_index plain(kMetric);
    plain.add("x", embed_text(fam_a(0)));
    plain.build();
    CHECK(sim::strategy_hint(plain, embed_text(fam_a(1))).empty());
    sim::sym_index only_labels(kMetric);
    only_labels.add("x", embed_text(fam_a(0)), "bitblast", 0.5);
    only_labels.build();
    auto fl = sim::strategy_hint(only_labels, embed_text(fam_a(1)));
    REQUIRE(!fl.empty());
    CHECK(fl[0].strategy == "bitblast");
}

TEST_CASE("sim: tree edit distance ranks a one-operator change below a different formula") {
    auto t = [](const std::string& s) { auto r = sim::from_smt2(s); REQUIRE(r.has_value()); return sim::canonicalise(*r); };
    const auto a = t(bvdecl("a b") + "(assert (bvult (bvadd a b) (bvmul a b)))");
    const auto b = t(bvdecl("a b") + "(assert (bvult (bvsub a b) (bvmul a b)))");
    const auto c = t(bvdecl("a b c") + "(assert (and (bvslt a b) (bvule b c) (not (= a c))))");
    CHECK(sim::tree_edit_distance(a, a).value() == 0.0);
    const auto ab = sim::tree_edit_distance(a, b), ac = sim::tree_edit_distance(a, c);
    REQUIRE(ab.has_value());
    REQUIRE(ac.has_value());
    CHECK(*ab < *ac);
    CHECK_FALSE(sim::tree_edit_distance(a, c, 3).has_value());  // beyond the node bound
}

TEST_CASE("sim: Lean goals translate to the same sym_vec as the SMT formula") {
    auto lean = [](const char* s) {
        std::string why;
        auto t = sim::lean_goal_to_term(s, &why);
        REQUIRE_MESSAGE(t.has_value(), why);
        return sim::embed(*t);
    };
    const auto l1 = lean("theorem t (x y : BitVec 32) : x + y = y + x := by bv_decide");
    const auto s1 = embed_text(bvdecl("x y") + "(assert (= (bvadd x y) (bvadd y x)))");
    CHECK(kMetric.distance(l1, s1) == 0.0f);
    const auto l2 = lean("theorem t : \xE2\x88\x80 x : BitVec 8, x * 2#8 = x <<< 1#8");
    const auto s2 = embed_text(bvdecl("x", 8) + "(assert (= (bvmul x (_ bv2 8)) (bvshl x (_ bv1 8))))");
    CHECK(kMetric.distance(l2, s2) == 0.0f);
    // Nat / Int linear and a hypothesis
    const auto l3 = lean("theorem t (a b : Int) (h : a < b) : a + 1 <= b := by omega");
    CHECK(l3.vars == 2);
    // not translatable: reasons, not guesses
    std::string why;
    CHECK_FALSE(sim::lean_goal_to_term("theorem t (x : Real) : x + 0 = x := by simp", &why).has_value());
    CHECK(why.find("Real") != std::string::npos);
    CHECK_FALSE(sim::lean_goal_to_term("theorem t (l : List Nat) : l.length >= 0 := by simp", &why).has_value());
}


// ---------------------------------------------------------------------------------------------
// Measurements for docs/SIM_SEARCH.md (skipped unless PRISM_SIM_EVAL=<dir>).
//   PRISM_SIM_EVAL=<dir> ./prism_tests -tc="sim eval collect*"   writes <dir>/vcs/*.smt2 and <dir>/vcs.jsonl
//   PRISM_SIM_EVAL=<dir> ./prism_tests -tc="sim eval measure*"   reads them, writes <dir>/sim_eval.json
// The labelled set is PRISM's own verification conditions from the pir encoder (the conformance and pir
// sources), each solved alone by Z3 under several tactic strategies: the FastSMT setting (a strategy per formula).
// ---------------------------------------------------------------------------------------------
namespace {
const std::vector<std::string>& strategies() {
    static const std::vector<std::string> v = {"default", "qfbv", "simplify-bitblast-sat", "smt", "solveeqs-bitblast-sat", "propagate-smt"};
    return v;
}
z3::solver make_strategy(z3::context& c, const std::string& s, unsigned timeout_ms) {
    z3::solver sv = [&]() {
        if (s == "default") return z3::solver(c);
        if (s == "qfbv") return z3::tactic(c, "qfbv").mk_solver();
        if (s == "simplify-bitblast-sat") return (z3::tactic(c, "simplify") & z3::tactic(c, "bit-blast") & z3::tactic(c, "sat")).mk_solver();
        if (s == "smt") return z3::tactic(c, "smt").mk_solver();
        if (s == "solveeqs-bitblast-sat")
            return (z3::tactic(c, "simplify") & z3::tactic(c, "solve-eqs") & z3::tactic(c, "bit-blast") & z3::tactic(c, "sat")).mk_solver();
        return (z3::tactic(c, "simplify") & z3::tactic(c, "propagate-values") & z3::tactic(c, "ctx-simplify") & z3::tactic(c, "smt")).mk_solver();
    }();
    z3::params p(c);
    p.set("timeout", timeout_ms);
    sv.set(p);
    return sv;
}
bool held_out_file(const std::string& key, double share = 0.3) {
    const auto h = prism::solver::sha256_hex(key).substr(0, 8);
    return static_cast<double>(std::stoull(h, nullptr, 16) % 1000) / 1000.0 < share;
}
double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}  // namespace

TEST_CASE("sim eval collect: PRISM VCs with per-strategy Z3 times") {
    const char* e = std::getenv("PRISM_SIM_EVAL");
    if (!e || !*e) return;
    namespace fs = std::filesystem;
    const fs::path out = e;
    fs::create_directories(out / "vcs");
    const auto repo = fs::path(PRISM_SOURCE_DIR);
    std::vector<fs::path> roots;
    {
        std::ifstream in(out / "roots.txt");
        for (std::string l; std::getline(in, l);)
            if (!l.empty()) roots.push_back(repo / l);
    }
    if (roots.empty())
        for (const char* r : {"testdata/conformance", "testdata/pir", "testdata/planted", "testdata/tp", "testdata/fp", "testdata/fixtures"})
            if (fs::exists(repo / r)) roots.push_back(repo / r);
    auto cfg = prism::default_config();
    auto fe = prism::pir::find_frontend(cfg);
    REQUIRE(fe.clang);
    std::vector<fs::path> srcs;
    for (const auto& root : roots)
        for (auto& en : fs::recursive_directory_iterator(root)) {
            const auto ext = en.path().extension().string();
            if (en.is_regular_file() && (ext == ".c" || ext == ".cpp" || ext == ".i")) srcs.push_back(en.path());
        }
    std::sort(srcs.begin(), srcs.end());
    std::ofstream log(out / "vcs.jsonl", std::ios::trunc);
    const unsigned timeout_ms = 5000;
    int nvc = 0, nfiles = 0, disagreements = 0;
    std::set<std::string> seen_text;
    for (const auto& src : srcs) {
        const auto key = fs::relative(src, repo).generic_string();
        std::string err;
        std::vector<prism::pir::FoldedUb> folded;
        std::vector<std::pair<int, int>> sshl;
        auto ir = prism::pir::lower_to_ir(fe, src, 30.0, err, &folded, &sshl);
        if (!ir) continue;
        ++nfiles;
        auto mod = prism::pir::ir::parse_module(*ir);
        prism::pir::TranslateOptions topt;
        topt.signed_shl = sshl;
        topt.folded = folded;
        for (auto& irf : mod.functions) {
            auto tr = prism::pir::translate(mod, irf, topt);
            if (!tr.fn) continue;
            tr.fn->name = irf.name;
            int k = 0;
            for (auto& vc : prism::pir::pir_vcs(*tr.fn, 8)) {
                if (++k > 6) break;
                if (!seen_text.insert(vc.smt2).second) continue;  // exact duplicates kept once (counted in the measurement)
                z3::context c;
                z3::expr_vector asserts(c);
                try { asserts = c.parse_string(vc.smt2.c_str()); } catch (...) { continue; }
                nlohmann::json rec;
                rec["file"] = key;
                rec["function"] = irf.name;
                rec["vc"] = vc.kind + ":" + vc.prop;
                const std::string base = std::to_string(nvc) + ".smt2";
                std::ofstream(out / "vcs" / base) << vc.smt2;
                rec["smt2"] = "vcs/" + base;
                std::string answer;
                for (const auto& s : strategies()) {
                    double best = 1e9;
                    std::string res = "unknown";
                    for (int rep = 0; rep < 3; ++rep) {
                        auto sv = make_strategy(c, s, timeout_ms);
                        for (const auto& a : asserts) sv.add(a);
                        const double t0 = now_s();
                        z3::check_result r = z3::unknown;
                        try { r = sv.check(); } catch (...) {}
                        const double dt = now_s() - t0;
                        res = r == z3::sat ? "sat" : r == z3::unsat ? "unsat" : "unknown";
                        best = std::min(best, dt);
                        if (res == "unknown") break;
                    }
                    rec["times"][s] = {{"r", res}, {"s", res == "unknown" ? timeout_ms / 1000.0 : best}};
                    if (res != "unknown") {
                        if (!answer.empty() && answer != res) ++disagreements;
                        answer = res;
                    }
                }
                log << rec.dump() << "\n";
                ++nvc;
            }
        }
    }
    MESSAGE("collected ", nvc, " distinct VCs from ", nfiles, " files; strategy disagreements: ", disagreements);
    CHECK(disagreements == 0);
}

TEST_CASE("sim eval measure: index, recall, near-duplicates, kNN strategy selection") {
    const char* e = std::getenv("PRISM_SIM_EVAL");
    if (!e || !*e) return;
    namespace fs = std::filesystem;
    using json = nlohmann::json;
    const fs::path dir = e;
    std::vector<json> recs;
    {
        std::ifstream in(dir / "vcs.jsonl");
        for (std::string l; std::getline(in, l);)
            if (!l.empty()) recs.push_back(json::parse(l));
    }
    if (recs.empty()) return;
    json out;
    out["vcs"] = recs.size();
    // ---- embed ----
    std::vector<sim::sym_vec> vecs;
    std::vector<std::size_t> kept;
    const double t0 = now_s();
    for (std::size_t i = 0; i < recs.size(); ++i) {
        std::string why;
        auto v = sim::embed_smt2_file((dir / recs[i]["smt2"].get<std::string>()).string(), &why);
        if (!v) continue;
        vecs.push_back(std::move(*v));
        kept.push_back(i);
    }
    const double t_embed = now_s() - t0;
    out["embedded"] = vecs.size();
    out["embed_seconds"] = t_embed;
    out["embed_ms_per_formula"] = 1e3 * t_embed / static_cast<double>(std::max<std::size_t>(1, vecs.size()));
    std::size_t sem_const = 0;
    double nodes = 0;
    for (const auto& v : vecs) { sem_const += v.sem_constant; nodes += v.nodes; }
    out["sem_constant_fraction"] = static_cast<double>(sem_const) / static_cast<double>(vecs.size());
    std::size_t sub_nonempty = 0, informative = 0;
    for (const auto& v : vecs) {
        sub_nonempty += !v.sub.empty();
        informative += (!v.sem_constant || !v.sub.empty());
    }
    out["sub_nonempty_fraction"] = static_cast<double>(sub_nonempty) / static_cast<double>(vecs.size());
    out["informative_semantic_fraction"] = static_cast<double>(informative) / static_cast<double>(vecs.size());
    out["mean_nodes"] = nodes / static_cast<double>(vecs.size());
    // ---- index and ANN recall ----
    sim::sym_index ix(kMetric);
    for (std::size_t i = 0; i < vecs.size(); ++i) ix.add(recs[kept[i]]["smt2"].get<std::string>(), vecs[i]);
    const double t1 = now_s();
    ix.build();
    out["hnsw_build_seconds"] = now_s() - t1;
    const std::size_t nq = std::min<std::size_t>(vecs.size(), 300);
    for (std::size_t ef : {32u, 64u, 128u, 256u}) {
        std::size_t hit = 0, tot = 0;
        double tq = 0, te = 0;
        for (std::size_t q = 0; q < nq; ++q) {
            const auto& qv = vecs[(q * 7919) % vecs.size()];
            double a = now_s();
            const auto ex = ix.search_exact(qv, 11);  // includes itself
            double b = now_s();
            const auto an = ix.search(qv, 11, ef);
            double c2 = now_s();
            te += b - a;
            tq += c2 - b;
            std::set<std::uint64_t> truth;
            for (const auto& n : ex) truth.insert(n.id);
            for (const auto& n : an) hit += truth.count(n.id);
            tot += ex.size();
        }
        out["recall_at_10"][std::to_string(ef)] = {{"recall", static_cast<double>(hit) / static_cast<double>(tot)},
                                                   {"hnsw_ms_per_query", 1e3 * tq / static_cast<double>(nq)},
                                                   {"exact_ms_per_query", 1e3 * te / static_cast<double>(nq)}};
    }
    // ---- near duplicates (nearest other formula) ----
    std::size_t d0 = 0, d_lt05 = 0, d_lt10 = 0, sem0_struct = 0, struct0 = 0;
    for (std::size_t i = 0; i < vecs.size(); ++i) {
        const auto nn = ix.search_exact(vecs[i], 2);
        for (const auto& n : nn) {
            if (n.id == i) continue;
            const auto& o = vecs[n.id];
            d0 += n.dist == 0.0f;
            d_lt05 += n.dist < 0.05f;
            d_lt10 += n.dist < 0.10f;
            sem0_struct += (sim::semantic_distance(vecs[i], o) == 0.0 && sim::structural_distance(vecs[i], o) > 0.0);
            struct0 += sim::structural_distance(vecs[i], o) == 0.0;
            break;
        }
    }
    const double N = static_cast<double>(vecs.size());
    out["near_duplicates"] = {{"nearest_d_eq_0", d0 / N}, {"nearest_d_lt_0.05", d_lt05 / N}, {"nearest_d_lt_0.10", d_lt10 / N},
                              {"nearest_structurally_identical", struct0 / N}, {"nearest_same_semantics_different_structure", sem0_struct / N}};
    // ---- kNN strategy selection, held-out split by source file ----
    const auto& S = strategies();
    auto time_of = [&](const json& r, const std::string& s) { return r["times"][s]["s"].get<double>(); };
    auto answered = [&](const json& r, const std::string& s) { return r["times"][s]["r"].get<std::string>() != "unknown"; };
    std::vector<std::size_t> train, test;
    for (std::size_t i = 0; i < vecs.size(); ++i) (held_out_file(recs[kept[i]]["file"].get<std::string>()) ? test : train).push_back(i);
    out["train"] = train.size();
    out["test"] = test.size();
    if (!train.empty() && !test.empty()) {
        auto best_of = [&](const json& r) {
            std::string b;
            double bt = 1e18;
            for (const auto& s : S)
                if (answered(r, s) && time_of(r, s) < bt) { bt = time_of(r, s); b = s; }
            return b;
        };
        // alpha (weight of the structural part) is chosen on a validation split of the train files, never on the test set
        auto api_total = [&](float alpha, const std::vector<std::size_t>& fit, const std::vector<std::size_t>& evl) {
            sim::sym_index ix(sim::symbolic_metric{alpha});
            for (auto i : fit) {
                const auto& r = recs[kept[i]];
                std::vector<std::pair<std::string, double>> tt;
                for (const auto& s : S) tt.emplace_back(s, time_of(r, s));
                ix.add(r["smt2"].get<std::string>(), vecs[i], best_of(r), 0.0, tt);
            }
            ix.build();
            double t = 0;
            for (auto i : evl) {
                sim::hint_params hp;
                hp.k = 15;
                const auto h = sim::strategy_hint(ix, vecs[i], hp);
                t += time_of(recs[kept[i]], h.empty() ? "default" : h[0].strategy);
            }
            return t;
        };
        std::vector<std::size_t> fit, val;
        for (auto i : train) (held_out_file(recs[kept[i]]["file"].get<std::string>() + "#val", 0.25) ? val : fit).push_back(i);
        float best_alpha = 0.5f;
        double best_val = 1e18;
        json sweep = json::array();
        for (float a : {1.0f, 0.75f, 0.5f, 0.25f, 0.0f}) {
            const double tv = api_total(a, fit, val), tt = api_total(a, train, test);
            sweep.push_back({{"alpha", a}, {"validation_seconds", tv}, {"test_seconds", tt}});
            if (tv < best_val) { best_val = tv; best_alpha = a; }
        }
        out["alpha_sweep"] = sweep;
        out["alpha_chosen_on_validation"] = best_alpha;
        out["validation_default_z3_seconds"] = [&] { double t = 0; for (auto i : val) t += time_of(recs[kept[i]], "default"); return t; }();
        sim::sym_index tix(sim::symbolic_metric{best_alpha});
        std::map<std::string, double> train_total;
        for (auto i : train) {
            const auto& r = recs[kept[i]];
            const auto b = best_of(r);
            std::vector<std::pair<std::string, double>> tt;
            for (const auto& s : S) tt.emplace_back(s, time_of(r, s));
            tix.add(r["smt2"].get<std::string>(), vecs[i], b, b.empty() ? std::nan("") : time_of(r, b), tt);
            for (const auto& s : S) train_total[s] += time_of(r, s);
        }
        tix.build();
        std::string best_static;
        double bs = 1e18;
        for (const auto& s : S)
            if (train_total[s] < bs) { bs = train_total[s]; best_static = s; }
        double t_default = 0, t_static = 0, t_vote = 0, t_oracle = 0, t_api = 0, t_noguard = 0;
        double lg_api = 0, lg_vote = 0, lg_oracle = 0;  // sum of log((t + 1 ms) / (t_default + 1 ms)): the typical formula, not the tail
        std::size_t api_slower = 0, api_faster = 0, vote_slower = 0, vote_faster = 0;
        std::vector<std::pair<double, std::string>> api_losses;  // (seconds lost vs default, description)
        std::size_t api_correct = 0, api_deviations = 0;
        std::map<std::string, double> t_each;
        for (auto i : test) {
            const auto& r = recs[kept[i]];
            sim::hint_params hp;  // the API default: regression with the 20% guard
            hp.k = 15;
            const auto h = sim::strategy_hint(tix, vecs[i], hp);
            const std::string pick = h.empty() ? "default" : h[0].strategy;
            sim::hint_params ng = hp;
            ng.default_strategy.clear();
            const auto hn = sim::strategy_hint(tix, vecs[i], ng);
            sim::hint_params vp = hp;
            vp.how = sim::hint_params::method::vote;
            const auto hv = sim::strategy_hint(tix, vecs[i], vp);
            t_default += time_of(r, "default");
            t_static += time_of(r, best_static);
            t_api += time_of(r, pick);
            t_noguard += time_of(r, hn.empty() ? "default" : hn[0].strategy);
            t_vote += time_of(r, hv.empty() ? "default" : hv[0].strategy);
            t_oracle += time_of(r, best_of(r));
            {
                const double td = time_of(r, "default") + 1e-3;
                const std::string vpick = hv.empty() ? "default" : hv[0].strategy;
                const double ta = time_of(r, pick) + 1e-3, tv = time_of(r, vpick) + 1e-3, to = time_of(r, best_of(r)) + 1e-3;
                lg_api += std::log(ta / td);
                lg_vote += std::log(tv / td);
                lg_oracle += std::log(to / td);
                api_slower += ta > 1.25 * td;
                api_faster += ta < td / 1.25;
                vote_slower += tv > 1.25 * td;
                vote_faster += tv < td / 1.25;
                if (ta - td > 0.05) api_losses.emplace_back(ta - td, r["smt2"].get<std::string>() + " picked " + pick);
            }
            for (const auto& s : S) t_each[s] += time_of(r, s);
            api_deviations += pick != "default";
            api_correct += pick == best_of(r);
        }
        out["strategy_selection"] = {{"strategies", S}, {"best_static_on_train", best_static}, {"test_seconds_default_z3", t_default},
                                     {"test_seconds_best_static", t_static}, {"test_seconds_vote", t_vote},
                                     {"test_seconds_regression_unguarded", t_noguard}, {"test_seconds_api_default_regression_guarded", t_api},
                                     {"api_deviations_from_default", api_deviations}, {"test_seconds_oracle", t_oracle},
                                     {"geomean_time_ratio_vs_default", {{"api", std::exp(lg_api / static_cast<double>(test.size()))},
                                                                        {"vote", std::exp(lg_vote / static_cast<double>(test.size()))},
                                                                        {"oracle", std::exp(lg_oracle / static_cast<double>(test.size()))}}},
                                     {"formulas_over_25pct_slower_than_default", {{"api", api_slower}, {"vote", vote_slower}}},
                                     {"formulas_over_25pct_faster_than_default", {{"api", api_faster}, {"vote", vote_faster}}},
                                     {"api_losses_over_50ms", api_losses},
                                     {"test_seconds_per_strategy", t_each},
                                     {"api_pick_is_oracle", static_cast<double>(api_correct) / static_cast<double>(test.size())}};
    }
    std::ofstream(dir / "sim_eval.json") << out.dump(2) << "\n";
    MESSAGE(out.dump(2));
}

#endif  // PRISM_HAS_Z3
