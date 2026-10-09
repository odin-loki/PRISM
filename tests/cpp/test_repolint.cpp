// Repository text locks for Lean proofs, the pir encoder, the solver library
// and docs. Ports tests/test_proofs_loopcut.py, tests/test_proofs_float_conc.py
// and tests/test_solver.py.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// LF-only text for line-oriented locks (CRLF checkouts on Windows).
std::string slurp_lf(const fs::path& p) {
    auto s = slurp(p);
    std::erase(s, '\r');
    return s;
}

std::string norm_ws(std::string s) {
    std::string out;
    bool sp = false;
    for (char c : s) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            if (!sp && !out.empty()) out += ' ';
            sp = true;
        } else {
            out += c;
            sp = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string strip_lean_comments(std::string text) {
    std::erase(text, '\r');
    text = std::regex_replace(text, std::regex("--[^\n]*"), "");
    text = std::regex_replace(text, std::regex("/-.*?-/", std::regex::extended), "");
    return text;
}

bool lean_has_axiom_decl(const std::string& stripped) {
    for (std::size_t i = 0; i < stripped.size();) {
        auto j = stripped.find('\n', i);
        if (j == std::string::npos) j = stripped.size();
        auto line = stripped.substr(i, j - i);
        std::size_t k = 0;
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
        if (line.compare(k, 5, "axiom") == 0) return true;
        i = j == stripped.size() ? stripped.size() : j + 1;
    }
    return false;
}

std::string body_between(const std::string& text, const std::string& start, const std::string& end) {
    auto i = text.find(start);
    REQUIRE(i != std::string::npos);
    auto j = text.find(end, i + start.size());
    REQUIRE(j != std::string::npos);
    return text.substr(i, j - i);
}

void check_contains_norm(const std::string& hay, const std::string& needle) {
    CHECK(hay.find(norm_ws(needle)) != std::string::npos);
}

}  // namespace

TEST_CASE("repolint: loop-cut Lean model theorems and audit") {
    const auto loopcut = slurp_lf(repo() / "proofs" / "techniques" / "PrismTechniques" / "LoopCut.lean");
    const auto stripped = strip_lean_comments(loopcut);
    const auto audit = slurp_lf(repo() / "proofs" / "techniques" / "PrismTechniques" / "Audit.lean");
    for (const char* thm :
         {"encViol_iff", "encExit_iff", "old_viol_imp", "s9_old_encoding_misses", "exec_cut", "cexec_grun",
          "grun_complete", "loopcut_sound", "houdini_loopcut_sound"}) {
        CHECK(loopcut.find(std::string("theorem ") + thm) != std::string::npos);
        CHECK(audit.find(std::string("#assert_axioms LoopCut.") + thm) != std::string::npos);
    }
    CHECK(slurp(repo() / "proofs" / "techniques" / "PrismTechniques.lean").find("import PrismTechniques.LoopCut") !=
          std::string::npos);
    CHECK_FALSE(std::regex_search(stripped, std::regex("\\b(sorry|admit|native_decide|bv_decide)\\b")));
    CHECK_FALSE(lean_has_axiom_decl(stripped));
    for (const char* ctor :
         {"| loop (L : Nat) (body : Prog S)", "| brk", "| cont", "| ret"}) {
        CHECK(loopcut.find(ctor) != std::string::npos);
    }
    CHECK(loopcut.find("(¬ v ∧ ¬ q1 → Inv L e s)") != std::string::npos);
}

TEST_CASE("repolint: loop-cut encoder parity with encode.cpp and houdini.inc") {
    const auto enc = slurp(repo() / "src" / "prism" / "pir" / "encode.cpp");
    const auto hou = slurp(repo() / "src" / "prism" / "pir" / "houdini.inc");
    CHECK(enc.find("case Stmt::Check: props.push_back(PropInst{&s, id, r && is1(lookup(s.args[0], id))}); break;") !=
          std::string::npos);
    CHECK(enc.find("case Stmt::Assume: r = r && is1(lookup(s.args[0], id)); break;") != std::string::npos);
    CHECK(enc.find("exit_reach[static_cast<std::size_t>(id)] = r;") != std::string::npos);
    CHECK(enc.find("const auto& r = exit_reach[static_cast<std::size_t>(from)];") != std::string::npos);
    std::regex guarded(
        R"(z3::implies\(\s*\*sel\[i\],\s*z3::implies\(e\.reach\[static_cast<std::size_t>\(CL\.node\)\]\s*&&\s*noviol\(CL\.props_before\))");
    auto begin = std::sregex_iterator(hou.begin(), hou.end(), guarded);
    CHECK(std::distance(begin, std::sregex_iterator()) >= 2);
    CHECK(hou.find("[&](std::size_t M) { return e.cut_loops[M].seq < seqL; }") != std::string::npos);
    CHECK(hou.find("[&](std::size_t M) { return e.cut_loops[M].seq <= seqT; }") != std::string::npos);
    CHECK(hou.find("e.reach[static_cast<std::size_t>(CL.node)] && noviol(CL.props_before)") != std::string::npos);
    CHECK(hou.find("lt.guard && noviol(lt.props_before)") != std::string::npos);
    CHECK(hou.find("selectors([](std::size_t) { return true; }, all_sel);") != std::string::npos);
}

TEST_CASE("repolint: float checks mirror FloatOps.lean") {
    const auto cpp = slurp(repo() / "src" / "prism" / "pir" / "translate_fp.cpp");
    const auto checks = norm_ws(body_between(cpp, "void FpTr::checks(", "\nbool FpTr::inst("));
    const auto lean = norm_ws(slurp(repo() / "proofs" / "refinement" / "PrismRefine" / "FloatOps.lean"));
    for (const char* expr :
         {"Arg n = p(b, Op::FIsNaN, {a});", "Arg fin = p(b, Op::Xor, {p(b, Op::Or, {n, p(b, Op::FIsInf, {a})}), Arg::c(1, 1)});",
          "any_nan = any_nan ? p(b, Op::Or, {*any_nan, n}) : n;",
          "all_finite = all_finite ? p(b, Op::And, {*all_finite, fin}) : fin;",
          "if (op == Op::FDiv) { divz = p(b, Op::FIsZero, {args[1]});",
          R"(t_.check(b, p(b, Op::And, {*divz, p(b, Op::Xor, {p(b, Op::FIsNaN, {args[0]}), Arg::c(1, 1)})}), "fp-div0", "FLOAT-DIV-ZERO",)",
          R"(t_.check(b, p(b, Op::And, {p(b, Op::FIsNaN, {r}), p(b, Op::Xor, {*any_nan, Arg::c(1, 1)})}), "fp-invalid", "FLOAT-INVALID",)",
          "Arg ovf = p(b, Op::And, {p(b, Op::FIsInf, {r}), *all_finite});",
          "if (divz) ovf = p(b, Op::And, {ovf, p(b, Op::Xor, {*divz, Arg::c(1, 1)})});",
          R"(t_.check(b, ovf, "fp-overflow", "FLOAT-OVERFLOW",)"}) {
        check_contains_norm(checks, expr);
    }
    for (const char* d :
         {"def pFin (a : Val) : Bool := (a.isNaN || a.isInf) ^^ true",
          "def prismDivZero (x y : Val) : Bool := y.isZero && (x.isNaN ^^ true)",
          "def prismInvalid (x y r : Val) : Bool := r.isNaN && ((x.isNaN || y.isNaN) ^^ true)",
          "def prismOverflow (op : BinOp) (x y r : Val) : Bool := if op = .div then (r.isInf && (pFin x && pFin y)) && "
          "(y.isZero ^^ true) else r.isInf && (pFin x && pFin y)"}) {
        check_contains_norm(lean, d);
    }
    const auto bin_map = body_between(cpp, "static const std::map<std::string, Op> bin{", "};");
    for (const auto& pair : std::vector<std::pair<const char*, const char*>>{{"fadd", "FAdd"}, {"fsub", "FSub"},
                                                                            {"fmul", "FMul"}, {"fdiv", "FDiv"}})
        CHECK(bin_map.find(std::string("{\"") + pair.first + "\", Op::" + pair.second + "}") != std::string::npos);
    CHECK(cpp.find("checks(cur, it->second, w, {a, b}, r, c.line);") != std::string::npos);
    const auto audit = slurp_lf(repo() / "proofs" / "refinement" / "Audit.lean");
    for (const char* thm :
         {"roundQ_nearest", "roundF_correct", "sub_correct", "mul_correct", "div_correct", "prism_overflow_eq",
          "prism_invalid_eq", "ieee_invalid_eq", "prism_divzero_eq", "ieee_divzero_imp_prism", "cast_ovf_iff"}) {
        CHECK(std::regex_search(lean, std::regex(std::string("theorem ") + thm + "\\b")));
        CHECK(audit.find(std::string("#print axioms PrismRefine.Float.") + thm + "\n") != std::string::npos);
    }
}

TEST_CASE("repolint: float cast encoder and interpreter mirror Lean") {
    const auto enc_fp = norm_ws(body_between(slurp(repo() / "src" / "prism" / "pir" / "encode.cpp"), "case Op::FToSIOvf:",
                                             "case Op::FLibm:"));
    for (const char* expr :
         {"auto t = z3::expr(c, Z3_mk_fpa_round_to_integral(c, rm(Z3_mk_fpa_rtz(c)), x));",
          "z3::expr lo = s.op == Op::FToSIOvf ? fnum(-std::ldexp(1.0, static_cast<int>(k) - 1), fw) "
          ": z3::expr(c, Z3_mk_fpa_zero(c, fsort(fw), false));",
          "z3::expr hi = fnum(std::ldexp(1.0, static_cast<int>(s.op == Op::FToSIOvf ? k - 1 : k)), fw);",
          "auto below = s.op == Op::FToSIOvf ? flt(t, lo) : (flt(t, lo));",
          "auto above = z3::expr(c, Z3_mk_fpa_geq(c, t, hi));",
          "return b2bv(is_nan(x) || is_inf(x) || below || above);"}) {
        check_contains_norm(enc_fp, expr);
    }
    const auto fp = norm_ws(body_between(slurp(repo() / "src" / "prism" / "pir" / "fp.cpp"), "case Op::FToSIOvf:",
                                         "case Op::FLibm:"));
    for (const char* expr :
         {"if (std::isnan(x) || std::isinf(x)) return 1;", "double t = std::trunc(x);",
          "if (op == Op::FToSIOvf) return t < -std::ldexp(1.0, k - 1) || t >= std::ldexp(1.0, k - 1);",
          "return t < 0 || t >= std::ldexp(1.0, k);"}) {
        check_contains_norm(fp, expr);
    }
    const auto lean = norm_ws(slurp(repo() / "proofs" / "refinement" / "PrismRefine" / "FloatOps.lean"));
    check_contains_norm(lean,
                        "def prismCastOvf (f : Fmt) (signed : Bool) (k : Nat) (x : Val) : Bool := let t := rtz f x let lo "
                        ":= if signed then (pow2Val f (k - 1)).neg else .fin false 0 let hi := pow2Val f (if signed then "
                        "k - 1 else k) x.isNaN || x.isInf || Val.lt t lo || Val.le hi t");
}

TEST_CASE("repolint: lazy.cpp schedule mirrors LazySeqN.lean") {
    const auto cpp = norm_ws(slurp(repo() / "src" / "prism" / "conc" / "lazy.cpp"));
    check_contains_norm(cpp,
                        "for (int r = 0; r < opt.rounds; ++r) { for (std::size_t t = 0; t < N; ++t) run_slot(r, "
                        "static_cast<int>(t));");
    CHECK(cpp.find("run_slot(opt.rounds, 0);") != std::string::npos);
    CHECK(cpp.find(R"(res.extra["context_switch_bound"] = std::to_string(opt.rounds * N);)") != std::string::npos);
    const auto lean = norm_ws(slurp(repo() / "proofs" / "techniques" / "PrismTechniques" / "LazySeqN.lean"));
    CHECK(lean.find("def rr (N K : Nat) : List Nat := (List.replicate K (List.range N)).flatten") != std::string::npos);
    CHECK(lean.find("def prismSched (N K : Nat) : List Nat := rr N K ++ [0]") != std::string::npos);
    CHECK(lean.find("(prismSched N K).length = K * N + 1") != std::string::npos);
    const auto audit = slurp_lf(repo() / "proofs" / "techniques" / "PrismTechniques" / "Audit.lean");
    for (const char* thm :
         {"slots_of_star", "star_of_slots", "slots_mono", "length_prismSched", "rr_covers_runs", "lazy_sound",
          "lazy_covers_runs", "lazy_covers", "lazy_covers_two", "per_thread_bound_not_enough"}) {
        CHECK(std::regex_search(lean, std::regex(std::string("theorem ") + thm + "\\b")));
        CHECK(audit.find(std::string("#assert_axioms LazySeqN.") + thm + "\n") != std::string::npos);
    }
}

TEST_CASE("repolint: solver sources are listed and certified status is one literal") {
    const auto cmake = slurp(repo() / "CMakeLists.txt");
    const fs::path solver_dir = repo() / "src" / "prism" / "solver";
    for (auto& e : fs::directory_iterator(solver_dir)) {
        if (e.path().extension() != ".cpp") continue;
        CHECK(cmake.find("src/prism/solver/" + e.path().filename().string()) != std::string::npos);
    }
    CHECK_FALSE(std::regex_search(cmake, std::regex("GLOB[^)]*solver")));
    const auto at = cmake.find("src/cuda/probsat.cu");
    CHECK(at != std::string::npos);
    CHECK(cmake.find("if(PRISM_CUDA)") < at);
    CHECK(cmake.find("if(PRISM_LLAMA)") > at);
    const auto hdr = slurp(repo() / "include" / "prism" / "solver.hpp");
    CHECK(hdr.find("kProvedCertified = laws::PROVED_CERTIFIED") != std::string::npos);
    CHECK(hdr.find("\"PROVED-CERTIFIED\"") == std::string::npos);
    for (auto& e : fs::directory_iterator(solver_dir)) {
        if (e.path().extension() != ".cpp") continue;
        CHECK(slurp(e.path()).find("\"PROVED-CERTIFIED\"") == std::string::npos);
    }
    auto portfolio = slurp(solver_dir / "portfolio.cpp");
    std::vector<std::size_t> sets;
    for (std::size_t at = 0; (at = portfolio.find("res.certified = true", at)) != std::string::npos; at += 18)
        sets.push_back(at);
    CHECK(sets.size() == 2);
    for (auto at : sets) {
        auto window = portfolio.substr(at > 400 ? at - 400 : 0, 400);
        CHECK(window.find("ck.certified") != std::string::npos);
    }
    CHECK(slurp(solver_dir / "util.cpp").find(R"(has_line(p.out, "s VERIFIED UNSAT"))") != std::string::npos);
    CHECK(portfolio.find("validate_model(c, formula, msg.model") != std::string::npos);
    CHECK(portfolio.find("validate_model(c, formula, m, &why)") != std::string::npos);
    auto sls = portfolio.substr(portfolio.find("case MemberKind::Sls:"));
    sls = sls.substr(0, sls.find("break;"));
    CHECK(sls.find("Kind::Unsat") == std::string::npos);
}

TEST_CASE("repolint: Lean bit-blaster executables and TRUSTED_BASE promises") {
    const auto portfolio = slurp(repo() / "src" / "prism" / "solver" / "portfolio.cpp");
    auto body = portfolio.substr(portfolio.find("Certify run_checkers("));
    body = body.substr(0, body.find("c.certified = true;"));
    CHECK(body.find("find_tool(\"cake_lpr\"") < body.find("check_lrat_dag(plan.chk"));
    CHECK(body.find("\"not certified: prism-lrat-check not found (NOTRUN)\"") != std::string::npos);
    CHECK(portfolio.find("\"bitblast: lean-proved (toCNF_equisat), prism-bitblast \"") != std::string::npos);
    const auto lake = slurp(repo() / "proofs" / "techniques" / "lakefile.toml");
    for (const char* exe : {"prism-bitblast", "prism-lrat-check"}) {
        CHECK(lake.find(std::string("name = \"") + exe + "\"") != std::string::npos);
        CHECK(lake.substr(lake.find("defaultTargets")).find(std::string("\"") + exe + "\"") != std::string::npos);
    }
    const auto cmake = slurp(repo() / "CMakeLists.txt");
    CHECK(cmake.find("PRISM_LEAN_BIN_DIR") != std::string::npos);
    CHECK(cmake.find("proofs/techniques/.lake/build/bin") != std::string::npos);
    const auto doc = slurp(repo() / "docs" / "TRUSTED_BASE.md");
    for (const char* s :
         {"cake_lpr", "s VERIFIED UNSAT", "bitblast: lean-proved", "checkDag_sound", "tests/cpp/test_leanbb.cpp"})
        CHECK(doc.find(s) != std::string::npos);
}
