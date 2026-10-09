// ParanoidBSD bridge honesty (was tests/test_pbsd.py). Portable lints and
// missing-tree behaviour also live in tests/cpp/test_main.cpp.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/checkers.hpp"
#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"
#include "prism/stages.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace {

namespace fs = std::filesystem;
namespace laws = prism::laws;

fs::path td() { return fs::path(PRISM_SOURCE_DIR) / "testdata"; }

fs::path missing_root() { return fs::current_path().root_path() / "prism-no-such-paranoidbsd"; }

struct PbsdEnv {
    std::string was;
    bool had = false;
    PbsdEnv() {
#ifdef _WIN32
        if (const char* v = std::getenv("PRISM_PBSD")) {
            had = true;
            was = v;
        }
        _putenv_s("PRISM_PBSD", "");
#else
        if (const char* v = std::getenv("PRISM_PBSD")) {
            had = true;
            was = v;
            unsetenv("PRISM_PBSD");
        }
#endif
    }
    ~PbsdEnv() {
#ifdef _WIN32
        _putenv_s("PRISM_PBSD", had ? was.c_str() : "");
#else
        if (had) setenv("PRISM_PBSD", was.c_str(), 1);
        else unsetenv("PRISM_PBSD");
#endif
    }
};

void never_proof(const std::vector<prism::Finding>& hits) {
    for (auto& f : hits) {
        CHECK(f.status != laws::CLEAN);
        CHECK(f.status != laws::PROVED);
        CHECK(f.status != laws::PROVED_UNBOUNDED);
        CHECK(f.status != laws::PROVED_ASSUMING);
        CHECK_FALSE(laws::is_proof(f.status));
    }
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

fs::path tmpdir(const char* tag) {
    static int n = 0;
    auto d = fs::temp_directory_path() / (std::string("prism_pbsd_") + tag + "_" +
                                          std::to_string(std::random_device{}()) + "_" + std::to_string(n++));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

}  // namespace

TEST_CASE("pbsd: adapters.cpp never emits CLEAN or PROVED") {
    const auto cpp = slurp(fs::path(PRISM_SOURCE_DIR) / "src" / "prism" / "adapters.cpp");
    CHECK(cpp.find("prism.portable") != std::string::npos);
    CHECK(cpp.find("MEM-ONESIDED-INDEX") != std::string::npos);
    CHECK(cpp.find("missing-bin") != std::string::npos);
    CHECK(cpp.find("not a clean sweep") != std::string::npos);
    CHECK(cpp.find("laws::NOTRUN") != std::string::npos);
    CHECK(cpp.find("Do not wrap sweep_all.py as CLEAN") != std::string::npos);
    CHECK(cpp.find("laws::CLEAN") == std::string::npos);
    CHECK(cpp.find("laws::PROVED") == std::string::npos);
    CHECK(cpp.find("PROVED-UNBOUNDED") == std::string::npos);
}

TEST_CASE("pbsd: onesided and capacity portable lints still fire") {
    auto onesided = prism::run_lints({td() / "onesided.c"}, td());
    bool hit = false;
    for (auto& f : onesided)
        if (f.cls == "MEM-ONESIDED-INDEX") {
            hit = true;
            CHECK(f.function == "onesided_index");
        }
    CHECK(hit);
    auto cap = prism::run_lints({td() / "capacity.c"}, td());
    CHECK(std::any_of(cap.begin(), cap.end(), [](auto& f) { return f.cls == "MEM-CAPACITY-FIRST"; }));
}

TEST_CASE("pbsd: missing configured tree is NOTRUN with install hint") {
    PbsdEnv env;
    prism::Config cfg = prism::default_config();
    cfg.root = td();
    cfg.pbsd_root = missing_root();
    auto hits = prism::run_pbsd_lints({td() / "abs_ok.c"}, cfg);
    never_proof(hits);
    CHECK(std::any_of(hits.begin(), hits.end(), [](auto& f) { return f.status == laws::NOTRUN; }));
}

TEST_CASE("pbsd: tree without --allow-exec is held; portable lints still run") {
    PbsdEnv env;
    auto root = tmpdir("held");
    fs::create_directories(root / "tools" / "verify");
    prism::Config cfg = prism::default_config();
    cfg.root = td();
    cfg.pbsd_root = root;
    auto hits = prism::run_pbsd_lints({td() / "onesided.c"}, cfg);
    never_proof(hits);
    bool held = false, portable = false;
    for (auto& f : hits) {
        if (f.extra.count("reason") && f.extra.at("reason") == "executes-scanned-code") held = true;
        if (f.cls == "MEM-ONESIDED-INDEX" && f.status == laws::FAILED) portable = true;
    }
    CHECK(held);
    CHECK(portable);
}

TEST_CASE("pbsd: empty scope is never CLEAN-as-proof") {
    PbsdEnv env;
    prism::Config cfg = prism::default_config();
    cfg.root = td();
    cfg.pbsd_root = missing_root();
    auto hits = prism::run_pbsd_lints({}, cfg);
    never_proof(hits);
    for (auto& f : hits) CHECK(f.status != laws::CLEAN);
}

TEST_CASE("pbsd: exec stage table lists pbsd as part") {
    CHECK(prism::exec_stages().at("pbsd") == "part");
}
