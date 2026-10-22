// Minimal doctest main for the Windows CI subset (see PRISM_TESTS_WIN_SUBSET).
// Full integration tests live in test_main.cpp (Unix / future full Windows port).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace {
struct PrivateTmp {
    std::filesystem::path dir;
    PrivateTmp() {
        std::error_code ec;
        auto base = std::filesystem::temp_directory_path(ec);
        if (ec) return;
#ifdef _WIN32
        auto pid = _getpid();
#else
        auto pid = getpid();
#endif
        dir = base / ("prism_tests." + std::to_string(pid));
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            dir.clear();
            return;
        }
#ifdef _WIN32
        _putenv_s("TMP", dir.string().c_str());
        _putenv_s("TEMP", dir.string().c_str());
#else
        setenv("TMPDIR", dir.c_str(), 1);
#endif
    }
    ~PrivateTmp() {
        if (dir.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    PrivateTmp(const PrivateTmp&) = delete;
    PrivateTmp& operator=(const PrivateTmp&) = delete;
};
const PrivateTmp g_private_tmp;
}  // namespace
