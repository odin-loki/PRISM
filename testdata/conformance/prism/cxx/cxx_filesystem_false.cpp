// PRISM conformance task cxx/cxx_filesystem_false.cpp: expected false (memsafety)
// filename of a/b has size 1; size + 1 is past a two-char buffer
#include <filesystem>
#include <string>
int cxx_filesystem_false(int n) {
    const std::filesystem::path p("a/b");
    const std::string f = p.filename().string();
    char buf[2] = {'x', 'y'};
    if (n < 0 || n > 1) return 0;
    return buf[f.size() + static_cast<std::size_t>(n)];
}
