// Tiny PE stub for adapter doctests on WIN32: runs the sibling .cmd with the
// same basename and forwards argv (see tests/cpp/test_adapters.cpp fake()).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::wstring quote(const std::wstring& a) {
    if (a.empty()) return L"\"\"";
    if (a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring o = L"\"";
    for (wchar_t c : a) {
        if (c == L'"') o += L'\\';
        o += c;
    }
    return o + L"\"";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (!n || n >= MAX_PATH) return 1;
    auto script = std::filesystem::path(self);
    script.replace_extension(L".cmd");
    if (!std::filesystem::exists(script)) return 1;
    std::wstringstream cl;
    cl << L"cmd.exe /d /c " << quote(script.wstring());
    for (int i = 1; i < argc; ++i) cl << L' ' << quote(argv[i]);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cl.str();
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi))
        return 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}
