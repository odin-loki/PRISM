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
    std::printf("empty_fn_empty=%d\n", empty_fn_empty);
    std::printf("empty_fn_dot=%d\n", empty_fn_dot);
    std::printf("fn_b=%d\n", fn_b);
    std::printf("ext=%d\n", ext);
    std::printf("parent=%d\n", parent);
    std::printf("join=%d\n", join);
    std::printf("dotdot=%d\n", dotdot);
    std::printf("keep=%d\n", keep);
    if (!empty_fn_empty || empty_fn_dot || !fn_b || !ext || !parent || !join || !dotdot || !keep) return 1;
    return 0;
}
