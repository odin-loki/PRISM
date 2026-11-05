#include <cmath>
// FORGE inference engine end to end on a tiny real .fpk (FORGE tests/tiny_model.hpp):
// no test double, the same forge_llm path PRISM uses for Qwen 3.5.
#include <doctest/doctest.h>

#include "forge_test_model.hpp"
#include "prism/forge_llm.hpp"

namespace sd = prism::stages_detail;

TEST_CASE("forge ai: the tiny .fpk opens and the engine binds to FORGE") {
    const auto fpk = prism_test::tiny_fpk();
    CHECK(prism::forge_llm::is_fpk(fpk));
    prism::Config cfg;
    cfg.fpk = fpk;
    cfg.out = std::filesystem::temp_directory_path() / "prism_forge_ai";
    sd::LlmEngine e(cfg);
    CHECK(e.backend == "forge");
    CHECK(e.available());
    CHECK_FALSE(prism::forge_llm::model_digest(fpk).empty());
}

TEST_CASE("forge ai: a completion on the tiny model returns tokens, no error") {
    prism::forge_llm::Request req;
    req.messages = {{"system", "be brief"}, {"user", "hi"}};
    req.max_new = 4;
    req.temperature = 0.f;
    auto r = prism::forge_llm::complete(prism_test::tiny_fpk(), req);
    CHECK(r.error.empty());
    CHECK(r.prompt_tokens > 0);
    CHECK(r.new_tokens > 0);
    CHECK(r.new_tokens <= 4);
}

TEST_CASE("forge ai: a grammar-constrained completion only emits the grammar's language") {
    prism::forge_llm::Request req;
    req.messages = {{"user", "yes or no?"}};
    req.grammar = "root ::= \"yes\" | \"no\"\n";
    req.max_new = 8;
    req.temperature = 0.f;
    auto r = prism::forge_llm::complete(prism_test::tiny_fpk(), req);
    CHECK(r.error.empty());
    CHECK((r.text == "yes" || r.text == "no"));
}

TEST_CASE("forge ai: embedding with a generative .fpk is a vector or a clear error, never a crash") {
    // FORGE main pools a generative model's hidden states; older FORGE refused.
    std::string err;
    auto v = prism::forge_llm::embed(prism_test::tiny_fpk(), "int x;", &err);
    if (v) {
        CHECK_FALSE(v->empty());
        for (double x : *v) CHECK(std::isfinite(x));
    } else {
        CHECK_FALSE(err.empty());
    }
}

TEST_CASE("forge ai: a missing .fpk is not a model") {
    CHECK_FALSE(prism::forge_llm::is_fpk("/nonexistent/prism.fpk"));
    auto r = prism::forge_llm::complete("/nonexistent/prism.fpk", {});
    CHECK_FALSE(r.error.empty());
}
