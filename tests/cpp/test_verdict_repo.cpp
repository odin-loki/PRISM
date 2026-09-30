// Repository locks around the verdict lattice: the Lean proofs the verdict
// module is checked against contain no `sorry` / `native_decide`, the Lean
// toolchain is pinned, and the verdict audit runs exactly twice per pipeline
// run (after the stages, and again after the stages that follow unify).
// The lattice itself is compared with the Lean tables in test_main.cpp.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/laws.hpp"
#include "prism/models.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>

namespace {

namespace fs = std::filesystem;

fs::path repo() { return fs::path(__FILE__).parent_path().parent_path().parent_path(); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Lean source without its comments (`-- ...` to end of line, `/- ... -/`
// nested) and string literals, so a word in prose or a message is not code.
std::string lean_code(const std::string& s) {
    std::string out;
    std::size_t i = 0, n = s.size();
    while (i < n) {
        if (s.compare(i, 2, "--") == 0) {
            while (i < n && s[i] != '\n') ++i;
        } else if (s.compare(i, 2, "/-") == 0) {
            int depth = 0;
            do {
                if (s.compare(i, 2, "/-") == 0) {
                    ++depth;
                    i += 2;
                } else if (s.compare(i, 2, "-/") == 0) {
                    --depth;
                    i += 2;
                } else {
                    ++i;
                }
            } while (i < n && depth > 0);
            out += ' ';
        } else if (s[i] == '"') {
            ++i;
            while (i < n && s[i] != '"') i += (s[i] == '\\') ? 2 : 1;
            ++i;
            out += "\"\"";
        } else {
            out += s[i++];
        }
    }
    return out;
}

}  // namespace

TEST_CASE("repo: lean_code strips comments and strings, keeps code") {
    CHECK(lean_code("theorem t : True := sorry -- ok") == "theorem t : True := sorry ");
    CHECK(lean_code("/- sorry /- nested -/ sorry -/ x") == "  x");
    CHECK(lean_code("#eval \"sorry\"") == "#eval \"\"");
}

TEST_CASE("repo: the Lean proofs contain no sorry and no native_decide") {
    auto root = repo() / "proofs";
    REQUIRE(fs::is_directory(root));
    std::regex bad("\\bsorry\\b|native_decide");
    int files = 0;
    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file() || e.path().extension() != ".lean") continue;
        bool in_lake = false;
        for (auto& part : e.path()) in_lake |= part == ".lake";
        if (in_lake) continue;
        ++files;
        INFO(e.path().string());
        CHECK_FALSE(std::regex_search(lean_code(slurp(e.path())), bad));
    }
    CHECK(files > 0);
}

TEST_CASE("repo: the Lean toolchain is pinned to a release") {
    auto tc = slurp(repo() / "proofs" / "lean-toolchain");
    while (!tc.empty() && (tc.back() == '\n' || tc.back() == '\r' || tc.back() == ' ')) tc.pop_back();
    CHECK(std::regex_match(tc, std::regex("leanprover/lean4:v4\\.\\d+\\.\\d+")));
}

TEST_CASE("repo: the pipeline runs the verdict audit exactly twice") {
    auto src = slurp(repo() / "src" / "prism" / "pipeline.cpp");
    int n = 0;
    for (std::size_t at = src.find("laws::audit_report("); at != std::string::npos;
         at = src.find("laws::audit_report(", at + 1))
        ++n;
    CHECK(n == 2);
}

TEST_CASE("verdict audit: the ERROR row is marked as the audit's, the demoted row keeps its original") {
    prism::RunReport rep;
    prism::StageResult fuzz;
    fuzz.name = "fuzz";
    prism::Finding lie{"fuzz", std::string(prism::laws::PROVED), "a.c", std::string("f"), 3, "",
                       "lying fuzzer", std::string(prism::laws::STRENGTH_PROVES)};
    fuzz.findings = {lie};
    rep.stages = {fuzz};
    CHECK(prism::laws::audit_report(rep) == 1);
    auto& fz = rep.stages[0].findings;
    REQUIRE(fz.size() == 2);
    CHECK(fz[0].extra.at("audit_original") == prism::laws::PROVED);
    CHECK(fz[1].status == prism::laws::ERROR);
    CHECK(fz[1].extra.at("audit") == "verdict");
    CHECK(fz[1].message == "verdict audit: fuzz may not emit PROVED");
}
