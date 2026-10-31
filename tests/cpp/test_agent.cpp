// The model-driven stage pieces (src/prism/stages/: llm, hypothesize,
// execute_cex, rlef, fuse): the chat engine and its test seam, reply parsing,
// the sandbox verdict and RLEF reward, the interpreter and repair loops with a
// replaceable model and BMC oracle, counterexample replay parsing, and the
// Dafny-style contract hypotheses. Model output never proves anything about
// the scanned code (Law 4); a missing model or compiler is NOTRUN (Law 1).

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "../../src/prism/stages/agent.hpp"

#include "forge_test_model.hpp"
#include "prism/config.hpp"
#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"
#include "prism/stages.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace sd = prism::stages_detail;
using json = nlohmann::json;

namespace {

using Messages = std::vector<std::pair<std::string, std::string>>;

// Installs a chat function for one scope.
struct ChatDouble {
    std::vector<Messages> seen;
    std::vector<sd::ChatResult> replies;
    std::size_t next = 0;
    std::mutex mu;
    ChatDouble() {
        sd::set_chat_backend_for_testing([this](const Messages& m, double) {
            std::lock_guard<std::mutex> g(mu);
            seen.push_back(m);
            if (replies.empty()) return sd::ChatResult{"", "test-double", "no reply scripted"};
            auto r = replies[std::min(next, replies.size() - 1)];
            ++next;
            if (r.backend.empty()) r.backend = "test-double";
            return r;
        });
    }
    ~ChatDouble() { sd::set_chat_backend_for_testing(nullptr); }
};

fs::path agent_tmp(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("prism-agent-" + tag);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

prism::Config offline_cfg(const fs::path& out) {
    prism::Config cfg = prism::default_config();
    cfg.out = out;
    cfg.fpk.clear();
    return cfg;
}

bool have_cc() { return sd::which_cc().has_value(); }


}  // namespace

// ------------------------------------------------------------------ reply parsing
TEST_CASE("agent: the C file of a model reply") {
    CHECK(sd::c_from_llm("```int main(){}```").empty());  // a fence line with no newline holds no file
    CHECK(sd::c_from_llm("```c\nint x;\n```") == "int x;");
    CHECK(sd::c_from_llm("Here it is:\n```c\nint y;\n```\nDone.") == "int y;");
    CHECK(sd::c_from_llm("int z;") == "int z;");
    CHECK(sd::c_from_llm("  \n ").empty());
    CHECK(sd::c_from_llm("see ```inline``` here") == "inline");
}

TEST_CASE("agent: hex seeds are read like bytes.fromhex after dropping 0x") {
    using B = std::vector<uint8_t>;
    CHECK(sd::parse_hex_bytes("0x01 02") == B{1, 2});
    CHECK(sd::parse_hex_bytes("0X0a0B") == B{10, 11});
    CHECK(sd::parse_hex_bytes("de ad\tbe\nef") == B{0xde, 0xad, 0xbe, 0xef});
    CHECK(sd::parse_hex_bytes("0x10 0x20") == B{0x10, 0x20});
    CHECK_FALSE(sd::parse_hex_bytes("0 1"));  // a byte is never split
    CHECK_FALSE(sd::parse_hex_bytes("abc"));
    CHECK_FALSE(sd::parse_hex_bytes("zz"));
    CHECK(sd::parse_hex_bytes("") == B{});
    const auto data = json::parse(R"({"seeds": ["0102", 1234, "", "zz", "0x ff", null]})");
    CHECK(sd::json_hex_list(data, "seeds") == std::vector<B>{{1, 2}, {0x12, 0x34}, {0xff}});
    CHECK(sd::json_hex_list(json::parse(R"({"seeds": "0102"})"), "seeds").empty());
    CHECK(sd::score_prompt_seeds({{1, 2, 3}, {1, 2}, {1, 2, 9}, {}}, 2) == 1);
}

TEST_CASE("agent: documentation comments leave out contracts and ACSL") {
    const std::string src =
        "/* requires : x > 0 */\n"
        "// diff: old behaviour\n"
        "/** Adds\n *  two   numbers\n **/\n"
        "// Ensures: nothing\n"
        "// plain note\n"
        "/*@ ensures \\result > 0; */\n"
        "int add(int a, int b) { return a + b; }\n";
    CHECK(sd::documentation_from_comments(src) == "Adds two numbers\nplain note");
    std::string many;
    for (int i = 0; i < 12; ++i) many += "// note " + std::to_string(i) + "\n";
    const auto d = sd::documentation_from_comments(many);
    CHECK(d.find("note 7") != std::string::npos);
    CHECK(d.find("note 8") == std::string::npos);  // at most 8 comments
}

// ------------------------------------------------------------------ verdicts and rewards
TEST_CASE("agent: the sandbox verdict never proves; a missing compiler is NOTRUN") {
    CHECK(sd::sandbox_verdict({{"ok", false}, {"error", "no compiler"}}) == prism::laws::NOTRUN);
    CHECK(sd::sandbox_verdict({{"ok", false}, {"error", "exec-disabled"}}) == prism::laws::NOTRUN);
    CHECK(sd::sandbox_verdict({{"ok", true}, {"error", nullptr}}) == prism::laws::CLEAN);
    CHECK(sd::sandbox_verdict({{"ok", false}, {"error", "crash"}}) == prism::laws::CRASH);
    CHECK(sd::sandbox_verdict({{"ok", false}, {"error", "exit"}}) == prism::laws::FAILED);
    CHECK(sd::sandbox_verdict({{"ok", false}, {"error", "compile"}}) == prism::laws::FAILED);
    // A timeout is never CLEAN, even when ok was set or partial output came back.
    CHECK(sd::sandbox_verdict({{"ok", false}, {"error", "timeout"}, {"stdout", "partial"}}) ==
          prism::laws::FAILED);
    CHECK(sd::sandbox_verdict({{"ok", true}, {"error", "timeout"}, {"stdout", "partial"}}) ==
          prism::laws::FAILED);
    CHECK(sd::sandbox_verdict({{"ok", false}, {"error", "compile-timeout"}}) == prism::laws::FAILED);
    const auto off = sd::sandbox_run("int main(void){return 0;}", 2.0, /*allow_exec=*/false);
    CHECK(off["error"] == "exec-disabled");  // Law 9
    CHECK(off["stdout"] == "");
    CHECK(off["stderr"] == "");
}

TEST_CASE("agent: sandbox output keeps its tail, and a timeout keeps what was printed") {
    auto ends = [](const std::string& s, const std::string& suf) {
        return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    };
    const std::string body(2500, 'A');
    const std::string marked = body + "TAIL";
    const auto t400 = sd::tail(marked, 400);
    const auto t2000 = sd::tail(marked, 2000);
    CHECK(t400.size() == 400);
    CHECK(t2000.size() == 2000);
    CHECK(ends(t400, "TAIL"));
    CHECK(ends(t2000, "TAIL"));
    // The same limits used to keep the prefix, which never reaches TAIL.
    CHECK(marked.substr(0, 400).find("TAIL") == std::string::npos);
    CHECK(marked.substr(0, 2000).find("TAIL") == std::string::npos);
    const std::string utf = std::string(8, 'a') + "\xE2\x82\xAC" + "Z";
    const auto cut = sd::tail(utf, 2);
    CHECK(ends(cut, "Z"));
    if (!cut.empty()) CHECK((static_cast<unsigned char>(cut.front()) & 0xC0u) != 0x80u);

    if (!have_cc()) {
        MESSAGE("no gcc/clang: sandbox_run tail and timeout not exercised");
        return;
    }
    auto printed = sd::sandbox_run(
        "#include <stdio.h>\n"
        "int main(void) {\n"
        "  int i;\n"
        "  for (i = 0; i < 3000; ++i) putchar('B');\n"
        "  fputs(\"USEFUL-TAIL\\n\", stdout);\n"
        "  return 0;\n"
        "}\n",
        8.0, true);
    CAPTURE(printed.dump());
    REQUIRE(printed["ok"] == true);
    const auto out = printed["stdout"].get<std::string>();
    CHECK(out.size() == 2000);
    CHECK(out.find("USEFUL-TAIL") != std::string::npos);

    auto timed = sd::sandbox_run(
        "#include <stdio.h>\n"
        "int main(void) {\n"
        "  volatile int spin = 1;\n"
        "  fputs(\"PARTIAL-OUT\\n\", stdout);\n"
        "  fflush(stdout);\n"
        "  while (spin) continue;\n"
        "  return 0;\n"
        "}\n",
        2.0, true);
    CAPTURE(timed.dump());
    CHECK(timed["ok"] == false);
    CHECK(timed["error"] == "timeout");
    CHECK(timed["code"].is_null());
    CHECK(timed["stdout"].get<std::string>().find("PARTIAL-OUT") != std::string::npos);
    CHECK(sd::sandbox_verdict(timed) == prism::laws::FAILED);
    CHECK(sd::sandbox_verdict(timed) != prism::laws::CLEAN);

    auto dir = agent_tmp("tail");
    ChatDouble chat;
    chat.replies.push_back(
        {"```c\n#include <stdio.h>\n"
         "int main(void) {\n"
         "  int i;\n"
         "  for (i = 0; i < 500; ++i) putchar('X');\n"
         "  fputs(\"MARK\\n\", stdout);\n"
         "  return 0;\n"
         "}\n```",
         "", ""});
    sd::LlmEngine engine(offline_cfg(dir));
    auto clean = sd::interpreter_loop(engine, "show the tail", 1, true);
    REQUIRE(clean.size() == 1);
    CAPTURE(clean[0].message);
    CHECK(clean[0].status == prism::laws::CLEAN);
    CHECK(clean[0].message.find("not a proof") != std::string::npos);
    CHECK(clean[0].extra.count("sandbox") == 1);
    CHECK(clean[0].extra.count("code") == 0);
    const auto so = clean[0].extra.at("stdout");
    CHECK(so.size() == 400);
    CHECK(so.find("MARK") != std::string::npos);
}

TEST_CASE("agent: the RLEF reward; only a BMC proof of the patch is terminal, BOUNDED is not") {
    auto r = sd::rlef_reward({{"ok", false}, {"error", "compile"}}, "");
    CHECK(r.score == 0);
    CHECK(r.proved.empty());
    CHECK(sd::rlef_reward({{"ok", true}, {"error", nullptr}}, "").score == 3);
    CHECK(sd::rlef_reward({{"ok", false}, {"error", "crash"}}, "").score == 0);
    CHECK(sd::rlef_reward({{"ok", false}, {"error", "exit"}}, prism::laws::FAILED).score == -1);
    CHECK(sd::rlef_reward({{"ok", true}, {"error", nullptr}}, prism::laws::PROVED).proved == prism::laws::PROVED);
    auto b = sd::rlef_reward({{"ok", true}, {"error", nullptr}}, prism::laws::BOUNDED);
    CHECK(b.proved.empty());
    CHECK(b.score == 3);
    CHECK(sd::rlef_reward({{"ok", true}, {"error", nullptr}}, prism::laws::CLEAN).proved.empty());
}

// ------------------------------------------------------------------ the chat engine
TEST_CASE("agent: the chat seam binds every engine; unreachable servers are NOTRUN, not ERROR") {
    auto out = agent_tmp("engine");
    {
        sd::LlmEngine none(offline_cfg(out));
        CHECK_FALSE(none.available());
        auto r = none.complete({{"user", "hi"}});
        CHECK_FALSE(r.error.empty());
        CHECK(sd::llm_httpish(r.error));
    }
    ChatDouble chat;
    chat.replies.push_back({"hello", "", "", json{{"id", 1}}});
    sd::LlmEngine e(offline_cfg(out));
    REQUIRE(e.available());
    CHECK(e.backend == "test-double");
    auto r = e.complete({{"system", "s"}, {"user", "u"}});
    CHECK(r.text == "hello");
    CHECK(r.raw["id"] == 1);
    REQUIRE(chat.seen.size() == 1);
    CHECK(chat.seen[0][1].second == "u");
}

#ifndef _WIN32

#endif

// ------------------------------------------------------------------ interpreter and repair loops
TEST_CASE("agent: the interpreter loop runs model C only with --allow-exec; CLEAN and CRASH carry the sandbox") {
    auto out = agent_tmp("interp");
    ChatDouble chat;
    chat.replies.push_back({"```c\n#include <stdio.h>\nint main(void) { puts(\"hi\"); return 0; }\n```", "", ""});
    sd::LlmEngine e(offline_cfg(out));
    auto off = sd::interpreter_loop(e, "prove it", 2, /*allow_exec=*/false);
    REQUIRE(off.size() == 1);
    CHECK(off[0].status == prism::laws::NOTRUN);
    CHECK(chat.seen.empty());  // Law 9: not even asked
    if (!have_cc()) {
        auto nc = sd::interpreter_loop(e, "prove it", 2, true);
        CHECK(nc[0].status == prism::laws::NOTRUN);
        return;
    }
#ifdef _WIN32
    // NOTRUN: the CLEAN / CRASH shapes below need the POSIX sandbox with a gcc-style
    // compiler (stdout "hi\n", SIGSEGV); the MSVC-target build has neither here.
    if (false) {
#endif
    auto clean = sd::interpreter_loop(e, "prove it", 2, true);
    REQUIRE(clean.size() == 1);
    CAPTURE(clean[0].message);
    CHECK(clean[0].status == prism::laws::CLEAN);
    CHECK(clean[0].message.find("not a proof") != std::string::npos);
    CHECK(clean[0].extra.at("stdout") == "hi\n");
    CHECK(clean[0].extra.count("sandbox") == 1);
    chat.replies = {{"```c\n#include <signal.h>\nint main(void) { raise(SIGSEGV); return 0; }\n```", "", ""}};
    chat.next = 0;
    auto crash = sd::interpreter_loop(e, "prove it", 2, true);
    REQUIRE(crash.size() == 1);
    CHECK(crash[0].status == prism::laws::CRASH);
    CHECK(crash[0].extra.count("code") == 1);
    CHECK(crash[0].extra.count("sandbox") == 1);
#ifdef _WIN32
    }
#endif
    // A model that errors: HTTP-ish is NOTRUN, anything else is ERROR.
    chat.replies = {{"", "", "HTTP 503: busy"}};
    chat.next = 0;
    CHECK(sd::interpreter_loop(e, "p", 1, true)[0].status == prism::laws::NOTRUN);
    chat.replies = {{"", "", "timed out"}};
    chat.next = 0;
    CHECK(sd::interpreter_loop(e, "p", 1, true)[0].status == prism::laws::ERROR);
}

TEST_CASE("agent: RLEF repair with a replaceable BMC oracle; a proof is about the patch only") {
    auto td = agent_tmp("rlef");
    std::ofstream(td / "div.c") << "int f(int a, int b) {\n    return a / b;\n}\n";
    prism::Finding fail;
    fail.stage = "bmc";
    fail.status = std::string(prism::laws::FAILED);
    fail.file = (td / "div.c").string();
    fail.function = "f";
    fail.line = 1;
    fail.cls = "INT-DIV-ZERO";
    fail.message = "div0";
    auto cfg = offline_cfg(td / "out");
    cfg.repair_rounds = 1;
    ChatDouble chat;
    chat.replies.push_back(
        {"```c\nint f(int a, int b) {\n    return b == 0 ? 0 : a / b;\n}\nint main(void) { return f(1, 0); }\n```", "",
         ""});
    // Without --allow-exec nothing runs (Law 9).
    auto off = sd::rlef_repair_with(fail, cfg, [](const std::string&) { return std::string("PROVED"); });
    REQUIRE(off.size() == 1);
    CHECK(off[0].status == prism::laws::NOTRUN);
    if (!have_cc()) return;
    cfg.allow_exec = true;
    std::vector<std::string> asked;
    auto proved = sd::rlef_repair_with(fail, cfg, [&](const std::string& src) {
        asked.push_back(src);
        return std::string(prism::laws::PROVED);
    });
    REQUIRE(proved.size() == 1);
    CHECK(asked.size() == 1);
    CHECK(proved[0].status == prism::laws::HYPOTHESIS);  // Law 4: never a verdict on the scanned code
    CHECK(proved[0].strength == prism::laws::STRENGTH_READS);
    CHECK(proved[0].extra.at("patch_verdict") == "PROVED");
    chat.next = 0;
    auto bounded = sd::rlef_repair_with(fail, cfg, [](const std::string&) { return std::string("BOUNDED"); });
    REQUIRE(bounded.size() == 1);
    CHECK(bounded[0].status == prism::laws::HYPOTHESIS);  // score >= 3, no BMC proof
    CHECK(bounded[0].strength == prism::laws::STRENGTH_READS);
    CHECK(bounded[0].message.find("best score") != std::string::npos);
    CHECK(bounded[0].message.find("not a proof") != std::string::npos);
    CHECK(bounded[0].extra.count("history") == 1);
    CHECK(bounded[0].extra.count("best") == 1);
    CHECK(bounded[0].status != prism::laws::PROVED);
    CHECK(bounded[0].status != prism::laws::PROVED_UNBOUNDED);
    chat.next = 0;
    auto thrown = sd::rlef_repair_with(fail, cfg, [](const std::string&) -> std::string { throw std::runtime_error("x"); });
    REQUIRE(thrown.size() == 1);
    CHECK(thrown[0].status != prism::laws::PROVED);
}

// ------------------------------------------------------------------ counterexample replay
TEST_CASE("agent: counterexample values are read like int(v, 0); others are not replayed") {
    auto td = agent_tmp("cex");
    std::ofstream(td / "d.c") << "int f(int a, int b) {\n    return 100 / (b - 16);\n}\n";
    auto fns = prism::extract_functions(td / "d.c", "d.c");
    REQUIRE(fns.size() == 1);
    REQUIRE(fns[0].kind == "SCALAR");
    auto cfg = offline_cfg(td / "out");
    cfg.llm = false;
    auto replay = [&](const std::string& cex) {
        prism::Finding f;
        f.stage = "bmc";
        f.status = std::string(prism::laws::FAILED);
        f.file = "d.c";
        f.function = "f";
        f.line = 1;
        f.cls = "INT-DIV-ZERO";
        f.counterexample = cex;
        return prism::execute_cex({f}, fns, cfg);
    };
    // "010" is refused (not octal 8); b = 0x10 = 16 divides by zero.
    auto a = replay("a=010, b=0x10");
    REQUIRE(a.size() == 1);
    CHECK(a[0].status == prism::laws::CRASH);
    CHECK(replay("int b =\t16")[0].status == prism::laws::CRASH);  // whitespace of any kind
    CHECK(replay("b=0b1_0000")[0].status == prism::laws::CRASH);
    CHECK(replay("b=17")[0].status == prism::laws::CLEAN);
    // An unsigned 32-bit pattern is replayed as its bits (the interpreter is LP64:
    // wider values are kept too); a number beyond 64 bits is not replayed.
    CHECK(replay("b=4294967295")[0].status == prism::laws::CLEAN);
    auto big = replay("b=99999999999999999999");
    REQUIRE(big.size() == 1);
    CHECK(big[0].status == prism::laws::NOTRUN);
    CHECK(big[0].message == "no FAILED/CRASH cex to replay");
    CHECK(replay("b=010")[0].status == prism::laws::NOTRUN);
}

// ------------------------------------------------------------------ Dafny-style contracts
TEST_CASE("agent: Dafny-style specs are hypotheses; no model is NOTRUN") {
    auto td = agent_tmp("dafny");
    std::ofstream(td / "c.c") << "int inc(int x) {\n    return x + 1;\n}\nvoid nop(void) {\n}\n";
    auto fns = prism::extract_functions(td / "c.c", "c.c");
    REQUIRE(fns.size() == 2);
    auto cfg = offline_cfg(td / "out");
    auto none = prism::dafny_specs(fns, 3, cfg);
    REQUIRE(none.size() == 1);
    CHECK(none[0].stage == "contracts");
    CHECK(none[0].status == prism::laws::NOTRUN);
    CHECK(none[0].strength == prism::laws::STRENGTH_READS);
    CHECK(none[0].cls == "FUNC-CONTRACT");
    CHECK(none[0].message == "no FORGE model (.fpk) configured");
    ChatDouble chat;
    chat.replies = {{R"({"requires":["x < 2147483647"],"ensures":["\\result == x + 1"]})", "", ""},
                    {"", "", "HTTP 500: oops"}};
    auto specs = prism::dafny_specs(fns, 3, cfg);
    REQUIRE(specs.size() == 2);
    CHECK(specs[0].status == prism::laws::HYPOTHESIS);
    CHECK(specs[0].strength == prism::laws::STRENGTH_READS);
    CHECK(specs[0].message == "Dafny-style spec (hypothesis, not proved)");
    CHECK(json::parse(specs[0].extra.at("spec"))["requires"][0] == "x < 2147483647");
    CHECK(specs[0].extra.at("backend") == "test-double");
    CHECK(specs[1].status == prism::laws::NOTRUN);
    CHECK(specs[1].extra.count("install") == 1);
    CHECK(prism::dafny_specs(fns, 0, cfg).empty());
}

TEST_CASE("agent: dafny_specs with no FORGE model is one NOTRUN FUNC-CONTRACT read") {
    auto td = agent_tmp("dafny-nogguf");
    std::ofstream(td / "c.c") << "int inc(int x) {\n    return x + 1;\n}\nvoid nop(void) {\n}\n";
    auto fns = prism::extract_functions(td / "c.c", "c.c");
    REQUIRE(fns.size() == 2);
    // Config{}, not default_config(): the latter fills fpk from PRISM_FPK or a guessed blob.
    prism::Config cfg;
    cfg.out = td / "out";
    cfg.fpk.clear();
    cfg.llm = true;
    REQUIRE(cfg.fpk.empty());
    auto specs = prism::dafny_specs(fns, 3, cfg);
    REQUIRE(specs.size() == 1);
    CHECK(specs[0].stage == "contracts");
    CHECK(specs[0].status == prism::laws::NOTRUN);
    CHECK(specs[0].strength == prism::laws::STRENGTH_READS);
    CHECK(specs[0].cls == "FUNC-CONTRACT");
    CHECK(specs[0].message == "no FORGE model (.fpk) configured");
    CHECK(specs[0].status != prism::laws::PROVED);
    CHECK(specs[0].status != prism::laws::PROVED_UNBOUNDED);
    CHECK(specs[0].status != prism::laws::PROVED_CERTIFIED);
    CHECK(specs[0].status != prism::laws::CLEAN);
    CHECK_FALSE(prism::laws::is_proof(specs[0].status));
}

TEST_CASE("agent: the contracts stage adds the model's specs beside its proofs, as hypotheses") {
    auto td = agent_tmp("contracts-stage");
    fs::create_directories(td / "src");
    std::ofstream(td / "src" / "c.c") << "int inc(int x) {\n    return x + 1;\n}\n";
    ChatDouble chat;
    chat.replies = {{R"({"requires":["x < 100"]})", "", ""}};
    auto cfg = offline_cfg(td / "out");
    cfg.root = td / "src";
    cfg.llm = true;
    cfg.stages = std::vector<std::string>{"inventory", "classify", "contracts"};
    auto rep = prism::run_pipeline(cfg);
    bool hyp = false;
    for (const auto& s : rep.stages)
        if (s.name == "contracts")
            for (const auto& f : s.findings)
                if (f.message == "Dafny-style spec (hypothesis, not proved)") {
                    hyp = true;
                    CHECK(f.status == prism::laws::HYPOTHESIS);
                }
    CHECK(hyp);
    cfg.llm = false;
    cfg.out = td / "out2";
    auto rep2 = prism::run_pipeline(cfg);
    for (const auto& s : rep2.stages)
        if (s.name == "contracts")
            for (const auto& f : s.findings) CHECK(f.message != "Dafny-style spec (hypothesis, not proved)");
}

TEST_CASE("repo lint: LLM stage strings and pipeline exports (ex test_llm.py)") {
    namespace fs = std::filesystem;
    auto slurp = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const fs::path root(PRISM_SOURCE_DIR);
    const auto llm_hpp = slurp(root / "src" / "prism" / "stages" / "llm.hpp");
    CHECK(llm_hpp.find("LLM_UNAVAILABLE_MSG") != std::string::npos);
    CHECK(llm_hpp.find("LLM_INSTALL") != std::string::npos);
    CHECK(llm_hpp.find("no FORGE model (.fpk) configured") != std::string::npos);
    const auto hyp = slurp(root / "src" / "prism" / "stages" / "hypothesize.cpp");
    CHECK(hyp.find("LLM_INSTALL") != std::string::npos);
    CHECK(hyp.find("llm_httpish") != std::string::npos);
    const auto pipe_hpp = slurp(root / "include" / "prism" / "pipeline.hpp");
    CHECK(pipe_hpp.find("llm_forced_reads") != std::string::npos);
    const auto tax = slurp(root / "src" / "prism" / "taxonomy.cpp");
    CHECK(tax.find("coverage_from_report") != std::string::npos);
}

// ---- FORGE-era replacements for the TinyHttp llama-server fakes ----
TEST_CASE("agent: a failed model call keeps its error; a good answer keeps its text") {
    auto out = agent_tmp("chat");
    {
        prism_test::ScriptedChat bad([](const std::string&) { return std::string(); });
        bad.fail_with("FORGE: context 4096 exceeded");
        sd::LlmEngine e(offline_cfg(out));
        REQUIRE(e.backend == "test-double");
        auto r = e.complete({{"user", "hi"}}, 5.0);
        CHECK(r.error == "FORGE: context 4096 exceeded");
        CHECK(r.text.empty());
    }
    prism_test::ScriptedChat ok([](const std::string&) { return std::string("fine"); });
    sd::LlmEngine e2(offline_cfg(out));
    auto r2 = e2.complete({{"user", "hi"}}, 5.0);
    CHECK(r2.error.empty());
    CHECK(r2.text == "fine");
    CHECK(ok.chats() == 1);
}
#ifndef _WIN32  // setenv
TEST_CASE("agent: Fuzz4All uses the run's model settings, not built-in defaults") {
    auto td = agent_tmp("fuse-cfg");
    std::ofstream(td / "f.c") << "int f(int x) {\n    return x + 1;\n}\n";
    auto fns = prism::extract_functions(td / "f.c", "f.c");
    REQUIRE(!fns.empty());
    auto has_skip = [](const std::vector<prism::Finding>& fs) {
        for (const auto& f : fs)
            if (f.message.find("stall mutants / autoprompt skipped") != std::string::npos) return true;
        return false;
    };
    // The run's Config names a real (tiny) FORGE model; the defaults name none.
    auto cfg = offline_cfg(td / "out");
    cfg.fpk = prism_test::tiny_fpk();
    ::setenv("PRISM_FPK", (td / "no_such_model.fpk").c_str(), 1);
    CHECK_FALSE(has_skip(prism::run_fuse(fns, {}, td, 0.05, 2, true, cfg)));
    CHECK(has_skip(prism::run_fuse(fns, {}, td, 0.05, 2, true)));
    ::unsetenv("PRISM_FPK");
}
#endif
