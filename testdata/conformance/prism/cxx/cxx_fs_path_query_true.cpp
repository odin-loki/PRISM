#include <filesystem>
int cxx_fs_path_query_true(int x) {
    std::filesystem::path p("/a/b.txt");
    p.replace_filename("c");
    if (p.is_absolute() && !p.has_extension() && x < 100) return x + 1;
    return 0;
}
