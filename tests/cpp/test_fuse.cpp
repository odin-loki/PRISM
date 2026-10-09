// Doctests for the fuse stage: FuSeBMC branch goals (GOAL_n numbering),
// Fuzz4All prompt scoring and autoprompt, and the honesty of the loop: a
// fuzzer CLEAN is not a proof (Law 3), a missing LLM / AFL++ / libFuzzer is
// NOTRUN (Law 1) and never takes the greybox CLEAN away, and a distilled
// prompt is a HYPOTHESIS (Law 4). The model is a fake llama-server on
// 127.0.0.1 that answers each system prompt with a fixed reply.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/sandbox.hpp"
#include "prism/stages.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <stdlib.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

fs::path td() { return fs::path(PRISM_SOURCE_DIR) / "testdata"; }

prism::FunctionInfo fn_named(const char* file, const char* name) {
    for (auto& f : prism::extract_functions(td() / file, file))
        if (f.name == name) return f;
    FAIL("missing function " << name);
    return {};
}

bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

std::vector<std::string> json_list(const prism::Finding& f, const char* key) {
    auto it = f.extra.find(key);
    if (it == f.extra.end() || it->second.empty()) return {};
    auto j = nlohmann::json::parse(it->second);
    return j.get<std::vector<std::string>>();
}

std::string extra_or(const prism::Finding& f, const char* key) {
    auto it = f.extra.find(key);
    return it == f.extra.end() ? std::string() : it->second;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<prism::Finding> with_status(const std::vector<prism::Finding>& recs, std::string_view st) {
    std::vector<prism::Finding> out;
    for (auto& r : recs)
        if (r.status == st) out.push_back(r);
    return out;
}

bool any_proof(const std::vector<prism::Finding>& recs) {
    return std::any_of(recs.begin(), recs.end(), [](auto& r) { return prism::laws::is_proof(r.status); });
}

const char* kLlmSkipMsg = "llama.cpp/Ollama not reachable; stall mutants / autoprompt skipped";

#ifndef _WIN32
// Environment variables set for one case and restored after it.
struct ScopedEnv {
    std::map<std::string, std::optional<std::string>> saved;
    void set(const char* k, const std::optional<std::string>& v) {
        if (!saved.contains(k)) {
            auto* cur = std::getenv(k);
            saved[k] = cur ? std::optional<std::string>(cur) : std::nullopt;
        }
        if (v) setenv(k, v->c_str(), 1);
        else unsetenv(k);
    }
    ~ScopedEnv() {
        for (auto& [k, v] : saved) {
            if (v) setenv(k.c_str(), v->c_str(), 1);
            else unsetenv(k.c_str());
        }
    }
};

// A port nothing listens on: bound, read back, closed.
int closed_port() {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    ::close(s);
    return ntohs(a.sin_port);
}

// No model anywhere: llama-server and Ollama endpoints refuse, no GGUF.
void llm_down(ScopedEnv& env) {
    auto dead = "http://127.0.0.1:" + std::to_string(closed_port());
    env.set("PRISM_LLAMA_SERVER", dead);
    env.set("OLLAMA_HOST", dead);
    env.set("PRISM_GGUF", (fs::temp_directory_path() / "prism_no_such_model.gguf").string());
}

// A llama-server stand-in: /health is up, /v1/chat/completions answers with
// reply(system prompt) and records every system prompt it saw.
struct FakeLlm {
    int lfd = -1;
    int port = 0;
    std::atomic<bool> stop{false};
    std::thread th;
    std::mutex mu;
    std::vector<std::string> systems;
    std::function<std::string(const std::string&)> reply;

    explicit FakeLlm(std::function<std::string(const std::string&)> r) : reply(std::move(r)) {
        lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        socklen_t len = sizeof a;
        ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        REQUIRE(::listen(lfd, 16) == 0);
        th = std::thread([this] { loop(); });
    }
    ~FakeLlm() {
        stop = true;
        th.join();
        ::close(lfd);
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
    int count(const std::string& prefix) {
        std::lock_guard<std::mutex> lock(mu);
        return static_cast<int>(
            std::count_if(systems.begin(), systems.end(), [&](auto& s) { return s.rfind(prefix, 0) == 0; }));
    }
    void loop() {
        while (!stop) {
            pollfd p{lfd, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) continue;
            int c = ::accept(lfd, nullptr, nullptr);
            if (c < 0) continue;
            serve(c);
            ::close(c);
        }
    }
    void serve(int c) {
        std::string raw;
        char buf[4096];
        std::size_t hdr = std::string::npos;
        while ((hdr = raw.find("\r\n\r\n")) == std::string::npos) {
            auto n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) return;
            raw.append(buf, static_cast<std::size_t>(n));
        }
        auto head = lower(raw.substr(0, hdr));
        std::size_t clen = 0;
        if (auto k = head.find("content-length:"); k != std::string::npos)
            clen = std::stoul(head.substr(k + 15, head.find("\r\n", k) - (k + 15)));
        while (raw.size() < hdr + 4 + clen) {
            auto n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) return;
            raw.append(buf, static_cast<std::size_t>(n));
        }
        std::string body = "{}";
        if (head.find(" /v1/chat/completions") != std::string::npos) {
            auto req = nlohmann::json::parse(raw.substr(hdr + 4, clen));
            std::string system;
            for (auto& m : req["messages"])
                if (m["role"] == "system") system = m["content"].get<std::string>();
            {
                std::lock_guard<std::mutex> lock(mu);
                systems.push_back(system);
            }
            body = nlohmann::json{{"choices", {{{"message", {{"content", reply(system)}}}}}}}.dump();
        }
        auto resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        ::send(c, resp.data(), resp.size(), 0);
    }
};

// System prompts of the fuse stage's model calls.
const std::string kSeeds = "You distill a fuzzing prompt";
const std::string kAuto = "You are an auto-prompting tool";
const std::string kMutate = "The previous generation was interesting";
const std::string kCombine = "Combine the two previous interesting encodings";

std::function<std::string(const std::string&)> replies(std::vector<std::string> seeds, std::vector<std::string> mutants,
                                                       std::vector<std::string> combined) {
    return [=](const std::string& sys) -> std::string {
        if (sys.rfind(kSeeds, 0) == 0) return nlohmann::json{{"prompt", "p"}, {"seeds", seeds}}.dump();
        if (sys.rfind(kAuto, 0) == 0) return "clamp x into [0, 100]";
        if (sys.rfind(kMutate, 0) == 0) return nlohmann::json{{"mutants", mutants}}.dump();
        if (sys.rfind(kCombine, 0) == 0) return nlohmann::json{{"mutants", combined}}.dump();
        return nlohmann::json{{"mutants", nlohmann::json::array()}}.dump();
    };
}

fs::path bin_dir(const char* name) {
    auto d = fs::temp_directory_path() / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}
#endif

}  // namespace

// ---- goals

TEST_CASE("fuse goals: then, implicit else, loop exit and switch cases, no duplicates") {
    auto goals = prism::branch_goals(fn_named("fuse_goals.c", "fuse_goals"));
    for (auto* g : {"x > 0", "!(x > 0)", "n > 0", "!(n > 0)", "(k) == (1)", "(k) == (2)"}) {
        INFO(g);
        CHECK(has(goals, g));
    }
    CHECK(goals.size() >= 6);
    CHECK(std::set<std::string>(goals.begin(), goals.end()).size() == goals.size());
}

TEST_CASE("fuse goals: both polarities of every branch in saturate") {
    auto goals = prism::branch_goals(fn_named("saturate.c", "saturate"));
    for (auto* g : {"x > 100", "!(x > 100)", "x < 0", "!(x < 0)"}) {
        INFO(g);
        CHECK(has(goals, g));
    }
}

TEST_CASE("fuse goals: GOAL_n labels follow FuSeBMC numbering") {
    auto f = fn_named("fuse_goals.c", "fuse_goals");
    auto goals = prism::branch_goals(f);
    auto labeled = prism::numbered_goals(f);
    REQUIRE(labeled.size() == goals.size());
    for (std::size_t i = 0; i < labeled.size(); ++i) {
        CHECK(labeled[i].first == "GOAL_" + std::to_string(i + 1));
        CHECK(labeled[i].second == goals[i]);
    }
}

TEST_CASE("fuse: a CLEAN run carries its goal ids and map and is not a proof") {
    auto f = fn_named("fuse_goals.c", "fuse_goals");
    auto goals = prism::branch_goals(f);
    auto labeled = prism::numbered_goals(f);
    auto recs = prism::run_fuse({f}, {}, td(), 0.2, 4, false);
    REQUIRE(recs.size() == 1);
    auto& r = recs[0];
    INFO(r.status << ": " << r.message);
    CHECK(r.status == prism::laws::CLEAN);
    CHECK(r.status != prism::laws::NOTRUN);
    CHECK(r.message.find("not a proof") != std::string::npos);
    auto ids = json_list(r, "goal_ids");
    REQUIRE(ids.size() == goals.size());
    CHECK(ids[0] == "GOAL_1");
    auto gmap = nlohmann::json::parse(extra_or(r, "goal_map"));
    CHECK(gmap["GOAL_1"] == goals[0]);
    std::vector<std::string> want;
    for (auto& [lab, _] : labeled) want.push_back(lab);
    CHECK(json_list(r, "goals") == want);
}

#ifdef PRISM_HAS_Z3
TEST_CASE("fuse: a spent fuzz budget skips the goal BMC and proves nothing") {
    auto recs = prism::run_fuse({fn_named("fuse_goals.c", "fuse_goals")}, {}, td(), 0.0, 4, false);
    REQUIRE_FALSE(recs.empty());
    CHECK(extra_or(recs[0], "bmc_goals_skipped").find("fuzz budget spent") != std::string::npos);
    CHECK(recs[0].status != prism::laws::PROVED);
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
}
#endif

// ---- Fuzz4All scoring and prompt ingredients

TEST_CASE("fuzz4all: a prompt scores its unique valid encodings") {
    CHECK(prism::score_prompt_seeds({Bytes{0, 0, 0, 0}, Bytes{0, 0, 0, 0}}, 4) == 1);
    CHECK(prism::score_prompt_seeds({Bytes{1, 0, 0, 0}, Bytes{2, 0, 0, 0}}, 4) == 2);
    CHECK(prism::score_prompt_seeds({Bytes{}}, 4) == 0);
}

TEST_CASE("fuzz4all: the best-scoring prompt wins; none scores 0") {
    auto pick = prism::pick_best_prompt(
        {{"p1", {Bytes{0, 0, 0, 0}, Bytes{0, 0, 0, 0}}}, {"p2", {Bytes{1, 0, 0, 0}, Bytes{2, 0, 0, 0}}}}, 4);
    CHECK(pick.prompt == "p2");
    CHECK(pick.score == 2);
    CHECK(pick.seeds.size() == 2);
    auto tie = prism::pick_best_prompt({{"a", {Bytes{1, 0, 0, 0}}}, {"b", {Bytes{2, 0, 0, 0}}}}, 4);
    CHECK(tie.prompt == "a");
    auto none = prism::pick_best_prompt({}, 4);
    CHECK(none.prompt.empty());
    CHECK(none.seeds.empty());
    CHECK(none.score == 0);
}

TEST_CASE("fuzz4all: update strategies generate, mutate, stay equivalent, combine") {
    CHECK(prism::fuzz4all_update_strategy("aa", "", 0).find("generate a new encoding") != std::string::npos);
    CHECK(prism::fuzz4all_update_strategy("aa", "", 1).find("mutate the previous generation") != std::string::npos);
    CHECK(prism::fuzz4all_update_strategy("aa", "", 2).find("semantically equivalent") != std::string::npos);
    auto comb = prism::fuzz4all_update_strategy("aa", "bb", 3);
    CHECK(comb.find("prev=bb") != std::string::npos);
    CHECK(comb.find("combine") != std::string::npos);
    CHECK(prism::fuzz4all_update_strategy("aa", "", 3).find("mutate the previous generation") != std::string::npos);
}

TEST_CASE("fuzz4all: documentation comments become the prompt ingredients") {
    auto f = fn_named("fuzz4all_docs.c", "fuzz4all_docs");
    std::ifstream in(td() / "fuzz4all_docs.c", std::ios::binary);
    std::string src{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    auto docs = prism::documentation_from_comments(src);
    CHECK(docs.find("clamp x into [0, 100]") != std::string::npos);
    CHECK(docs.find("signed integer") != std::string::npos);
    auto p = prism::create_prompt_from_source(f.name, f.body, src);
    CHECK(p.target_api == "fuzz4all_docs");
    CHECK(p.docstring.find("clamp") != std::string::npos);
    CHECK_FALSE(p.hw_prompt.empty());
    // Contract lines and ACSL blocks are not documentation.
    CHECK(prism::documentation_from_comments("// requires: x > 0\n/*@ ensures \\result > 0; */\n").empty());
}

// ---- the loop

TEST_CASE("fuse: without an LLM there is one CLEAN record and no NOTRUN row") {
    auto recs = prism::run_fuse({fn_named("saturate.c", "saturate")}, {}, td(), 0.2, 4, false);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == prism::laws::CLEAN);
    CHECK(recs[0].status != prism::laws::NOTRUN);
    CHECK(recs[0].message.find("not a proof") != std::string::npos);
}

TEST_CASE("fuse: a POINTER function is NEEDS-HARNESS, not ERROR") {
    auto f = fn_named("null_branch.c", "null_branch");
    REQUIRE(f.kind == "POINTER");
    auto recs = prism::run_fuse({f}, {}, td(), 0.1, 1, false);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == prism::laws::NEEDS_HARNESS);
    CHECK(recs[0].status != prism::laws::ERROR);
    CHECK(recs[0].message.find("POINTER") != std::string::npos);
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
}

#ifndef _WIN32
TEST_CASE("fuse: an unreachable LLM is a NOTRUN row, never CLEAN or a proof") {
    ScopedEnv env;
    llm_down(env);
    auto recs = prism::run_fuse({fn_named("saturate.c", "saturate")}, {}, td(), 0.2, 4, true);
    auto notrun = with_status(recs, prism::laws::NOTRUN);
    REQUIRE_FALSE(notrun.empty());
    auto& n = notrun[0];
    CHECK(n.message == kLlmSkipMsg);
    CHECK(extra_or(n, "autoprompt") == "NOTRUN");
    CHECK(extra_or(n, "chatfuzz") == "NOTRUN");
    CHECK(n.status != prism::laws::CLEAN);
    CHECK(n.status != prism::laws::ERROR);
    CHECK(n.strength == prism::laws::STRENGTH_READS);
    CHECK_FALSE(any_proof(recs));
    for (auto& r : recs) CHECK(r.status != prism::laws::BOUNDED);
}

TEST_CASE("fuse: POINTER with an unreachable LLM is NEEDS-HARNESS plus the NOTRUN row") {
    ScopedEnv env;
    llm_down(env);
    auto recs = prism::run_fuse({fn_named("null_branch.c", "null_branch")}, {}, td(), 0.1, 1, true);
    auto ptr = with_status(recs, prism::laws::NEEDS_HARNESS);
    REQUIRE_FALSE(ptr.empty());
    CHECK(ptr[0].message.find("POINTER") != std::string::npos);
    auto auto_rows = std::count_if(recs.begin(), recs.end(), [](auto& r) { return extra_or(r, "autoprompt") == "NOTRUN"; });
    CHECK(auto_rows >= 1);
    for (auto& r : recs) {
        CHECK(r.status != prism::laws::ERROR);
        CHECK(r.status != prism::laws::PROVED);
    }
    CHECK_FALSE(any_proof(recs));
}

TEST_CASE("fuse: without the file, the NOTRUN row's documentation comes from the function text") {
    ScopedEnv env;
    llm_down(env);
    prism::FunctionInfo f;
    f.file = "no_such_dir/nowhere.c";
    f.name = "nowhere";
    f.kind = "SCALAR";
    f.line = 1;
    f.signature = "int nowhere(int x)";
    f.params = {{"int", "x"}};
    f.body = "    // usage: pass a signed integer\n    return x;";
    auto recs = prism::run_fuse({f}, {}, fs::temp_directory_path() / "prism_fuse_nowhere", 0.1, 1, true);
    auto notrun = with_status(recs, prism::laws::NOTRUN);
    REQUIRE_FALSE(notrun.empty());
    // The body's own comment is the documentation left.
    CHECK(extra_or(notrun[0], "docstring").find("signed integer") != std::string::npos);
}

TEST_CASE("fuse: an interesting input is mutated once by the model") {
    FakeLlm llm(replies({}, {"03000000"}, {}));
    ScopedEnv env;
    env.set("PRISM_LLAMA_SERVER", llm.url());
    auto recs = prism::run_fuse({fn_named("saturate.c", "saturate")}, {}, td(), 0.2, 4, true);
    REQUIRE_FALSE(recs.empty());
    INFO(recs[0].status << ": " << recs[0].message);
    CHECK(llm.count(kMutate) == 1);
    CHECK(recs[0].status == prism::laws::CLEAN);
    CHECK(extra_or(recs[0], "fuzz4all_mutate") == "true");
    CHECK_FALSE(any_proof(recs));
}

TEST_CASE("fuse: two interesting inputs are combined once by the model") {
    FakeLlm llm(replies({"01000000", "02000000"}, {}, {"04000000"}));
    ScopedEnv env;
    env.set("PRISM_LLAMA_SERVER", llm.url());
    auto recs = prism::run_fuse({fn_named("saturate.c", "saturate")}, {}, td(), 0.2, 4, true);
    REQUIRE_FALSE(recs.empty());
    CHECK(llm.count(kCombine) == 1);
    CHECK(extra_or(recs[0], "fuzz4all_combine") == "true");
    CHECK(recs[0].status == prism::laws::CLEAN);
    CHECK_FALSE(any_proof(recs));
}

TEST_CASE("fuse: a model seed that reaches a branch records its GOAL label; the prompt is a HYPOTHESIS") {
    // x = 101 takes the x > 100 branch: GOAL_1.
    FakeLlm llm(replies({"65000000"}, {}, {}));
    ScopedEnv env;
    env.set("PRISM_LLAMA_SERVER", llm.url());
    auto recs = prism::run_fuse({fn_named("saturate.c", "saturate")}, {}, td(), 0.2, 4, true);
    REQUIRE_FALSE(recs.empty());
    auto& fuse = recs[0];
    CHECK(fuse.status == prism::laws::CLEAN);
    CHECK(has(json_list(fuse, "goals"), "GOAL_1"));
    CHECK(has(json_list(fuse, "new_goals"), "GOAL_1"));
    CHECK(has(json_list(fuse, "covered_goals"), "GOAL_1"));
    auto hyps = with_status(recs, prism::laws::HYPOTHESIS);
    REQUIRE_FALSE(hyps.empty());
    CHECK(hyps[0].strength == prism::laws::STRENGTH_READS);
    CHECK_FALSE(any_proof(recs));
}

TEST_CASE("fuse: the distilled prompt is a HYPOTHESIS with the documentation, not CLEAN") {
    FakeLlm llm(replies({}, {}, {}));
    ScopedEnv env;
    env.set("PRISM_LLAMA_SERVER", llm.url());
    auto recs = prism::run_fuse({fn_named("fuzz4all_docs.c", "fuzz4all_docs")}, {}, td(), 0.2, 4, true);
    auto hyps = with_status(recs, prism::laws::HYPOTHESIS);
    REQUIRE_FALSE(hyps.empty());
    CHECK(hyps[0].strength == prism::laws::STRENGTH_READS);
    CHECK(extra_or(hyps[0], "docstring").find("clamp") != std::string::npos);
    CHECK(extra_or(hyps[0], "target_api") == "fuzz4all_docs");
    CHECK(hyps[0].status != prism::laws::CLEAN);
    CHECK_FALSE(any_proof(recs));
    CHECK(llm.count(kAuto) >= 1);
}

TEST_CASE("fuse: AFL++ without a C compiler is NOTRUN and does not take the greybox CLEAN") {
    prism::sandbox::Policy policy(true);
    auto bin = bin_dir("prism_fuse_afl_nocc");
    std::ofstream(bin / "afl-fuzz") << "#!/bin/sh\nexit 1\n";
    fs::permissions(bin / "afl-fuzz", fs::perms::owner_all);
    ScopedEnv env;
    env.set("PRISM_AFL", "1");
    env.set("PRISM_LIBFUZZER", std::nullopt);
    env.set("PATH", bin.string());
    auto recs = prism::run_fuse({fn_named("saturate.c", "saturate")}, {}, td(), 0.2, 4, false);
    REQUIRE(recs.size() == 1);
    auto& r = recs[0];
    INFO(r.status << ": " << r.message);
    CHECK(r.status == prism::laws::CLEAN);
    CHECK(r.status != prism::laws::NOTRUN);
    CHECK(r.status != prism::laws::ERROR);
    CHECK(extra_or(r, "engine") != "afl");
    CHECK(extra_or(r, "afl") == "NOTRUN");
    CHECK(extra_or(r, "install") == "install gcc or clang");
    CHECK(lower(r.message).find("not a proof") != std::string::npos);
    CHECK_FALSE(prism::laws::is_proof(r.status));
}

TEST_CASE("fuse: PRISM_AFL=1 without afl-fuzz is NOTRUN with the fetch hint, not engine=afl") {
    auto bin = bin_dir("prism_fuse_noafl");
    ScopedEnv env;
    env.set("PRISM_AFL", "1");
    env.set("PRISM_LIBFUZZER", std::nullopt);
    env.set("PATH", bin.string());
    auto recs = prism::run_fuse({fn_named("saturate.c", "saturate")}, {}, td(), 0.2, 4, false);
    REQUIRE(recs.size() == 1);
    auto& r = recs[0];
    CHECK(r.status == prism::laws::CLEAN);
    CHECK(extra_or(r, "afl") == "NOTRUN");
    CHECK(extra_or(r, "engine") != "afl");
    CHECK(extra_or(r, "install").find("prism-deps tool aflplusplus") != std::string::npos);
    CHECK(lower(r.message).find("not a proof") != std::string::npos);
    CHECK_FALSE(prism::laws::is_proof(r.status));
}

TEST_CASE("fuse: libFuzzer without clang is NOTRUN and does not claim the engine") {
    prism::sandbox::Policy policy(true);
    auto bin = bin_dir("prism_fuse_noclang");
    ScopedEnv env;
    env.set("PRISM_LIBFUZZER", "1");
    env.set("PRISM_AFL", std::nullopt);
    env.set("PATH", bin.string());
    auto recs = prism::run_fuse({fn_named("saturate.c", "saturate")}, {}, td(), 0.2, 4, false);
    REQUIRE(recs.size() == 1);
    auto& r = recs[0];
    INFO(r.status << ": " << r.message);
    CHECK(r.status == prism::laws::CLEAN);
    CHECK(extra_or(r, "libfuzzer") == "NOTRUN");
    CHECK(extra_or(r, "engine") != "libfuzzer");
    CHECK(extra_or(r, "engine") != "afl");
    CHECK(lower(r.message).find("not a proof") != std::string::npos);
    CHECK_FALSE(prism::laws::is_proof(r.status));
}
#endif
