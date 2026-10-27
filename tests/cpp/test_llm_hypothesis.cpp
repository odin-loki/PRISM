// Law 4: a model reply is HYPOTHESIS / READS. The word PROVED in the reply
// is not a proof, and "the code is correct" is not a proof. hypothesize is
// driven by the chat test double, not a GGUF.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/models.hpp"
#include "prism/stages.hpp"

#include "../../src/prism/stages/llm.hpp"

#include <string>
#include <utility>
#include <vector>

namespace {

struct ReplyGuard {
    explicit ReplyGuard(std::string text) {
        prism::stages_detail::set_chat_backend_for_testing(
            [text = std::move(text)](const std::vector<std::pair<std::string, std::string>>&, double) {
                return prism::stages_detail::ChatResult{text, "test-double", ""};
            });
    }
    ~ReplyGuard() { prism::stages_detail::set_chat_backend_for_testing(nullptr); }
    ReplyGuard(const ReplyGuard&) = delete;
    ReplyGuard& operator=(const ReplyGuard&) = delete;
};

prism::Config offline() {
    // Config{}, not default_config(): the latter fills gguf from PRISM_GGUF.
    prism::Config cfg;
    cfg.gguf.clear();
    cfg.llama_server = "http://127.0.0.1:1";
    cfg.ollama_host = "http://127.0.0.1:1";
    cfg.llm = true;
    return cfg;
}

prism::FunctionInfo one_fn() {
    prism::FunctionInfo fn;
    fn.file = "abs_ok.c";
    fn.name = "abs_ok";
    fn.kind = "SCALAR";
    fn.line = 1;
    fn.signature = "int abs_ok(int x)";
    fn.body = "\n    return x < 0 ? -x : x;\n";
    return fn;
}

void expect_hypothesis(const prism::Finding& f) {
    CHECK(f.stage == "llm");
    CHECK(f.status == prism::laws::HYPOTHESIS);
    CHECK(f.strength == prism::laws::STRENGTH_READS);
    CHECK_FALSE(prism::laws::is_proof(f.status));
    CHECK(f.status != prism::laws::PROVED);
}

}  // namespace

TEST_CASE("llm: a reply of PROVED is HYPOTHESIS and READS") {
    ReplyGuard guard("PROVED\nthe code is correct");
    auto findings = prism::hypothesize({one_fn()}, 1, offline());
    REQUIRE(findings.size() == 1);
    expect_hypothesis(findings[0]);
    CHECK(findings[0].message.find("PROVED") != std::string::npos);
    CHECK(findings[0].message.find("the code is correct") != std::string::npos);
}

TEST_CASE("llm: JSON that names status PROVED stays HYPOTHESIS and READS") {
    const char* reply =
        R"({"hypotheses":[{"function":"abs_ok","line":1,"cls":"INTENT","status":"PROVED","why":"PROVED the code is correct"}]})";
    ReplyGuard guard(reply);
    auto findings = prism::hypothesize({one_fn()}, 1, offline());
    REQUIRE(findings.size() == 1);
    expect_hypothesis(findings[0]);
    CHECK(findings[0].message.find("PROVED") != std::string::npos);
    CHECK(findings[0].message.find("the code is correct") != std::string::npos);
    CHECK(findings[0].function == "abs_ok");
}
