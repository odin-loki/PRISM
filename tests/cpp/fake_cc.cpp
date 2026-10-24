// Fake gcc/clang for sanitize doctests on WIN32 (see tests/cpp/test_sanitize.cpp).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string env_c(const char* k) {
    const char* v = std::getenv(k);
    return v ? v : "";
}

static bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool san_allowed(const std::string& san) {
    if (san.empty()) return true;
    const std::string blob = " " + env_c("FAKE_SAN") + " ";
    return blob.find(" " + san + " ") != std::string::npos;
}

static fs::path self_path() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    return fs::path(buf);
}

static int run_from_sidecar(const fs::path& exe) {
    const fs::path body = fs::path(exe.string() + ".fakebody");
    std::error_code ec;
    if (!fs::is_regular_file(body, ec)) return -1;
    std::ifstream in(body);
    int rc = 0;
    in >> rc;
    std::string skip;
    std::getline(in, skip);
    std::string text((std::istreambuf_iterator<char>(in)), {});
    if (!text.empty()) std::fputs(text.c_str(), stderr);
    return rc;
}

static int compile_mode(int argc, char** argv) {
    std::vector<std::string> a;
    for (int i = 1; i < argc; ++i) a.emplace_back(argv[i]);
    if (!a.empty() && a[0] == "-dumpmachine") {
        const auto t = env_c("FAKE_TRIPLE");
        std::printf("%s\n", t.empty() ? "x86_64-linux-gnu" : t.c_str());
        return 0;
    }
    const std::string pfn = "-print-file-name=";
    if (!a.empty() && a[0].rfind(pfn, 0) == 0) {
        std::printf("%s\n", a[0].substr(pfn.size()).c_str());
        return 0;
    }
    std::string out, san;
    bool fire = false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] == "-o" && i + 1 < a.size()) out = a[i + 1];
        if (a[i].rfind("-fsanitize=", 0) == 0) san = a[i].substr(11);
        if (ends_with(a[i], "ub_fire.c") || ends_with(a[i], "as_fire.c")) fire = true;
    }
    if (const auto log = env_c("FAKE_LOG"); !log.empty()) {
        std::ofstream(log, std::ios::app) << "build " << san << " " << (fire ? 1 : 0) << "\n";
    }
    if (!san.empty() && !san_allowed(san)) {
        std::fprintf(stderr, "unsupported -fsanitize=%s\n", san.c_str());
        return 1;
    }
    int rc = 0;
    std::string text;
    if (fire && env_c("FAKE_NOFIRE").empty()) {
        rc = 1;
        if (san == "undefined")
            text = "x.c:3: runtime error: signed integer overflow\n";
        else
            text = "ERROR: AddressSanitizer: heap-buffer-overflow\n";
    } else if (!fire) {  // san empty reads FAKE_RUN_ (the POSIX fake does the same)
        const std::string key = "FAKE_RUN_" + san;
        const std::string body = env_c(key.c_str());
        if (!body.empty()) {
            const auto nl = body.find('\n');
            if (nl != std::string::npos) {
                rc = std::atoi(body.substr(0, nl).c_str());
                text = body.substr(nl + 1);
            }
        }
    }
    if (out.empty()) return 0;
    const auto self = self_path();
    if (self.empty()) return 1;
    std::error_code ec;
    fs::copy_file(self, out, fs::copy_options::overwrite_existing, ec);
    if (ec) return 1;
    std::ofstream(out + ".fakebody") << rc << "\n" << text;
    return 0;
}

int main(int argc, char** argv) {
    const auto self = self_path();
    if (self.empty()) return 1;
    const int run_rc = run_from_sidecar(self);
    if (run_rc >= 0) return run_rc;
    return compile_mode(argc, argv);
}
