// A lint hit is a defect. prism::lint_add (src/prism/checkers_dispatch.cpp)
// records laws::FAILED with strength FINDS. laws::is_proof is false for that
// status. A checker that cannot run is laws::NOTRUN (the Clang-AST layer when
// clang is absent). That row is a gap. It is not a pass and it is not "no defect".

#include <doctest/doctest.h>

#ifdef ERROR
#  undef ERROR
#endif

#include "prism/astlint.hpp"
#include "prism/checkers.hpp"
#include "prism/config.hpp"
#include "prism/laws.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

struct Snippet {
    fs::path dir;
    fs::path file;
    explicit Snippet(const std::string& text) {
        dir = fs::temp_directory_path() / "prism-lint-status";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        file = dir / "tiny.c";
        std::ofstream(file, std::ios::binary) << text;
    }
    ~Snippet() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

void check_hit(const prism::Finding& f) {
    CHECK(f.status == prism::laws::FAILED);
    CHECK_FALSE(prism::laws::is_proof(f.status));
    CHECK(f.status != prism::laws::PROVED);
    CHECK(f.status != prism::laws::PROVED_UNBOUNDED);
    CHECK(f.status != prism::laws::PROVED_CERTIFIED);
    CHECK(f.status != prism::laws::PROVED_ASSUMING);
    CHECK(f.status != prism::laws::BOUNDED);
    CHECK(f.status != prism::laws::CLEAN);
    // NOTRUN means the checker did not run. A hit must not wear that status.
    CHECK(f.status != prism::laws::NOTRUN);
}

}  // namespace

TEST_CASE("lint_add records a hit as FAILED and not a proof") {
    std::vector<prism::Finding> out;
    const std::vector<std::string> lines = {"    gets(b);"};
    prism::lint_add(out, "tiny.c", std::string("f"), 1, "API-GETS",
                    "gets() is unbounded (removed from C11)", lines);
    REQUIRE(out.size() == 1);
    check_hit(out[0]);
    CHECK(out[0].stage == "lints");
    CHECK(out[0].cls == "API-GETS");
    CHECK(out[0].strength == prism::laws::STRENGTH_FINDS);
    CHECK(out[0].strength != prism::laws::STRENGTH_PROVES);
}

TEST_CASE("run_lints on a gets call is FAILED and not a proof") {
    const Snippet s("void f(void) { char b[8]; gets(b); }\n");
    const auto out = prism::run_lints({s.file}, s.dir, 1);
    const prism::Finding* hit = nullptr;
    for (const auto& f : out)
        if (f.cls == "API-GETS") hit = &f;
    REQUIRE(hit != nullptr);
    check_hit(*hit);
    CHECK(hit->stage == "lints");
    CHECK(hit->strength == prism::laws::STRENGTH_FINDS);
}

TEST_CASE("a lint layer that cannot run is NOTRUN and the regex hit stays FAILED") {
    const Snippet s("void f(void) { char b[8]; gets(b); }\n");
    prism::Config cfg;
    cfg.jobs = 1;
    cfg.root = s.dir;
    // Backend::Process with no clang binary: the AST layer does not run.
    const auto out = prism::astlint::run_lints_ast({s.file}, s.dir, cfg, std::nullopt,
                                                   prism::astlint::Backend::Process);
    const prism::Finding* layer = nullptr;
    const prism::Finding* hit = nullptr;
    for (const auto& f : out) {
        if (prism::astlint::is_layer_row(f)) layer = &f;
        if (f.cls == "API-GETS") hit = &f;
    }
    REQUIRE(layer != nullptr);
    CHECK(layer->status == prism::laws::NOTRUN);
    CHECK_FALSE(prism::laws::is_proof(layer->status));
    CHECK(layer->status != prism::laws::CLEAN);
    CHECK(layer->status != prism::laws::PROVED);
    CHECK(layer->status != prism::laws::PROVED_UNBOUNDED);
    CHECK(layer->status != prism::laws::PROVED_CERTIFIED);
    CHECK(layer->status != prism::laws::PROVED_ASSUMING);
    CHECK(layer->status != prism::laws::BOUNDED);
    // The missing layer is not a pass that the snippet has no defect.
    REQUIRE(hit != nullptr);
    check_hit(*hit);
}
