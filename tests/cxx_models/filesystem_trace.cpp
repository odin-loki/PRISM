// Trace of std::filesystem::path decomposition. `#include <filesystem>` is
// PRISM's model when its directory is first on the include path, and
// libstdc++ otherwise. These comparisons are the same either way.
// exists, file_size, status, is_regular_file, is_directory, current_path,
// and absolute are not called: the model does not declare them.
#include <filesystem>
#include <cstdio>
#include <cstdlib>

// Host stand-ins for the PIR intrinsics the pointer constructor calls.
// libstdc++ does not reference them. A failed check aborts.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "check %s: %s\n", cls ? cls : "", msg ? msg : "");
        std::abort();
    }
}

extern "C" unsigned long __prism_obj_size(const void*) { return 1ul << 20; }

static int eq(const std::filesystem::path& p, const char* s) { return p == s; }

int main() {
    const std::filesystem::path empty;
    const int empty_fn_empty = empty.filename().empty();
    const int empty_fn_dot = eq(empty.filename(), ".");
    const int fn_b = eq(std::filesystem::path("a/b").filename(), "b");
    const int ext = eq(std::filesystem::path("a/b.txt").extension(), ".txt");
    const int parent = eq(std::filesystem::path("a/b").parent_path(), "a");
    const int join = eq(std::filesystem::path("a") / std::filesystem::path("b"), "a/b");
    const int dotdot = eq(std::filesystem::path("a") / std::filesystem::path(".."), "a/..");
    const int keep = eq(std::filesystem::path("a/..").filename(), "..");
    const int stem_empty = empty.stem().empty();
    const int stem_empty_dot = eq(empty.stem(), ".");
    const int stem_txt = eq(std::filesystem::path("a/b.txt").stem(), "b");
    const int stem_b = eq(std::filesystem::path("a/b").stem(), "b");
    const int stem_dot = eq(std::filesystem::path(".").stem(), ".");
    const int stem_dotdot = eq(std::filesystem::path("..").stem(), "..");
    const int stem_hidden = eq(std::filesystem::path(".txt").stem(), ".txt");
    const int stem_keep = eq(std::filesystem::path("a/..").stem(), "..");
    const int stem_multi = eq(std::filesystem::path("a/b.tar.txt").stem(), "b.tar");
    std::printf("empty_fn_empty=%d\n", empty_fn_empty);
    std::printf("empty_fn_dot=%d\n", empty_fn_dot);
    std::printf("fn_b=%d\n", fn_b);
    std::printf("ext=%d\n", ext);
    std::printf("parent=%d\n", parent);
    std::printf("join=%d\n", join);
    std::printf("dotdot=%d\n", dotdot);
    std::printf("keep=%d\n", keep);
    std::printf("stem_empty=%d\n", stem_empty);
    std::printf("stem_empty_dot=%d\n", stem_empty_dot);
    std::printf("stem_txt=%d\n", stem_txt);
    std::printf("stem_b=%d\n", stem_b);
    std::printf("stem_dot=%d\n", stem_dot);
    std::printf("stem_dotdot=%d\n", stem_dotdot);
    std::printf("stem_hidden=%d\n", stem_hidden);
    std::printf("stem_keep=%d\n", stem_keep);
    std::printf("stem_multi=%d\n", stem_multi);
    if (!empty_fn_empty || empty_fn_dot || !fn_b || !ext || !parent || !join || !dotdot || !keep || !stem_empty ||
        stem_empty_dot || !stem_txt || !stem_b || !stem_dot || !stem_dotdot || !stem_hidden || !stem_keep ||
        !stem_multi)
        return 1;
    return 0;
}
