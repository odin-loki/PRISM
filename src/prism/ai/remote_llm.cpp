// Remote model backend: see include/prism/remote_llm.hpp.
#include "prism/remote_llm.hpp"

#include <forge/inference/remote.hpp>

#include <atomic>
#include <cstdlib>
#include <string>

namespace prism::remote_llm {
namespace rm = forge::inference::remote;

namespace {

class Backend final : public ai::ModelBackend {
public:
    explicit Backend(rm::config c) : cfg_(std::move(c)) {}
    std::string name() const override {
        return "remote:" + cfg_.name + ":" + (cfg_.model.empty() ? std::string("default") : cfg_.model);
    }
    std::string model_sha256() const override { return "unknown"; }  // a served model has no digest we can see
    ai::ModelReply complete(const ai::ModelRequest& req) override {
        rm::request r;
        if (!req.system.empty()) r.messages.push_back({"system", req.system});
        r.messages.push_back({"user", req.user});
        r.max_tokens = 1024;
        if (const char* e = std::getenv("PRISM_MAX_NEW"); e && *e) r.max_tokens = std::max(16, std::atoi(e));
        // Every PRISM feature asks for one JSON value (its grammar's shape) and
        // validates it; the GBNF goes to servers that accept it (send_grammar).
        r.grammar = req.grammar_text;
        r.json = !req.grammar_text.empty() && cfg_.kind == rm::provider::openai;
        if (cfg_.kind != rm::provider::anthropic) r.temperature = 0.6;
        auto rep = rm::complete(cfg_, r);
        ++calls_;
        if (!rep.ok()) return {"", "remote model (" + name() + "): " + rep.error};
        return {rep.text, ""};
    }

private:
    rm::config cfg_;
    std::atomic<unsigned> calls_{0};
};

}  // namespace

std::shared_ptr<ai::ModelBackend> from_env(std::string* why) {
    std::string w;
    auto c = rm::config_from_env(&w);
    if (why) *why = w;
    if (!c) return nullptr;
    return std::make_shared<Backend>(std::move(*c));
}

}  // namespace prism::remote_llm
