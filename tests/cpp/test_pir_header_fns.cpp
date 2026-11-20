// --pir-header-fns (docs/PIR.md "Header functions"): a stub unit that includes a header gets the header functions
// PRISM_PIR_ONLY names checked in it; other header code stays skipped. Folded UB in a header function keeps it from a
// proof, and the unit's own folded-UB diagnostics are never placed in a header function that has an instruction on the
// same line number. A big-endian target is not checked.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/cli.hpp"
#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#ifndef _WIN32

namespace {

namespace fs = std::filesystem;
namespace pp = prism::pir;
namespace L = prism::laws;

bool skip_without_frontend() {
    auto cfg = prism::default_config();
    std::string why;
    if (!pp::frontend_usable(cfg, &why)) {
        MESSAGE(why);
        return true;
    }
    if (!pp::z3_available()) {
        MESSAGE("NOTRUN: pir needs Z3 (rebuild with -DPRISM_Z3=ON)");
        return true;
    }
    return false;
}

const fs::path kDir = fs::temp_directory_path() / "prism_pir_header_fns_test";

void write(const std::string& name, const std::string& text) {
    fs::create_directories(kDir);
    std::ofstream(kDir / name, std::ios::binary) << text;
}

// hf.h line numbers matter: hf_same_line is on line 4, like unit_fold's folded UB in the stub
const char* kHeader =
    "static inline unsigned hf_ok(unsigned a) { return a + 1u; }\n"
    "static inline int hf_add(int a, int b) { return a + b; }\n"
    "static inline int hf_fold(void) { return 1 << 31; }\n"
    "static inline int hf_same_line(int x) { return x & 1; }\n";
const char* kStub =
    "#include \"hf.h\"\n"
    "int unit_fn(int x) { return x; }\n"
    "int unit_other(void) { return 0; }\n"
    "int unit_fold(void) { return 1 << 31; }\n";

struct Got {
    std::map<std::string, prism::Finding> fn;
    std::vector<prism::Finding> unit;
};

Got run(const std::string& file, bool header_fns, const std::string& only, prism::Config cfg = prism::default_config()) {
    cfg.root = kDir;
    cfg.jobs = 1;
    cfg.pir_header_fns = header_fns;
    if (only.empty()) ::unsetenv("PRISM_PIR_ONLY");
    else ::setenv("PRISM_PIR_ONLY", only.c_str(), 1);
    Got g;
    for (auto& f : pp::run_pir({kDir / file}, cfg)) {
        if (f.function) g.fn[*f.function] = f;
        else g.unit.push_back(f);
    }
    ::unsetenv("PRISM_PIR_ONLY");
    return g;
}

std::string extra(const prism::Finding& f, const char* k) {
    auto it = f.extra.find(k);
    return it == f.extra.end() ? std::string() : it->second;
}

std::string only(std::initializer_list<const char*> fns) {
    std::string s;
    for (auto* f : fns) s += std::string("hf_stub.c\t") + f + "\n";
    return s;
}

}  // namespace

TEST_CASE("pir header fns: without the option a header function has no record, even when named") {
    if (skip_without_frontend()) return;
    write("hf.h", kHeader);
    write("hf_stub.c", kStub);
    auto g = run("hf_stub.c", false, only({"hf_ok", "hf_add", "unit_fn"}));
    CHECK(g.fn.contains("unit_fn"));
    CHECK_FALSE(g.fn.contains("hf_ok"));
    CHECK_FALSE(g.fn.contains("hf_add"));
}

TEST_CASE("pir header fns: the named header functions are checked in the stub, the others are not") {
    if (skip_without_frontend()) return;
    write("hf.h", kHeader);
    write("hf_stub.c", kStub);
    auto g = run("hf_stub.c", true, only({"hf_ok", "hf_add"}));
    REQUIRE(g.fn.contains("hf_ok"));
    REQUIRE(g.fn.contains("hf_add"));
    CHECK_MESSAGE(g.fn["hf_ok"].status == L::PROVED, (g.fn["hf_ok"].status + ": " + g.fn["hf_ok"].message));
    CHECK_MESSAGE(g.fn["hf_add"].status == L::FAILED, (g.fn["hf_add"].status + ": " + g.fn["hf_add"].message));
    CHECK(extra(g.fn["hf_ok"], "header_file").ends_with("hf.h"));
    CHECK(extra(g.fn["hf_ok"], "header_context").find("--pir-header-fns") != std::string::npos);
    CHECK(g.fn["hf_ok"].line == 1);  // the header's line
    CHECK_FALSE(g.fn.contains("hf_fold"));       // not named
    CHECK_FALSE(g.fn.contains("hf_same_line"));  // not named
    CHECK_FALSE(g.fn.contains("unit_fn"));       // PRISM_PIR_ONLY names only the two
    // without PRISM_PIR_ONLY no header function is checked: every includer would check its headers again
    auto all = run("hf_stub.c", true, "");
    CHECK(all.fn.contains("unit_fn"));
    CHECK_FALSE(all.fn.contains("hf_ok"));
}

TEST_CASE("pir header fns: folded UB in a header function is never a proof; the unit's folded UB stays in the unit") {
    if (skip_without_frontend()) return;
    write("hf.h", kHeader);
    write("hf_stub.c", kStub);
    auto g = run("hf_stub.c", true, only({"hf_fold", "hf_same_line", "unit_fold"}));
    REQUIRE(g.fn.contains("hf_fold"));
    CHECK_MESSAGE(g.fn["hf_fold"].status != L::PROVED, (g.fn["hf_fold"].status + ": " + g.fn["hf_fold"].message));
    CHECK_MESSAGE(g.fn["hf_fold"].status != L::PROVED_ASSUMING, g.fn["hf_fold"].message);
    CHECK(g.fn["hf_fold"].message.find("clang-folded UB in") != std::string::npos);
    // unit_fold's diagnostic is on line 4 of the stub; hf_same_line is on line 4 of hf.h: the marker is the unit's
    REQUIRE(g.fn.contains("unit_fold"));
    REQUIRE(g.fn.contains("hf_same_line"));
    CHECK_MESSAGE(g.fn["hf_same_line"].status == L::PROVED,
                  (g.fn["hf_same_line"].status + ": " + g.fn["hf_same_line"].message));
    CHECK_MESSAGE((g.fn["unit_fold"].status == L::FAILED || g.fn["unit_fold"].status == L::NEEDS_HARNESS),
                  (g.fn["unit_fold"].status + ": " + g.fn["unit_fold"].message));
    CHECK(g.fn["unit_fold"].status != L::PROVED);
    // the same without the option (header functions not checked, but still emitted and on the same line numbers): the
    // unit's folded UB is still the unit's
    ::setenv("PRISM_PIR_EMIT_ALL_DECLS", "1", 1);  // the header functions get IR without being checked
    auto n = run("hf_stub.c", false, only({"unit_fold", "unit_fn"}));
    ::unsetenv("PRISM_PIR_EMIT_ALL_DECLS");
    REQUIRE(n.fn.contains("unit_fold"));
    CHECK_MESSAGE((n.fn["unit_fold"].status == L::FAILED || n.fn["unit_fold"].status == L::NEEDS_HARNESS),
                  (n.fn["unit_fold"].status + ": " + n.fn["unit_fold"].message));
}

TEST_CASE("pir: a big-endian target is not checked") {
    if (skip_without_frontend()) return;
    write("be.c", "int be_fn(int x) { return x; }\n");
    write("be_cc.json", "[{\"directory\": \"" + kDir.generic_string() + "\", \"file\": \"" + (kDir / "be.c").generic_string() +
                            "\", \"arguments\": [\"clang\", \"--target=s390x-linux-gnu\", \"-c\", \"" +
                            (kDir / "be.c").generic_string() + "\"]}]");
    auto cfg = prism::default_config();
    cfg.compile_commands = kDir / "be_cc.json";
    auto g = run("be.c", false, "", cfg);
    bool notrun = false;
    std::string msgs;
    for (auto& f : g.unit) {
        msgs += f.status + ": " + f.message + "\n";
        if (f.status == L::NOTRUN && f.message.find("big-endian") != std::string::npos) notrun = true;
    }
    if (!notrun && msgs.find("big-endian") == std::string::npos && g.fn.empty() && !g.unit.empty() &&
        g.unit.front().message.find("front end") != std::string::npos) {
        MESSAGE(("clang has no s390x target here: " + msgs));
        return;
    }
    CHECK_MESSAGE(notrun, msgs);
    CHECK_FALSE(g.fn.contains("be_fn"));
    fs::remove_all(kDir);
}

#endif
