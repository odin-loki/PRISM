// Lock for prism::svcomp::replay when exec is off
// (src/prism/svcomp/replay.cpp). allow_exec false answers replay=notrun
// with a why that names --allow-exec, before any compiler lookup and
// before the ILP32 -m32 branch. The JSON text must not carry a false
// verdict. docs/finish-notes/replay.md

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/svcomp.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#ifdef _WIN32
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
using prism::svcomp::json;
using prism::svcomp::replay;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
#ifdef _WIN32
        const long pid = static_cast<long>(_getpid());
#else
        const long pid = static_cast<long>(::getpid());
#endif
        path = fs::temp_directory_path() /
               ("prism_svcomp_replay_" + std::to_string(pid) + "_" + std::to_string(n++));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path write(const std::string& name, const std::string& text) const {
        std::ofstream(path / name, std::ios::binary) << text;
        return path / name;
    }
};

// SV-COMP false answers are `false` or `false(<prop>)`. Either one, and a
// replay status spelled false, shows up in the JSON text as those letters.
void expect_notrun(const json& rec) {
    REQUIRE(rec.at("replay").is_string());
    CHECK(rec.at("replay").get<std::string>() == "notrun");
    const std::string text = rec.dump();
    CHECK(text.find("false") == std::string::npos);
    REQUIRE(rec.at("why").is_string());
    CHECK(rec.at("why").get<std::string>().find("--allow-exec") != std::string::npos);
}

}  // namespace

TEST_CASE("svcomp replay: allow_exec false is notrun and not a false verdict") {
    TempDir d;
    const auto src = d.write("t.c", "int main(void) { return 0; }\n");

    const auto lp64 = replay(src, "no-overflow", false, std::nullopt, d.path / "lp64", "LP64");
    expect_notrun(lp64);

    // ILP32 must not skip the exec gate and compile with -m32.
    const auto ilp32 = replay(src, "no-overflow", false, std::nullopt, d.path / "ilp32", "ILP32");
    expect_notrun(ilp32);
}
