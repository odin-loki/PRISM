// Differential tests of PRISM C++ library model headers (roadmap 2.6).
// Port of tests/test_cxx_models.py; data stays in tests/cxx_models/.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct CxxModelsCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

CxxModelsCtx& ctx() {
    static CxxModelsCtx c;
    return c;
}

bool run_cmd(const std::vector<std::string>& argv, std::string* out = nullptr, std::string* err = nullptr) {
#ifndef _WIN32
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    int pipe_out[2]{-1, -1};
    if (out && pipe(pipe_out) != 0) return false;
    pid_t pid = 0;
    if (posix_spawnp(&pid, argv[0].c_str(), nullptr, nullptr, args.data(), environ) != 0) {
        if (out) {
            close(pipe_out[0]);
            close(pipe_out[1]);
        }
        return false;
    }
    if (out) {
        close(pipe_out[1]);
        out->clear();
        char buf[8192];
        ssize_t n = 0;
        while ((n = ::read(pipe_out[0], buf, sizeof buf)) > 0) out->append(buf, static_cast<size_t>(n));
        close(pipe_out[0]);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return false;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
#else
    return false;
#endif
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    auto probe = tmp / "probe.cpp";
    std::ofstream(probe) << "#include <vector>\nint main() { std::vector<int> v{1}; return v[0] - 1; }\n";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> build{cxx};
    build.insert(build.end(), flags.begin(), flags.end());
    build.push_back(probe.string());
    build.push_back("-o");
    build.push_back((tmp / "probe").string());
    if (!run_cmd(build)) return false;
    return run_cmd({(tmp / "probe").string()});
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, const std::string& prog, bool with_models) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    fs::path out = tmp / (prog + (with_models ? ".model" : ".lib"));
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / (prog + ".cpp")).string());
    argv.push_back("-o");
    argv.push_back(out.string());
    REQUIRE(run_cmd(argv));
    return out;
}

std::vector<std::string> run_lines(const fs::path& exe, const std::string& arg = {}) {
    std::vector<std::string> argv{exe.string()};
    if (!arg.empty()) argv.push_back(arg);
    std::string stdout_text;
    REQUIRE(run_cmd(argv, &stdout_text));
    std::vector<std::string> lines;
    std::istringstream in(stdout_text);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
}

std::string headers(const std::string& cxx, const std::string& prog) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    argv.push_back("-isystem");
    argv.push_back(models.string());
    argv.push_back("-M");
    argv.push_back((repo() / "tests" / "cxx_models" / (prog + ".cpp")).string());
    std::string out;
    REQUIRE(run_cmd(argv, &out));
    return out;
}

void same_trace(const std::string& prog, const std::vector<const char*>& model_headers) {
    auto& c = ctx();
    REQUIRE(c.ok);
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    auto deps = headers(c.cxx, prog);
    for (const char* m : model_headers) CHECK(deps.find((models / m).string()) != std::string::npos);
    auto lib = build_prog(c.cxx, c.tmp, prog, false);
    auto mod = build_prog(c.cxx, c.tmp, prog, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a.size() > 20);
    CHECK(a == b);
}

}  // namespace

TEST_CASE("cxx models: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxmodels-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        for (const char* name : {"clang++-18", "clang++"}) {
            if (run_cmd({name, "--version"})) {
                c.cxx = name;
                break;
            }
        }
        if (!c.cxx.empty()) c.ok = probe_clang_asan(c.cxx, c.tmp);
    }
    if (!c.ok) MESSAGE("NOTRUN: clang++ with ASan/UBSan not usable here");
}

TEST_CASE("cxx models: vector trace matches libstdc++") {
    if (!ctx().ok) return;
    same_trace("vector_trace", {"vector"});
}

TEST_CASE("cxx models: string trace matches libstdc++") {
    if (!ctx().ok) return;
    same_trace("string_trace", {"bits/basic_string.tcc"});
}

#ifndef _WIN32
int run_capture(const fs::path& exe, const std::string& arg, std::string* combined) {
    std::string cmd = exe.string() + " " + arg + " 2>&1";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return -1;
    combined->clear();
    char buf[4096];
    while (fgets(buf, sizeof buf, fp)) *combined += buf;
    return pclose(fp);
}
#endif

TEST_CASE("cxx models: map/set trace matches libstdc++ and UAF on erase") {
    if (!ctx().ok) return;
    same_trace("map_set_trace", {"map", "set", "prism_tree.h"});
#ifndef _WIN32
    auto& c = ctx();
    for (bool models : {false, true}) {
        auto exe = c.tmp / ("map_set_trace" + std::string(models ? ".model" : ".lib"));
        std::string out;
        int rc = run_capture(exe, "uaf", &out);
        CHECK(rc != 0);
        CHECK(out.find("heap-use-after-free") != std::string::npos);
    }
#endif
}
