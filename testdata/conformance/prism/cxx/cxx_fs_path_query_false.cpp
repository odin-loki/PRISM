// PRISM conformance task cxx/cxx_fs_path_query_false.cpp: expected false (no-overflow)
// the path query holds for every x, so x + 1 is reached unbounded
#include <filesystem>
int cxx_fs_path_query_false(int x) {
    std::filesystem::path p("/a/b.txt");
    p.replace_filename("c");
    if (p.is_absolute() && !p.has_extension()) return x + 1;
    return 0;
}
