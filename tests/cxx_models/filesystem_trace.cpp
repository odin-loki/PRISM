// Trace of std::filesystem::path decomposition. `#include <filesystem>` is
// PRISM's model when its directory is first on the include path, and
// libstdc++ otherwise. These comparisons are the same either way.
// exists, file_size, status, is_regular_file, is_directory, current_path,
// and absolute are not called: the model does not declare them.
// is_absolute and is_relative read one stored character. An empty path is
// relative. A leading '/' is absolute. ".." is not removed.
// has_root_directory reads one stored character. An empty path has no root
// directory. A leading '/' has one. ".." is not removed.
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
    const int cmp_empty = empty.compare(empty) == 0;
    const int cmp_empty_a = empty.compare(std::filesystem::path("a")) < 0;
    const int cmp_a_empty = std::filesystem::path("a").compare(empty) > 0;
    const int cmp_aa = std::filesystem::path("a").compare(std::filesystem::path("a")) == 0;
    const int cmp_ab = std::filesystem::path("a").compare(std::filesystem::path("b")) < 0;
    const int cmp_ba = std::filesystem::path("b").compare(std::filesystem::path("a")) > 0;
    const int cmp_prefix = std::filesystem::path("a").compare(std::filesystem::path("ab")) < 0;
    const int cmp_longer = std::filesystem::path("ab").compare(std::filesystem::path("a")) > 0;
    const int cmp_dotdot = std::filesystem::path("a/..").compare(std::filesystem::path("a/..")) == 0;
    const int cmp_keep = std::filesystem::path("a/..").compare(std::filesystem::path(".")) != 0;
    const int cmp_dots = std::filesystem::path("..").compare(std::filesystem::path("..")) == 0;
    std::filesystem::path re_md("a/b.txt");
    const int repl_md = eq(re_md.replace_extension(".md"), "a/b.md");
    std::filesystem::path re_clear("a/b.txt");
    const int repl_clear = eq(re_clear.replace_extension(), "a/b");
    std::filesystem::path re_nodot("a/b.txt");
    const int repl_nodot = eq(re_nodot.replace_extension("md"), "a/b.md");
    std::filesystem::path re_dots("..");
    const int repl_dots = eq(re_dots.replace_extension(), "..");
    std::filesystem::path re_dots_md("..");
    const int repl_dots_md = eq(re_dots_md.replace_extension(".md"), "...md");
    std::filesystem::path re_keep("a/b.txt");
    const int repl_keep_dots = eq(re_keep.replace_extension(".."), "a/b..");
    // Empty replacement on an empty path stays empty here and in libstdc++.
    // A non-empty replacement on an empty path stays empty in this model.
    // libstdc++ stores that replacement, so this trace does not compare it.
    std::filesystem::path re_empty_path;
    const int repl_empty_path = re_empty_path.replace_extension().empty();
    std::filesystem::path re_multi("a/b.tar.txt");
    const int repl_multi = eq(re_multi.replace_extension(".md"), "a/b.tar.md");
    std::filesystem::path re_hidden(".txt");
    const int repl_hidden = eq(re_hidden.replace_extension(".md"), ".txt.md");
    const int abs_empty = empty.is_absolute();
    const int rel_empty = empty.is_relative();
    const int abs_root = std::filesystem::path("/").is_absolute();
    const int rel_root = std::filesystem::path("/").is_relative();
    const int abs_slash = std::filesystem::path("/a").is_absolute();
    const int rel_slash = std::filesystem::path("/a").is_relative();
    const int abs_rel = std::filesystem::path("a").is_absolute();
    const int rel_rel = std::filesystem::path("a").is_relative();
    const int abs_nested = std::filesystem::path("a/b").is_absolute();
    const int rel_nested = std::filesystem::path("a/b").is_relative();
    const int abs_dotdot = std::filesystem::path("a/../b").is_absolute();
    const int rel_dotdot = std::filesystem::path("a/../b").is_relative();
    const int abs_root_dotdot = std::filesystem::path("/a/../b").is_absolute();
    const int rel_root_dotdot = std::filesystem::path("/a/../b").is_relative();
    const int abs_blank = std::filesystem::path("").is_absolute();
    const int rel_blank = std::filesystem::path("").is_relative();
    const int abs_drive = std::filesystem::path("C:/a").is_absolute();
    const int rel_drive = std::filesystem::path("C:/a").is_relative();
    const int abs_bslash = std::filesystem::path("\\a").is_absolute();
    const int rel_bslash = std::filesystem::path("\\a").is_relative();
    const int abs_unc = std::filesystem::path("//net/share").is_absolute();
    const int rel_unc = std::filesystem::path("//net/share").is_relative();
    const int root_empty = empty.has_root_directory();
    const int root_root = std::filesystem::path("/").has_root_directory();
    const int root_slash = std::filesystem::path("/a").has_root_directory();
    const int root_rel = std::filesystem::path("a").has_root_directory();
    const int root_nested = std::filesystem::path("a/b").has_root_directory();
    const int root_dotdot = std::filesystem::path("a/../b").has_root_directory();
    const int root_root_dotdot = std::filesystem::path("/a/../b").has_root_directory();
    const int root_blank = std::filesystem::path("").has_root_directory();
    const int root_drive = std::filesystem::path("C:/a").has_root_directory();
    const int root_bslash = std::filesystem::path("\\a").has_root_directory();
    const int root_unc = std::filesystem::path("//net/share").has_root_directory();
    const int root_net = std::filesystem::path("//net").has_root_directory();
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
    std::printf("cmp_empty=%d\n", cmp_empty);
    std::printf("cmp_empty_a=%d\n", cmp_empty_a);
    std::printf("cmp_a_empty=%d\n", cmp_a_empty);
    std::printf("cmp_aa=%d\n", cmp_aa);
    std::printf("cmp_ab=%d\n", cmp_ab);
    std::printf("cmp_ba=%d\n", cmp_ba);
    std::printf("cmp_prefix=%d\n", cmp_prefix);
    std::printf("cmp_longer=%d\n", cmp_longer);
    std::printf("cmp_dotdot=%d\n", cmp_dotdot);
    std::printf("cmp_keep=%d\n", cmp_keep);
    std::printf("cmp_dots=%d\n", cmp_dots);
    std::printf("repl_md=%d\n", repl_md);
    std::printf("repl_clear=%d\n", repl_clear);
    std::printf("repl_nodot=%d\n", repl_nodot);
    std::printf("repl_dots=%d\n", repl_dots);
    std::printf("repl_dots_md=%d\n", repl_dots_md);
    std::printf("repl_keep_dots=%d\n", repl_keep_dots);
    std::printf("repl_empty_path=%d\n", repl_empty_path);
    std::printf("repl_multi=%d\n", repl_multi);
    std::printf("repl_hidden=%d\n", repl_hidden);
    std::printf("abs_empty=%d\n", abs_empty);
    std::printf("rel_empty=%d\n", rel_empty);
    std::printf("abs_root=%d\n", abs_root);
    std::printf("rel_root=%d\n", rel_root);
    std::printf("abs_slash=%d\n", abs_slash);
    std::printf("rel_slash=%d\n", rel_slash);
    std::printf("abs_rel=%d\n", abs_rel);
    std::printf("rel_rel=%d\n", rel_rel);
    std::printf("abs_nested=%d\n", abs_nested);
    std::printf("rel_nested=%d\n", rel_nested);
    std::printf("abs_dotdot=%d\n", abs_dotdot);
    std::printf("rel_dotdot=%d\n", rel_dotdot);
    std::printf("abs_root_dotdot=%d\n", abs_root_dotdot);
    std::printf("rel_root_dotdot=%d\n", rel_root_dotdot);
    std::printf("abs_blank=%d\n", abs_blank);
    std::printf("rel_blank=%d\n", rel_blank);
    std::printf("abs_drive=%d\n", abs_drive);
    std::printf("rel_drive=%d\n", rel_drive);
    std::printf("abs_bslash=%d\n", abs_bslash);
    std::printf("rel_bslash=%d\n", rel_bslash);
    std::printf("abs_unc=%d\n", abs_unc);
    std::printf("rel_unc=%d\n", rel_unc);
    std::printf("root_empty=%d\n", root_empty);
    std::printf("root_root=%d\n", root_root);
    std::printf("root_slash=%d\n", root_slash);
    std::printf("root_rel=%d\n", root_rel);
    std::printf("root_nested=%d\n", root_nested);
    std::printf("root_dotdot=%d\n", root_dotdot);
    std::printf("root_root_dotdot=%d\n", root_root_dotdot);
    std::printf("root_blank=%d\n", root_blank);
    std::printf("root_drive=%d\n", root_drive);
    std::printf("root_bslash=%d\n", root_bslash);
    std::printf("root_unc=%d\n", root_unc);
    std::printf("root_net=%d\n", root_net);
    if (!empty_fn_empty || empty_fn_dot || !fn_b || !ext || !parent || !join || !dotdot || !keep || !stem_empty ||
        stem_empty_dot || !stem_txt || !stem_b || !stem_dot || !stem_dotdot || !stem_hidden || !stem_keep ||
        !stem_multi || !cmp_empty || !cmp_empty_a || !cmp_a_empty || !cmp_aa || !cmp_ab || !cmp_ba || !cmp_prefix ||
        !cmp_longer || !cmp_dotdot || !cmp_keep || !cmp_dots || !repl_md || !repl_clear || !repl_nodot || !repl_dots ||
        !repl_dots_md || !repl_keep_dots || !repl_empty_path || !repl_multi || !repl_hidden || abs_empty ||
        !rel_empty || !abs_root || rel_root || !abs_slash || rel_slash || abs_rel || !rel_rel || abs_nested ||
        !rel_nested || abs_dotdot || !rel_dotdot || !abs_root_dotdot || rel_root_dotdot || abs_blank ||
        !rel_blank || abs_drive || !rel_drive || abs_bslash || !rel_bslash || !abs_unc || rel_unc || root_empty ||
        !root_root || !root_slash || root_rel || root_nested || root_dotdot || !root_root_dotdot || root_blank ||
        root_drive || root_bslash || !root_unc || !root_net)
        return 1;
    return 0;
}
