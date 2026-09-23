// Certified mode with the Lean-proved bit-blaster (roadmap 3.2 step 2, 5.4,
// 8.2 "Bit-blaster" and "Certificate checking").
//
// The serializer round trip (Z3 formula vs. its S-expression, on random
// assignments) always runs: against the C++ reference evaluator, and against
// the Lean semantics (`prism-bitblast --eval`) when proofs/techniques is
// built. The certification tests need CaDiCaL, cake_lpr and the two Lean
// executables; without them they assert the honest fallback instead.

#include <doctest/doctest.h>

#include "prism/solver.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#if defined(PRISM_HAS_Z3) && !defined(_WIN32)

namespace {

namespace ps = prism::solver;
namespace fs = std::filesystem;

struct LeanTmp {
    fs::path dir;
    LeanTmp() {
        static int n = 0;
        dir = fs::temp_directory_path() /
              ("prism-leanbb-test-" + std::to_string(std::random_device{}()) + "-" + std::to_string(n++));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~LeanTmp() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

ps::SolveOptions lean_opts(const LeanTmp& t) {
    ps::SolveOptions o;
    o.cache_dir = (t.dir / "cache").string();
    o.timeout_s = 60;
    o.use_cache = false;
    return o;
}

bool have_lean_tools() {
    ps::SolveOptions o;
    return ps::find_tool("prism-bitblast", o) && ps::find_tool("prism-lrat-check", o);
}

bool have_cert_chain() {
    ps::SolveOptions o;
    return have_lean_tools() && ps::find_tool("cadical", o) && ps::find_tool("cake_lpr", o);
}

// A value literal for a random or boundary assignment.
std::string rand_literal(std::mt19937_64& rng, unsigned w, bool is_bool) {
    if (is_bool) return (rng() & 1) ? "true" : "false";
    std::vector<bool> bits(w);
    const unsigned pick = unsigned(rng() % 8);
    for (unsigned i = 0; i < w; ++i) {
        bool b = rng() & 1;
        if (pick == 0) b = false;                  // 0
        if (pick == 1) b = i == 0;                 // 1
        if (pick == 2) b = true;                   // -1 / all ones
        if (pick == 3) b = i + 1 == w;             // INT_MIN
        if (pick == 4) b = i + 1 != w;             // INT_MAX
        if (pick == 5 && i > 3) b = false;         // small
        bits[i] = b;
    }
    std::string s = "#b";
    for (unsigned i = w; i-- > 0;) s += bits[i] ? '1' : '0';
    return s;
}

// Run `prism-bitblast --eval` on the DAG and the assignments; one 0/1 per line.
std::vector<int> lean_eval(const ps::ToolInfo& bb, const ps::LeanDag& dag, const std::vector<std::string>& rhos,
                           const fs::path& dir) {
    const fs::path in = dir / "eval.sexp", out = dir / "eval.out";
    {
        std::ofstream f(in);
        f << dag.text;
        for (const auto& r : rhos) f << r << "\n";
    }
    std::string cmd = "'" + bb.path.string() + "' --eval < '" + in.string() + "' > '" + out.string() + "'";
    std::vector<int> res;
    if (std::system(cmd.c_str()) != 0) return res;
    std::ifstream f(out);
    std::string line;
    while (std::getline(f, line)) res.push_back(line == "1" ? 1 : line == "0" ? 0 : -1);
    return res;
}

// The formulas the round trip covers: every operator the serializer maps,
// in the shapes the PIR encoder (src/prism/pir/encode.cpp) builds.
std::vector<std::pair<std::string, z3::expr>> fragment_formulas(z3::context& c) {
    std::vector<std::pair<std::string, z3::expr>> fs;
    for (unsigned w : {8u, 13u, 32u, 64u}) {
        auto x = c.bv_const(("x" + std::to_string(w)).c_str(), w);
        auto y = c.bv_const(("y" + std::to_string(w)).c_str(), w);
        auto z = c.bv_const(("z" + std::to_string(w)).c_str(), w);
        auto p = c.bool_const(("p" + std::to_string(w)).c_str());
        auto zero = c.bv_val(0, w), one = c.bv_val(1, w);
        auto W = std::to_string(w);
        auto sx = z3::sext(x, 1), sy = z3::sext(y, 1), zx = z3::zext(x, 1), zy = z3::zext(y, 1);
        fs.push_back({"add/sub/neg/mul/eq " + W, (x + y) * z - (-x) == y * (z + one)});
        fs.push_back({"sadd overflow (PIR shape) " + W, sx + sy != z3::sext(x + y, 1)});
        fs.push_back({"ssub overflow (PIR shape) " + W, sx - sy != z3::sext(x - y, 1)});
        fs.push_back({"uadd overflow (PIR shape) " + W, zx + zy != z3::zext(x + y, 1)});
        fs.push_back({"usub overflow " + W, z3::ult(x, y)});
        fs.push_back({"smul overflow (PIR shape) " + W,
                      !(z3::bvmul_no_overflow(x, y, true) && z3::bvmul_no_underflow(x, y))});
        fs.push_back({"umul overflow " + W, !z3::bvmul_no_overflow(x, y, false)});
        fs.push_back({"udiv/urem " + W, z3::udiv(x, y) + z3::urem(x, y) == z});
        fs.push_back({"sdiv/srem " + W, x / y - z3::srem(x, y) == z});
        fs.push_back({"div by zero " + W, z3::udiv(x, zero) == z || z3::srem(x, zero) == y});
        fs.push_back({"shifts by a variable " + W,
                      z3::shl(x, y) + z3::lshr(x, z) == z3::ashr(y, x)});
        fs.push_back({"shifts by a constant " + W,
                      z3::shl(x, c.bv_val(3, w)) + z3::lshr(x, c.bv_val(w - 1, w)) == z3::ashr(y, one)});
        fs.push_back({"comparisons " + W, z3::ule(x, y) && (x >= z) && !(y < z) && z3::ugt(z, x) ||
                                              (x <= y && z3::uge(y, z))});
        fs.push_back({"bitwise " + W, (((x & y) | ~z) ^ (y | z)) == z3::nand(x, z) || z3::xnor(x, y) == z3::nor(y, z)});
        fs.push_back({"ite / bool ops " + W,
                      z3::ite(p, x, y) == z && (z3::implies(p, x == y) || (p ^ (x != z)))});
        {
            z3::expr_vector v(c);
            v.push_back(x + one);
            v.push_back(y);
            v.push_back(z * y);
            fs.push_back({"distinct " + W, z3::distinct(v) || x != y});
        }
        fs.push_back({"extract/concat/zext/sext " + W,
                      z3::concat(x.extract(w - 1, w / 2), y.extract(w / 2 - 1, 0)) ==
                              z3::sext(z.extract(w / 2, 1), w - w / 2) ||
                          z3::zext(x, 3).extract(w + 2, 2) == z3::sext(y, 5).extract(w + 3, 3)});
        fs.push_back({"rotate/repeat " + W,
                      z3::expr(c, Z3_mk_rotate_left(c, 3, x)) == z3::expr(c, Z3_mk_rotate_right(c, 5, y)) ||
                          z3::expr(c, Z3_mk_repeat(c, 2, x.extract(3, 0))) == z3::expr(c, Z3_mk_repeat(c, 2, z.extract(3, 0)))});
        fs.push_back({"redor/redand " + W,
                      z3::expr(c, Z3_mk_bvredor(c, z)) == z3::expr(c, Z3_mk_bvredand(c, x)) ||
                          z3::expr(c, Z3_mk_bvredand(c, y)) == c.bv_val(1, 1)});
        // Sharing: a subterm used several times becomes one definition.
        auto s = (x * y + z) ^ (x - y);
        fs.push_back({"shared subterms " + W, (s + s) * s == z3::udiv(s, z | one) + s});
    }
    return fs;
}

}  // namespace

TEST_CASE("leanbb: the serializer round-trips against Z3 on random assignments") {
    LeanTmp t;
    z3::context c;
    std::mt19937_64 rng(20260923);
    ps::SolveOptions def;
    auto bb = ps::find_tool("prism-bitblast", def);
    std::size_t lean_checked = 0;
    for (const auto& [name, f] : fragment_formulas(c)) {
        std::string why;
        auto dag = ps::to_lean_dag(f, &why);
        REQUIRE_MESSAGE(dag, name << ": " << why);
        std::vector<std::map<std::string, std::string>> models;
        std::vector<std::string> rhos;
        std::vector<bool> z3_vals;
        for (int k = 0; k < 40; ++k) {
            std::map<std::string, std::string> m;
            for (const auto& in : dag->inputs) m[in.name] = rand_literal(rng, in.width, in.is_bool);
            // Z3's evaluator is the reference: validate_model is true iff f evaluates to true.
            const bool zv = ps::validate_model(c, f, m);
            std::string ewhy;
            auto cv = ps::eval_lean_dag(*dag, m, &ewhy);
            if (cv) {
                CHECK_MESSAGE(*cv == zv, name << " (C++ reference evaluator) differs from Z3 on assignment " << k
                                               << "\n" << dag->text);
            } else {
                CHECK_MESSAGE(ewhy.find("above 64 bits") != std::string::npos, name << ": " << ewhy);
            }
            auto rho = ps::lean_rho(*dag, m);
            REQUIRE(rho);
            rhos.push_back(*rho);
            models.push_back(m);
            z3_vals.push_back(zv);
        }
        if (bb) {
            // The Lean semantics itself (Dag.eval of the parsed formula).
            auto lv = lean_eval(*bb, *dag, rhos, t.dir);
            REQUIRE_MESSAGE(lv.size() == rhos.size(), name << ": prism-bitblast --eval failed");
            for (std::size_t k = 0; k < lv.size(); ++k)
                CHECK_MESSAGE(lv[k] == (z3_vals[k] ? 1 : 0),
                              name << " (Lean Dag.eval) differs from Z3 on assignment " << k << "\n" << dag->text);
            ++lean_checked;
        }
    }
    if (bb) CHECK(lean_checked == fragment_formulas(c).size());
    // bvcomp has no C API constructor: parse it from SMT-LIB.
    {
        auto v = c.parse_string("(declare-const ca (_ BitVec 8)) (declare-const cb (_ BitVec 8))"
                                "(assert (= (bvcomp ca cb) (bvcomp cb (bvadd ca #x01))))");
        REQUIRE(v.size() == 1);
        auto dag = ps::to_lean_dag(v[0]);
        REQUIRE(dag);
        for (int k = 0; k < 40; ++k) {
            std::map<std::string, std::string> m{{"ca", rand_literal(rng, 8, false)},
                                                 {"cb", rand_literal(rng, 8, false)}};
            auto cv = ps::eval_lean_dag(*dag, m);
            REQUIRE(cv);
            CHECK(*cv == ps::validate_model(c, v[0], m));
        }
    }
    // Shared subterms are written once, as definitions.
    auto x = c.bv_const("sx", 16), y = c.bv_const("sy", 16);
    auto s = x * y;
    auto dag = ps::to_lean_dag((s + s) * s == y);
    REQUIRE(dag);
    CHECK(dag->defs >= 1);
    CHECK(dag->text.find("(def 16 32 ") != std::string::npos);  // above both inputs
}

TEST_CASE("leanbb: a formula outside the proved fragment falls back to Z3 with a note") {
    LeanTmp t;
    z3::context c;
    auto x = c.bv_const("x", 8), y = c.bv_const("y", 8);
    // bvsmod is not in the proved fragment.
    auto f = z3::smod(x, y) != z3::smod(x, y);
    std::string why;
    CHECK_FALSE(ps::to_lean_dag(f, &why));
    CHECK(why.find("bvsmod") != std::string::npos);
    auto o = lean_opts(t);
    o.certified = true;
    auto r = ps::solve(c, f, o);
    REQUIRE(r.kind == ps::SolveResult::Unsat);
    CHECK(r.note.find("bitblast: z3 tactics, unproved (Lean bit-blaster not used: formula outside the proved "
                      "fragment: operator bvsmod") != std::string::npos);
    if (r.certified) {
        CHECK(r.certificate_info.find("bitblast: z3 tactics, unproved") != std::string::npos);
        CHECK(r.certificate_info.find("lean-proved") == std::string::npos);
    }
    // Missing Lean tools: the same honest fallback (NOTRUN), never silent.
    auto g = z3::ult(x, c.bv_val(255, 8)) && !z3::ugt(x + 1, x);
    auto home = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".");
    o.search_default_tools = false;
    o.tool_dirs = {home + "/.prism/tools"};
    auto m = ps::solve(c, g, o);
    REQUIRE(m.kind == ps::SolveResult::Unsat);
    if (!ps::find_tool("prism-bitblast", o))
        CHECK(m.note.find("prism-bitblast not found (NOTRUN") != std::string::npos);
    // bitblaster=z3 is honoured and said.
    o = lean_opts(t);
    o.certified = true;
    o.bitblaster = ps::Bitblaster::Z3;
    auto z = ps::solve(c, g, o);
    CHECK(z.note.find("bitblast: z3 tactics (bitblaster=z3)") != std::string::npos);
}

TEST_CASE("leanbb: small overflow VCs are PROVED-CERTIFIED through the Lean-proved bit-blaster") {
    LeanTmp t;
    z3::context c;
    auto x = c.bv_const("x", 8), y = c.bv_const("y", 8);
    auto a = c.bv_const("a", 16), b = c.bv_const("b", 16);
    auto u = c.bv_const("u", 8), v = c.bv_const("v", 8);
    auto range = [&](const z3::expr& e, int lo, int hi, unsigned w) {
        return e >= c.bv_val(lo, w) && e <= c.bv_val(hi, w);
    };
    const std::vector<std::pair<std::string, z3::expr>> vcs = {
        // x, y in [0, 60]: x + y cannot overflow int8 (the PIR sadd shape).
        {"sadd", range(x, 0, 60, 8) && range(y, 0, 60, 8) && z3::sext(x, 1) + z3::sext(y, 1) != z3::sext(x + y, 1)},
        // a, b in [-100, 100]: a * b cannot overflow int16.
        {"smul", range(a, -100, 100, 16) && range(b, -100, 100, 16) &&
                     !(z3::bvmul_no_overflow(a, b, true) && z3::bvmul_no_underflow(a, b))},
        // a - b with a >= 0 and b >= 0 never overflows.
        {"ssub", a >= c.bv_val(0, 16) && b >= c.bv_val(0, 16) &&
                     z3::sext(a, 1) - z3::sext(b, 1) != z3::sext(a - b, 1)},
        // v != 0: u / v <= u, and the remainder is below the divisor.
        {"udiv", v != c.bv_val(0, 8) && (z3::ugt(z3::udiv(u, v), u) || z3::uge(z3::urem(u, v), v))},
        // a shift by an amount below the width is a multiplication.
        {"shl", z3::ult(y, c.bv_val(3, 8)) && z3::shl(x, y) != x * z3::shl(c.bv_val(1, 8), y)},
    };
    for (const auto& [name, f] : vcs) {
        auto o = lean_opts(t);
        o.certified = true;
        o.keep_artifacts = true;
        o.work_dir = (t.dir / ("work-" + name)).string();
        auto r = ps::solve(c, f, o);
        REQUIRE_MESSAGE(r.kind == ps::SolveResult::Unsat, name << ": " << r.note);
        if (!have_cert_chain()) {
            CHECK_FALSE(r.certified);
            continue;
        }
        REQUIRE_MESSAGE(r.certified, name << ": " << r.note);
        CHECK(ps::verdict_status(r) == ps::kProvedCertified);
        CHECK(r.certificate_info.find("bitblast: lean-proved (toCNF_equisat)") != std::string::npos);
        CHECK(r.certificate_info.find("checked by cake_lpr") != std::string::npos);
        CHECK(r.certificate_info.find("Lean's verified LRAT checker") != std::string::npos);
        CHECK(r.note.find("accepted by cake_lpr and Lean's LRAT checker") != std::string::npos);
        // The kept CNF is exactly what prism-bitblast wrote for the kept formula.
        const fs::path w = o.work_dir;
        CHECK(fs::exists(w / "query.dag"));
        CHECK(r.certificate_info.find("cnf sha256 " + ps::sha256_file(w / "query.cnf")) != std::string::npos);
    }
    if (!have_cert_chain()) return;
    // A counterexample found on the Lean CNF maps back through the variable
    // map and is validated in Z3.
    auto o = lean_opts(t);
    o.bitblaster = ps::Bitblaster::Lean;
    o.z3_in_process = false;
    o.sls = false;
    auto s = ps::solve(c, x * y == c.bv_val(35, 8) && z3::ugt(x, c.bv_val(1, 8)) && z3::ugt(y, c.bv_val(1, 8)), o);
    REQUIRE_MESSAGE(s.kind == ps::SolveResult::Sat, s.note);
    CHECK(s.note.find("bitblast: lean-proved") != std::string::npos);
    CHECK(ps::validate_model(c, x * y == c.bv_val(35, 8), s.model));
}

TEST_CASE("leanbb: a tampered LRAT proof is rejected by Lean's checker") {
    if (!have_cert_chain()) return;
    LeanTmp t;
    z3::context c;
    auto x = c.bv_const("x", 8), y = c.bv_const("y", 8);
    auto f = z3::sext(x, 1) + z3::sext(y, 1) != z3::sext(x + y, 1) && x >= c.bv_val(0, 8) &&
             y >= c.bv_val(0, 8) && x <= c.bv_val(50, 8) && y <= c.bv_val(50, 8);
    auto o = lean_opts(t);
    o.certified = true;
    o.keep_artifacts = true;
    o.work_dir = (t.dir / "work").string();
    auto r = ps::solve(c, f, o);
    REQUIRE(r.kind == ps::SolveResult::Unsat);
    REQUIRE_MESSAGE(r.certified, r.note);
    const fs::path w = o.work_dir, dag = w / "query.dag", cnf = w / "query.cnf", lrat = w / "proof.lrat";
    auto chk = *ps::find_tool("prism-lrat-check", o);
    auto good = ps::check_lrat_dag(chk, dag, cnf, lrat, 60);
    CHECK(good.ran);
    CHECK(good.verified);
    CHECK(ps::check_lrat(chk, cnf, lrat, 60).verified);  // DIMACS mode agrees
    // Drop the empty-clause step: both modes refuse.
    const std::string proof = slurp(lrat);
    {
        auto cut = proof.rfind('\n', proof.size() - 2);
        std::ofstream(lrat, std::ios::trunc) << proof.substr(0, cut + 1);
    }
    auto bad = ps::check_lrat_dag(chk, dag, cnf, lrat, 60);
    CHECK(bad.ran);
    CHECK_FALSE(bad.verified);
    auto bad2 = ps::check_lrat(chk, cnf, lrat, 60);
    CHECK(bad2.ran);
    CHECK_FALSE(bad2.verified);
    // Keep the empty-clause step but strip its hints: still refused.
    {
        const auto last = proof.rfind('\n', proof.size() - 2);
        std::istringstream ls(proof.substr(last + 1));
        std::string id;
        ls >> id;
        std::ofstream(lrat, std::ios::trunc) << proof.substr(0, last + 1) << id << " 0 0\n";
        auto bad3 = ps::check_lrat_dag(chk, dag, cnf, lrat, 60);
        CHECK(bad3.ran);
        CHECK_FALSE(bad3.verified);
    }
    // The --dag mode refuses a CNF that is not the proved bit-blaster's CNF,
    // even with the original (valid) proof.
    std::ofstream(lrat, std::ios::trunc) << proof;
    {
        std::string cnf_txt = slurp(cnf);
        std::ofstream(cnf, std::ios::trunc) << "c edited\n" << cnf_txt;
    }
    auto edited = ps::check_lrat_dag(chk, dag, cnf, lrat, 60);
    CHECK_FALSE(edited.verified);
    // End to end: a CaDiCaL that corrupts its proof never yields PROVED-CERTIFIED.
    auto real = ps::find_tool("cadical", o);
    const fs::path wrap = t.dir / "tools" / "cadical" / "tampered" / "bin" / "cadical";
    fs::create_directories(wrap.parent_path());
    std::ofstream(wrap) << "#!/bin/sh\n\"" + real->path.string() + "\" \"$@\"\nrc=$?\n"
                        << "for a in \"$@\"; do p=\"$a\"; done\n"
                        << "case \"$p\" in *.lrat) sed -i '$d' \"$p\" ;; esac\nexit $rc\n";
    fs::permissions(wrap, fs::perms::owner_all);
    auto o2 = lean_opts(t);
    o2.certified = true;
    o2.tool_dirs = {(t.dir / "tools").string()};
    auto r2 = ps::solve(c, f, o2);
    CHECK(r2.kind == ps::SolveResult::Unsat);
    CHECK_FALSE(r2.certified);
    CHECK(ps::verdict_status(r2) == "PROVED");
    CHECK(r2.note.find("bitblast: lean-proved") != std::string::npos);
    CHECK(r2.note.find("not certified") != std::string::npos);
}

TEST_CASE("leanbb: a cached Lean certificate is re-checked by both checkers") {
    if (!have_cert_chain()) return;
    LeanTmp t;
    z3::context c;
    auto x = c.bv_const("x", 8);
    auto f = z3::ult(x, c.bv_val(255, 8)) && !z3::ugt(x + 1, x);
    auto o = lean_opts(t);
    o.use_cache = true;
    o.certified = true;
    auto first = ps::solve(c, f, o);
    REQUIRE(first.certified);
    auto again = ps::solve(c, f, o);
    CHECK(again.cache_hit);
    CHECK(again.certified);
    CHECK(again.certificate_info.find("Lean's verified LRAT checker") != std::string::npos);
    // A request with the other bit-blaster does not reuse it.
    o.bitblaster = ps::Bitblaster::Z3;
    auto z = ps::solve(c, f, o);
    CHECK_FALSE(z.cache_hit);
    CHECK(z.note.find("made with the lean bit-blaster") != std::string::npos);
}

#endif  // PRISM_HAS_Z3 && !_WIN32
