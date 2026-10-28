// PRISM conformance task cxx/cxx_filesystem_true.cpp: expected true (memsafety)
// filename of a/b is b. filename of an empty path is empty, not a dot.
// stem of a/b.txt is b. stem of an empty path is empty, not a dot.
// stem of .txt is .txt. The path owns the characters; the destructor frees
// that copy.
#include <filesystem>
int cxx_filesystem_true(int) {
    const std::filesystem::path p("a/b");
    const std::filesystem::path e;
    if (!(p.filename() == "b")) return 1;
    if (!e.empty()) return 1;
    if (!e.filename().empty()) return 1;
    if (e.filename() == ".") return 1;
    if (!(std::filesystem::path("a/b.txt").stem() == "b")) return 1;
    if (!(std::filesystem::path("a/b").stem() == "b")) return 1;
    if (!e.stem().empty()) return 1;
    if (e.stem() == ".") return 1;
    if (!(std::filesystem::path(".").stem() == ".")) return 1;
    if (!(std::filesystem::path("..").stem() == "..")) return 1;
    if (!(std::filesystem::path(".txt").stem() == ".txt")) return 1;
    return 0;
}
