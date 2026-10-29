// no-false-twin: this harness labels positive contracts of a C++ header model only (no planted defect).
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

// stem is the filename without its extension. An empty path stays empty.
// "." and ".." stay themselves. A leading dot with no later dot stays.
// ".." is not dropped.
int fs_stem_true(int) {
    const std::filesystem::path e;
    assert(e.stem().empty());
    assert(!(e.stem() == "."));
    assert(std::filesystem::path("a/b.txt").stem() == "b");
    assert(std::filesystem::path("a/b").stem() == "b");
    assert(std::filesystem::path(".").stem() == ".");
    assert(std::filesystem::path("..").stem() == "..");
    assert(std::filesystem::path(".txt").stem() == ".txt");
    assert(std::filesystem::path("a/..").stem() == "..");
    assert(std::filesystem::path("a/b.tar.txt").stem() == "b.tar");
    return 0;
}

// compare orders the characters the path stored. An empty path compares
// equal to an empty path. ".." stays in that sequence; compare does not
// drop it and does not ask whether a file exists.
int fs_compare_true(int) {
    const std::filesystem::path e;
    assert(e.compare(e) == 0);
    assert(e.compare(std::filesystem::path("a")) < 0);
    assert(std::filesystem::path("a").compare(e) > 0);
    assert(std::filesystem::path("a").compare(std::filesystem::path("a")) == 0);
    assert(std::filesystem::path("a").compare(std::filesystem::path("b")) < 0);
    assert(std::filesystem::path("b").compare(std::filesystem::path("a")) > 0);
    assert(std::filesystem::path("a").compare(std::filesystem::path("ab")) < 0);
    assert(std::filesystem::path("ab").compare(std::filesystem::path("a")) > 0);
    assert(std::filesystem::path("a/..").compare(std::filesystem::path("a/..")) == 0);
    assert(std::filesystem::path("a/..").compare(std::filesystem::path(".")) != 0);
    assert(std::filesystem::path("..").compare(std::filesystem::path("..")) == 0);
    return 0;
}

// replace_extension rewrites stored characters. An empty replacement removes
// a trailing extension and does not leave a dot. A replacement that does not
// start with '.' gains one dot. ".." is a parent directory, not an extension.
int fs_replace_ext_true(int) {
    std::filesystem::path e;
    e.replace_extension();
    assert(e.empty());
    std::filesystem::path md("a/b.txt");
    assert(md.replace_extension(".md") == "a/b.md");
    std::filesystem::path gone("a/b.txt");
    assert(gone.replace_extension() == "a/b");
    std::filesystem::path nodot("a/b.txt");
    assert(nodot.replace_extension("md") == "a/b.md");
    std::filesystem::path dots("..");
    assert(dots.replace_extension() == "..");
    std::filesystem::path dots_md("..");
    assert(dots_md.replace_extension(".md") == "...md");
    std::filesystem::path keep("a/b.txt");
    assert(keep.replace_extension("..") == "a/b..");
    return 0;
}
