// Stand-in `prism` CLI for the window doctests (tests/cpp/test_gui.cpp,
// WIN32 only; POSIX uses a /bin/sh script). It does what the script does:
//
//   fake_gui_prism PATH --out OUT ...   makes OUT, copies the canned report to
//       OUT/report.json, prints "args: <argv...>" and exits with the code in
//       the sidecar.
//
// Sidecars next to the executable (the test copies this exe per case):
//   NAME.exe.report   one line: path of the canned report.json
//   NAME.exe.code     one line: the exit code (default 0)

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

std::string first_line(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string line;
    std::getline(in, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

}  // namespace

int main(int argc, char** argv) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return 2;
    const std::filesystem::path self(std::wstring(buf, n));
    const auto report = first_line(self.wstring() + L".report");
    const auto code_text = first_line(self.wstring() + L".code");
    // argv: [1] path, [2] --out, [3] OUT (the same $3 the script reads)
    if (argc < 4) return 2;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(argv[3]), ec);
    if (!report.empty()) {
        fs::copy_file(fs::path(report), fs::path(argv[3]) / "report.json",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) return 2;
    }
    std::cout << "args:";
    for (int i = 1; i < argc; ++i) std::cout << ' ' << argv[i];
    std::cout << std::endl;
    return code_text.empty() ? 0 : std::atoi(code_text.c_str());
}
