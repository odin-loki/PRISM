// Static parity between src/prism/pir/*.cpp and proofs/refinement (was
// tests/test_pir_refinement.py). Fixture runs live in tests/cpp/test_qa.cpp.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <set>
#include <string>

namespace {

namespace fs = std::filesystem;

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string binop_body(const std::string& cpp) {
    auto start = cpp.find("void binop(");
    REQUIRE(start != std::string::npos);
    auto end = cpp.find("void inst(", start);
    REQUIRE(end != std::string::npos);
    return cpp.substr(start, end - start);
}

}  // namespace

TEST_CASE("pir refinement: every Lean check property appears in translate.cpp") {
    const auto lean = slurp(repo() / "proofs" / "refinement" / "PrismRefine" / "Translate.lean");
    const auto cpp = slurp(repo() / "src" / "prism" / "pir" / "translate.cpp");
    std::regex pair_re(R"("([a-z0-9+*\-]+)" "([A-Z][A-Z\-]+)")");
    std::set<std::pair<std::string, std::string>> pairs;
    for (std::sregex_iterator it(lean.begin(), lean.end(), pair_re), end; it != end; ++it)
        pairs.insert({(*it)[1].str(), (*it)[2].str()});
    CHECK(pairs.size() >= 15u);
    for (auto& [prop, cls] : pairs) {
        CHECK(cpp.find('"' + prop + '"') != std::string::npos);
        CHECK(cpp.find('"' + cls + '"') != std::string::npos);
    }
}

TEST_CASE("pir refinement: memory checks in XTranslate.lean appear in translate_mem.cpp") {
    const auto xlean = slurp(repo() / "proofs" / "refinement" / "PrismRefine" / "XTranslate.lean");
    const auto mem = slurp(repo() / "src" / "prism" / "pir" / "translate_mem.cpp") +
                     slurp(repo() / "src" / "prism" / "pir" / "translate.cpp");
    std::regex pair_re(R"("([a-z0-9+*\-]+)" "([A-Z][A-Z\-]+)")");
    std::regex tuple_re(R"(\("([a-z0-9\-]+)", "([A-Z][A-Z\-]+)"\))");
    std::set<std::pair<std::string, std::string>> pairs;
    for (std::sregex_iterator it(xlean.begin(), xlean.end(), pair_re), end; it != end; ++it)
        pairs.insert({(*it)[1].str(), (*it)[2].str()});
    for (std::sregex_iterator it(xlean.begin(), xlean.end(), tuple_re), end; it != end; ++it)
        pairs.insert({(*it)[1].str(), (*it)[2].str()});
    CHECK(pairs.size() >= 8u);
    for (auto& [prop, cls] : pairs) {
        CHECK(mem.find('"' + prop + '"') != std::string::npos);
        CHECK(mem.find('"' + cls + '"') != std::string::npos);
    }
}

TEST_CASE("pir refinement: translate.cpp binop properties are modelled in Translate.lean") {
    const auto lean = slurp(repo() / "proofs" / "refinement" / "PrismRefine" / "Translate.lean");
    const auto body = binop_body(slurp(repo() / "src" / "prism" / "pir" / "translate.cpp"));
    std::regex prop_re(R"("([a-z0-9+*\-]+)",\s*"(?:INT|UB)-[A-Z\-]+")");
    std::set<std::string> props;
    for (std::sregex_iterator it(body.begin(), body.end(), prop_re), end; it != end; ++it)
        props.insert((*it)[1].str());
    props.insert("div0");
    props.insert("mod0");
    CHECK(props.size() >= 15u);
    for (auto& prop : props) CHECK(lean.find('"' + prop + '"') != std::string::npos);
}

TEST_CASE("pir refinement: binop flags are modelled or explicitly refused") {
    const auto cpp = slurp(repo() / "src" / "prism" / "pir" / "translate.cpp");
    const auto export_cpp = slurp(repo() / "src" / "prism" / "pir" / "export_lean.cpp");
    const auto xlean = slurp(repo() / "proofs" / "refinement" / "PrismRefine" / "XTranslate.lean");
    std::regex flag_re(R"has_flag\(in, "([a-z]+)"\)");
    std::set<std::string> flags;
    for (std::sregex_iterator it(cpp.begin(), cpp.end(), flag_re), end; it != end; ++it)
        flags.insert((*it)[1].str());
    for (auto& fl : flags) {
        if (fl == "inbounds") {
            CHECK(export_cpp.find(R"(if (fl != "inbounds") throw Unsupported{"getelementptr " + fl};)") !=
                  std::string::npos);
            CHECK(xlean.find("if inb then") != std::string::npos);
            continue;
        }
        if (fl == "samesign") {
            CHECK(cpp.find(R"(check(cur, p2(cur, Op::Ne, na, nb), "samesign", "UB-POISON")") !=
                  std::string::npos);
            continue;
        }
        CHECK((fl == "nsw" || fl == "nuw" || fl == "exact" || fl == "disjoint" || fl == "nneg"));
    }
    std::regex allow_re(R"(fl != "([a-z]+)")");
    std::set<std::string> allow;
    for (std::sregex_iterator it(export_cpp.begin(), export_cpp.end(), allow_re), end; it != end; ++it)
        allow.insert((*it)[1].str());
    allow.erase("inbounds");
    CHECK(allow == std::set<std::string>{"nsw", "nuw", "exact", "disjoint", "nneg"});
    CHECK(export_cpp.find(R"(fl != "samesign")") == std::string::npos);
}

TEST_CASE("pir refinement: Op names in translate.cpp match Check.lean") {
    const auto pir = slurp(repo() / "src" / "prism" / "pir" / "pir.cpp");
    const auto check = slurp(repo() / "proofs" / "refinement" / "PrismRefine" / "Check.lean");
    const auto body = binop_body(slurp(repo() / "src" / "prism" / "pir" / "translate.cpp"));
    std::regex name_re(R"(case Op::(\w+): return "([^"]+)";)");
    std::map<std::string, std::string> names;
    for (std::sregex_iterator it(pir.begin(), pir.end(), name_re), end; it != end; ++it)
        names[(*it)[1].str()] = (*it)[2].str();
    std::regex op_re(R"(Op::(\w+))");
    std::set<std::string> used;
    for (std::sregex_iterator it(body.begin(), body.end(), op_re), end; it != end; ++it)
        used.insert((*it)[1].str());
    for (auto* op : {"Eq", "Ne", "Ult", "Ule", "Ugt", "Uge", "Slt", "Sle", "Sgt", "Sge", "Select", "ZExt",
                     "SExt", "Trunc"})
        used.insert(op);
    for (auto& op : used) {
        REQUIRE(names.count(op));
        CHECK(check.find('"' + names[op] + '"') != std::string::npos);
    }
}

TEST_CASE("pir refinement: globals and pointer harness share definitions") {
    const auto mem = slurp(repo() / "src" / "prism" / "pir" / "translate_mem.cpp");
    const auto export_cpp = slurp(repo() / "src" / "prism" / "pir" / "export_lean.cpp");
    const auto tr = slurp(repo() / "src" / "prism" / "pir" / "translate.cpp");
    CHECK(mem.find("entry_globals(t_.module(), lay_, f, t_.options().globals_initial)") != std::string::npos);
    CHECK(export_cpp.find("pirmem::entry_globals(m, lay, f, opt.globals_initial)") != std::string::npos);
    CHECK(tr.find("mt.preassign_globals(f)") != std::string::npos);
    CHECK(tr.find("mt.emit_entry_globals()") != std::string::npos);
    CHECK(mem.find("uint64_t contract_elem_bytes(") != std::string::npos);
    CHECK(tr.find("pirmem::contract_elem_bytes(tr.mt.layout(), f, p, c)") != std::string::npos);
    CHECK(export_cpp.find("pirmem::contract_elem_bytes(lay, f, p, *c)") != std::string::npos);
    CHECK(mem.find("bool reaches_alloc_failed(") != std::string::npos);
    CHECK(tr.find("pirmem::reaches_alloc_failed(m, f)") != std::string::npos);
    CHECK(export_cpp.find("pirmem::reaches_alloc_failed(m, f)") != std::string::npos);
}
