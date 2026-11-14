// AI providers (include/prism/ai_providers.hpp): FORGE by default, any other
// AI through PRISM_AI. Request bodies and reply parsing only; no network.
#include "prism/ai_providers.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

namespace {

void set_env(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}

struct EnvGuard {
    ~EnvGuard() {
        for (const char* k : {"PRISM_AI", "PRISM_AI_URL", "PRISM_AI_MODEL", "PRISM_AI_KEY_ENV", "PRISM_AI_CMD"}) set_env(k, nullptr);
    }
};

}  // namespace

using namespace prism::ai;

TEST_CASE("ai providers: FORGE is the default, any other AI by PRISM_AI") {
    EnvGuard g;
    set_env("PRISM_AI", nullptr);
    std::string why;
    auto d = provider_from_env(&why);
    REQUIRE(d.has_value());
    CHECK(d->kind == "forge");
    CHECK(provider_backend(*d, &why) == nullptr);  // the caller uses FORGE

    set_env("PRISM_AI", "ollama");
    auto o = provider_from_env(&why);
    REQUIRE(o.has_value());
    CHECK(o->kind == "openai");
    CHECK(o->url == "http://localhost:11434/v1");

    set_env("PRISM_AI", "claude");
    auto a = provider_from_env(&why);
    REQUIRE(a.has_value());
    CHECK(a->kind == "anthropic");
    CHECK(a->model == "claude-opus-5-5");
    CHECK(a->key_env == "ANTHROPIC_API_KEY");

    set_env("PRISM_AI", "cmd");
    set_env("PRISM_AI_CMD", nullptr);
    auto c = provider_from_env(&why);
    REQUIRE(c.has_value());
    CHECK(provider_backend(*c, &why) == nullptr);
    CHECK(why.find("PRISM_AI_CMD") != std::string::npos);

    set_env("PRISM_AI", "nonsense");
    CHECK_FALSE(provider_from_env(&why).has_value());
}

TEST_CASE("ai providers: request bodies follow each API") {
    ModelRequest req;
    req.system = "sys";
    req.user = "hello";
    req.grammar_text = "root ::= \"x\"";
    ProviderConfig an;
    an.kind = "anthropic";
    an.model = "claude-opus-5-5";
    auto j = nlohmann::json::parse(provider_request_json(an, req));
    CHECK(j["model"] == "claude-opus-5-5");
    CHECK(j["system"] == "sys");
    CHECK(j["messages"][0]["role"] == "user");
    CHECK_FALSE(j.contains("temperature"));  // rejected on anthropic-version 2026-10-21
    CHECK_FALSE(j.contains("grammar"));

    ProviderConfig oa;
    oa.kind = "openai";
    oa.model = "m";
    auto k = nlohmann::json::parse(provider_request_json(oa, req));
    CHECK(k["messages"][0]["role"] == "system");
    CHECK(k["messages"][1]["content"] == "hello");
    CHECK_FALSE(k.contains("grammar"));
    oa.send_grammar = true;
    CHECK(nlohmann::json::parse(provider_request_json(oa, req)).contains("grammar"));
}

TEST_CASE("ai providers: replies, errors and refusals") {
    ProviderConfig an;
    an.kind = "anthropic";
    std::string err;
    CHECK(provider_reply_text(an, R"({"content":[{"type":"thinking","thinking":""},{"type":"text","text":"ok"}],"stop_reason":"end_turn"})", &err) == "ok");
    CHECK(provider_reply_text(an, R"({"content":[],"stop_reason":"refusal"})", &err).empty());
    CHECK(err.find("refusal") != std::string::npos);
    ProviderConfig oa;
    oa.kind = "openai";
    CHECK(provider_reply_text(oa, R"({"choices":[{"message":{"role":"assistant","content":"hi"}}]})", &err) == "hi");
    CHECK(provider_reply_text(oa, R"({"error":{"message":"bad key"}})", &err).empty());
    CHECK(err.find("bad key") != std::string::npos);
    CHECK(provider_reply_text(oa, "not json", &err).empty());
}
