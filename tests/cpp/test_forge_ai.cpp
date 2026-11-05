#include <cmath>
// FORGE inference engine end to end on a tiny real .fpk (FORGE tests/tiny_model.hpp):
// no test double, the same forge_llm path PRISM uses for Goedel-Prover-V2-32B.
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

TEST_CASE("forge ai: bounded repetition is lowered to the GBNF FORGE parses") {
    using prism::forge_llm::lower_grammar;
    CHECK(lower_grammar("ws ::= [ ]{0,2}\n") == "ws ::= (([ ] ([ ])?)?)\n");
    CHECK(lower_grammar("n ::= [0-9] [0-9]{2}\n") == "n ::= [0-9] ([0-9] [0-9])\n");
    CHECK(lower_grammar("s ::= \"\\\"\" c{1,400} \"\\\"\"\n") == "s ::= \"\\\"\" (c c*) \"\\\"\"\n");
    CHECK(lower_grammar("r ::= x ( \",\" x ){0,1}\n") == "r ::= x ((( \",\" x ))?)\n");
    CHECK(lower_grammar("r ::= a{3,}\n") == "r ::= (a a a a*)\n");
    // braces inside literals, classes and comments are text, not repetition
    CHECK(lower_grammar("# a{1,2}\nr ::= \"{1,2}\" [{]\n") == "# a{1,2}\nr ::= \"{1,2}\" [{]\n");
}

TEST_CASE("forge ai: every PRISM grammar compiles in FORGE and constrains decoding") {
    // A grammar FORGE cannot parse fails every grammar-constrained AI feature with a
    // real model (measured: "grammar line 8: expected rule name" before lowering).
    for (const char* name : {"invariants", "harness", "contract", "explain", "lean_proof", "assumption_audit", "ask", "draft"}) {
        CAPTURE(name);
        prism::forge_llm::Request req;
        req.messages = {{"user", "go"}};
        req.grammar = prism::ai::grammar_text(name);
        REQUIRE_FALSE(req.grammar.empty());
        req.max_new = 4;
        req.temperature = 0.f;
        auto r = prism::forge_llm::complete(prism_test::tiny_fpk(), req);
        CHECK(r.error.find("grammar") == std::string::npos);
        CHECK(r.error.find("FORGE") == std::string::npos);
    }
}

TEST_CASE("forge ai: the chat opener closes an empty think block unless thinking is asked for") {
    prism::forge_llm::Messages m{{"system", "s"}, {"user", "u"}};
    CHECK(prism::forge_llm::render_chat(m) ==
          "<|im_start|>system\ns<|im_end|>\n<|im_start|>user\nu<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n");
    CHECK(prism::forge_llm::render_chat(m, true).ends_with("<|im_start|>assistant\n<think>\n"));
}

// The real model (Goedel-Prover-V2-32B, ~20 GB): skipped by default. Run it with
//   PRISM_FPK=~/.prism/models/goedel-prover-v2-32b.fpk PRISM_DEVICE=cuda \
//   prism_tests --no-skip -tc='forge ai: the real model*'
// (default_config() resolves PRISM_FPK, else the default install path).
TEST_CASE("forge ai: the real model answers plainly, under a grammar, and thinks on request" * doctest::skip()) {
    const auto fpk = prism::default_config().fpk;
    REQUIRE_MESSAGE(prism::forge_llm::is_fpk(fpk), "no .fpk at " << fpk.string());
    prism::forge_llm::Request req;
    req.messages = {{"system", "Answer in one short sentence."}, {"user", "What does `int x = INT_MAX; x++;` do in C?"}};
    req.max_new = 64;
    req.temperature = 0.f;
    auto r = prism::forge_llm::complete(fpk, req);
    MESSAGE("plain: load " << r.load_s << " s, prefill " << r.prompt_tokens << " tok / " << r.prefill_s << " s, "
                           << r.new_tokens << " tok / " << r.decode_s << " s: " << r.text);
    CHECK(r.error.empty());
    CHECK(r.new_tokens > 0);
    CHECK(r.thinking.empty());

    req.messages = {{"user", "Is signed integer overflow undefined behaviour in C? Answer yes or no."}};
    req.grammar = "root ::= \"yes\" | \"no\"\n";
    auto g = prism::forge_llm::complete(fpk, req);
    MESSAGE("grammar: " << g.text);
    CHECK(g.error.empty());
    CHECK(g.text == "yes");

    req.think = true;
    req.think_max = 256;
    auto t = prism::forge_llm::complete(fpk, req);
    MESSAGE("think: " << t.thinking_tokens << " thinking tok (capped " << t.thinking_capped << "), answer " << t.text);
    CHECK(t.error.empty());
    CHECK(t.thinking_tokens > 0);
    CHECK((t.text == "yes" || t.text == "no"));
}
