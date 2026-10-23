// Stand-in `prism_gui` for the `prism --gui` launcher doctests
// (tests/cpp/test_gui_model.cpp, WIN32 only; POSIX uses a /bin/sh script).
//
// Records its argv (without argv[0]) as "ARG|" per argument in
// <this exe>.args and exits 7. A file, not stdout: the launcher starts it with
// CreateProcess and does not hand over its own stdout. The test copies this
// executable to NAME/prism_gui.exe beside a copy of prism.exe.

#include <fstream>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

int main(int argc, char** argv) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return 2;
    std::wstring self(buf, n);
    std::ofstream out(self + L".args", std::ios::binary);
    if (!out) return 2;
    for (int i = 1; i < argc; ++i) out << argv[i] << '|';
    return 7;
}
