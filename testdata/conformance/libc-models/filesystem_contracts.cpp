// std::filesystem::path operational model (src/prism/pir/models/cxx/filesystem).
// The pir stage puts the model headers first on the include path, so
// <filesystem> below is the model. Each function is one contract.
// exists, file_size, status, is_regular_file, is_directory, current_path,
// and absolute are not part of the model. There is no false harness that
// calls exists: a stub that returns true would be a fake file.
#include <cassert>
#include <filesystem>

// filename of a/b is b. filename of an empty path is empty, not a dot.
int fs_filename_true(int) {
    const std::filesystem::path p("a/b");
    const std::filesystem::path e;
    assert(p.filename() == "b");
    assert(e.empty());
    assert(e.filename().empty());
    assert(!(e.filename() == "."));
    return 0;
}

// extension, parent, and join keep the characters that were written.
// ".." stays "..". The join inserts one slash and does not drop a component.
int fs_join_true(int) {
    assert(std::filesystem::path("a/b.txt").extension() == ".txt");
    assert(std::filesystem::path("a/b").parent_path() == "a");
    assert((std::filesystem::path("a") / std::filesystem::path("b")) == "a/b");
    assert((std::filesystem::path("a") / std::filesystem::path("..")) == "a/..");
    assert(std::filesystem::path("a/..").filename() == "..");
    return 0;
}
