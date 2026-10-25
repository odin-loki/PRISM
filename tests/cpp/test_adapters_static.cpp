// Adapter/config checks that need no fake shell tools (WIN32 subset).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/stages.hpp"

#include "../../src/prism/adapters_internal.hpp"
#include "../../src/prism/stages/llm.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
namespace laws = prism::laws;
namespace ad = prism::adapters_detail;
namespace sd = prism::stages_detail;

namespace {

fs::path repo_root() { return fs::path(PRISM_SOURCE_DIR); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

fs::path testdata() { return repo_root() / "testdata" / "planted"; }

std::string xget(const prism::Finding& f, const std::string& key) {
    auto it = f.extra.find(key);
    return it == f.extra.end() ? std::string() : it->second;
}

}  // namespace

TEST_CASE("codeql: adapter removed; MANIFEST records why; no optional-tool row") {
    auto manifest = slurp(repo_root() / "third_party" / "MANIFEST.toml");
    CHECK(has(manifest, "name = \"codeql\""));
    CHECK(has(manifest, "restricted commercial use"));
    CHECK(has(manifest, "adapter was removed"));
    for (const char* file : {"adapters.cpp", "config.cpp", "taxonomy.cpp", "polyglot.cpp"}) {
        auto raw = slurp(repo_root() / "src" / "prism" / file);
        std::string code;
        for (std::size_t i = 0; i < raw.size();) {
            if (raw[i] == '/' && i + 1 < raw.size() && raw[i + 1] == '/') {
                while (i < raw.size() && raw[i] != '\n') ++i;
                continue;
            }
            code.push_back(raw[i++]);
        }
        CHECK(lower(code).find("codeql") == std::string::npos);
    }
    auto out = prism::run_optional_tools({}, prism::default_config());
    for (const auto& f : out) CHECK(f.stage != "codeql");
}

TEST_CASE("harness_for_parsefail maps what the BMC cannot model and nothing else") {
    auto t = prism::harness_for_parsefail("throw unencoded", "bitvector BMC");
    REQUIRE(t.has_value());
    CHECK(has(*t, "exception model"));
    auto u = prism::harness_for_parsefail("UNENCODED: bit-field", "bitvector BMC");
    REQUIRE(u.has_value());
    CHECK(has(*u, "not a proof"));
    CHECK_FALSE(prism::harness_for_parsefail("unexpected token ')'", "bitvector BMC").has_value());
}

TEST_CASE("config: every pinned external stage has a prism-deps hint (bitwuzla too)") {
    std::ifstream in(repo_root() / "third_party" / "MANIFEST.toml");
    std::string line, name, kind;
    int checked = 0;
    while (std::getline(in, line)) {
        if (line.ends_with('\r')) line.pop_back();
        if (line.starts_with("[[")) name.clear(), kind.clear();
        auto val = [&](const char* key) -> std::optional<std::string> {
            std::string k = std::string(key) + " = ";
            if (!line.starts_with(k)) return std::nullopt;
            return line.substr(k.size());
        };
        if (auto v = val("name")) name = v->substr(1, v->size() - 2);
        if (auto v = val("kind")) kind = v->substr(1, v->size() - 2);
        auto st = val("stages");
        if (!st || kind != "external") continue;
        std::string rest = *st;
        for (auto q = rest.find('"'); q != std::string::npos; q = rest.find('"', q)) {
            auto e = rest.find('"', q + 1);
            auto stage = rest.substr(q + 1, e - q - 1);
            q = e + 1;
            auto hint = prism::adapter_install(stage);
            CHECK_MESSAGE(has(hint, "prism-deps tool " + name), stage << " -> " << hint);
            CHECK_MESSAGE(prism::pinned_commit(name).has_value(), name);
            ++checked;
        }
    }
    CHECK(checked >= 15);
    CHECK(has(prism::adapter_install("bitwuzla"), "prism-deps tool bitwuzla"));
    auto esbmc = prism::adapter_install("esbmc");
    CHECK(has(esbmc, "prism-deps tool esbmc"));
    CHECK(has(esbmc, "third_party/MANIFEST.toml"));
    CHECK_FALSE(has(esbmc, "SOURCES.md"));
    CHECK_FALSE(has(esbmc, "apt install"));
}

TEST_CASE("adapters: doctest / Catch2 / shell not-found text is a missing tool") {
    CHECK(ad::probe_looks_missing("[doctest] doctest version is \"2.4.11\"\n--stack-trace", 0));
    CHECK(ad::probe_looks_missing("Unknown option: --timeout\n", 1));
    CHECK(ad::probe_looks_missing("", 127));
    CHECK_FALSE(ad::probe_looks_missing("", 0));
    CHECK_FALSE(ad::probe_looks_missing("usage: cbmc [options] file.c", 1));
    CHECK(ad::is_fake_adapter("Catch2 v3.5.0\n"));
    CHECK(ad::is_fake_adapter("[doctest] doctest version is 2.4.11\n"));
    CHECK_FALSE(ad::is_fake_adapter("CBMC 5.95.1\n"));
    CHECK(ad::tool_unusable("Catch2 v3.5.0\n", 0));
    CHECK(ad::tool_unusable("", 126));
    CHECK(ad::tool_unusable("", 127));
    CHECK(ad::tool_unusable("sh: 1: cbmc: not found\n", 1));
    CHECK(ad::tool_unusable("'cbmc' is not recognized as an internal or external command\n", 1));
    CHECK(ad::tool_unusable("cannot execute binary file\n", 126));
    CHECK_FALSE(ad::tool_unusable("VERIFICATION SUCCESSFUL\n", 0));
}

TEST_CASE("cbmc: Law 8 refuses check-disabling flags") {
    CHECK_THROWS_WITH_AS(ad::refuse_disabled_checks({"cbmc", "a.c", "--no-bounds-check"}),
                         "refusing to disable a check: --no-bounds-check", std::runtime_error);
    CHECK_NOTHROW(ad::refuse_disabled_checks({"cbmc", "a.c", "--bounds-check"}));
}

TEST_CASE("llm_httpish: connection errors are a missing backend, not a failed repair") {
    CHECK(sd::llm_httpish("Connection aborted."));
    CHECK(sd::llm_httpish("connection refused"));
    CHECK(sd::llm_httpish("[Errno 111] Connection refused"));
    CHECK(sd::llm_httpish("llama.cpp/Ollama not reachable"));
    CHECK_FALSE(sd::llm_httpish("candidate did not compile"));
    CHECK_FALSE(sd::llm_httpish(""));
}

TEST_CASE("execute_cex replays concretely (not a proof); rlef_repair without a model is NOTRUN") {
    auto src = testdata() / "div_param.c";
    prism::FunctionInfo fn;
    for (auto& f : prism::extract_functions(src, src.string()))
        if (f.name == "div_param") fn = f;
    REQUIRE(fn.name == "div_param");
    prism::Finding fail;
    fail.stage = "bmc";
    fail.status = std::string(laws::FAILED);
    fail.file = fn.file;
    fail.function = fn.name;
    fail.line = fn.line;
    fail.cls = "INT-DIV-ZERO";
    fail.message = "div by zero";
    fail.counterexample = "x=1,y=0";
    auto cfg = prism::default_config();
    cfg.llm = false;
    auto out = prism::execute_cex({fail}, {fn}, cfg);
    REQUIRE_FALSE(out.empty());
    CHECK(xget(out[0], "oracle") == "concrete-replay");
    CHECK((out[0].status == laws::CRASH || out[0].status == laws::CLEAN));
    for (const auto& f : out) CHECK_FALSE(laws::is_proof(f.status));

    cfg.gguf = "/nonexistent/prism-missing.gguf";
    cfg.ollama_host.clear();
    cfg.llama_server = "http://127.0.0.1:1";
    auto rep = prism::rlef_repair(fail, cfg);
    REQUIRE_FALSE(rep.empty());
    CHECK(rep[0].stage == "repair");
    CHECK(rep[0].status == laws::NOTRUN);
    CHECK(has(rep[0].message, sd::LLM_UNAVAILABLE_MSG));
}
