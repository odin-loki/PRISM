#pragma once
// FORGE-based test doubles for PRISM's AI paths (replace the deleted llama-server /
// Ollama HTTP fakes). Two levels:
//  - ScriptedChat: in-process chat seam (stages_detail::set_chat_backend_for_testing)
//    that answers by system prompt and records every call; deterministic replies.
//  - tiny_fpk(): a real FORGE .fpk built with FORGE's own tests/tiny_model.hpp
//    (random tiny Llama, byte tokenizer) so the forge_llm engine path runs end to end.

#include "../../src/prism/stages/llm.hpp"

#include <tiny_model.hpp>  // FORGE tests/ (on the prism_tests include path)

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace prism_test {

class ScriptedChat {
public:
    using Reply = std::function<std::string(const std::string& system)>;
    explicit ScriptedChat(Reply r) : st_(std::make_shared<State>()) {
        st_->reply = std::move(r);
        auto st = st_;
        prism::stages_detail::set_chat_backend_for_testing(
            [st](const std::vector<std::pair<std::string, std::string>>& msgs, double) {
                std::string sys;
                for (auto& [role, content] : msgs)
                    if (role == "system") sys = content;
                std::lock_guard<std::mutex> g(st->mu);
                st->systems.push_back(sys);
                if (st->fail) return prism::stages_detail::ChatResult{"", "test-double", st->fail_error};
                return prism::stages_detail::ChatResult{st->reply(sys), "test-double", ""};
            });
    }
    ~ScriptedChat() { prism::stages_detail::set_chat_backend_for_testing(nullptr); }
    ScriptedChat(const ScriptedChat&) = delete;
    ScriptedChat& operator=(const ScriptedChat&) = delete;

    // Every later call fails with `error` (a lost model / connection).
    void fail_with(std::string error) {
        std::lock_guard<std::mutex> g(st_->mu);
        st_->fail = true;
        st_->fail_error = std::move(error);
    }
    int chats() const {
        std::lock_guard<std::mutex> g(st_->mu);
        return int(st_->systems.size());
    }
    // Calls whose system prompt starts with `prefix`.
    int count(const std::string& prefix) const {
        std::lock_guard<std::mutex> g(st_->mu);
        int n = 0;
        for (auto& s : st_->systems) n += s.rfind(prefix, 0) == 0;
        return n;
    }

private:
    struct State {
        mutable std::mutex mu;
        Reply reply;
        std::vector<std::string> systems;
        bool fail = false;
        std::string fail_error;
    };
    std::shared_ptr<State> st_;
};

// A tiny real FORGE model (built once per process) for engine-level tests.
inline std::filesystem::path tiny_fpk() {
    static const std::filesystem::path p = [] {
        auto path = std::filesystem::temp_directory_path() / "prism_forge_tiny.fpk";
        forge_test::make_tiny_model(path.string());
        return path;
    }();
    return p;
}

}  // namespace prism_test
