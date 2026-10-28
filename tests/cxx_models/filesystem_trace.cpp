// Trace of std::filesystem::path decomposition. `#include <filesystem>` is
// PRISM's model when its directory is first on the include path, and
// libstdc++ otherwise. These comparisons are the same either way.
// exists, file_size, status, is_regular_file, is_directory, current_path,
// and absolute are not called: the model does not declare them.
// is_absolute and is_relative read one stored character. An empty path is
// relative. A leading '/' is absolute. ".." is not removed.
// has_root_directory reads one stored character. An empty path has no root
// directory. A leading '/' has one. ".." is not removed.
// has_filename reads one stored character. An empty path has no filename.
// A trailing '/' has none. Every other non-empty path has one. ".." stays.
// has_extension reads stored characters. An empty path has no extension.
// A trailing '/' has none. ".", "..", a name with no '.', and a name whose
// only '.' is the first character have none. A later '.' is an extension.
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
    const int has_empty = empty.has_filename();
    const int has_blank = std::filesystem::path("").has_filename();
    const int has_root = std::filesystem::path("/").has_filename();
    const int has_slash = std::filesystem::path("/a").has_filename();
    const int has_rel = std::filesystem::path("a").has_filename();
    const int has_nested = std::filesystem::path("a/b").has_filename();
    const int has_trail = std::filesystem::path("a/").has_filename();
    const int has_trail_b = std::filesystem::path("a/b/").has_filename();
    const int has_dot = std::filesystem::path(".").has_filename();
    const int has_dots = std::filesystem::path("..").has_filename();
    const int has_keep = std::filesystem::path("a/..").has_filename();
    const int has_dotdot = std::filesystem::path("a/../b").has_filename();
    const int has_root_dotdot = std::filesystem::path("/a/../b").has_filename();
    const int has_drive = std::filesystem::path("C:/a").has_filename();
    const int has_bslash = std::filesystem::path("\\a").has_filename();
    const int has_unc = std::filesystem::path("//net/share").has_filename();
    const int has_net = std::filesystem::path("//net").has_filename();
    const int has_txt = std::filesystem::path("a/b.txt").has_filename();
    const int has_hidden = std::filesystem::path(".txt").has_filename();
    const int hx_empty = empty.has_extension();
    const int hx_blank = std::filesystem::path("").has_extension();
    const int hx_root = std::filesystem::path("/").has_extension();
    const int hx_trail = std::filesystem::path("a/").has_extension();
    const int hx_trail_b = std::filesystem::path("a/b/").has_extension();
    const int hx_plain = std::filesystem::path("a").has_extension();
    const int hx_nested = std::filesystem::path("a/b").has_extension();
    const int hx_dot = std::filesystem::path(".").has_extension();
    const int hx_dots = std::filesystem::path("..").has_extension();
    const int hx_dot_file = std::filesystem::path("a/.").has_extension();
    const int hx_dot_parent = std::filesystem::path("a/..").has_extension();
    const int hx_hidden = std::filesystem::path(".txt").has_extension();
    const int hx_hidden_dir = std::filesystem::path("a/.txt").has_extension();
    const int hx_txt = std::filesystem::path("a/b.txt").has_extension();
    const int hx_file = std::filesystem::path("b.txt").has_extension();
    const int hx_multi = std::filesystem::path("a/b.tar.txt").has_extension();
    const int hx_later = std::filesystem::path(".txt.md").has_extension();
    const int hx_trailing_dot = std::filesystem::path("a.").has_extension();
    const int hx_three = std::filesystem::path("...").has_extension();
    const int hx_dotdot_file = std::filesystem::path("a/../b").has_extension();
    const int hx_dotdot_txt = std::filesystem::path("a/../b.txt").has_extension();
    const int hx_root_txt = std::filesystem::path("/a/../b.txt").has_extension();
    const int hx_drive = std::filesystem::path("C:/a").has_extension();
    const int hx_drive_txt = std::filesystem::path("C:/a.txt").has_extension();
    const int hx_bslash = std::filesystem::path("\\a").has_extension();
    const int hx_bslash_txt = std::filesystem::path("\\a.txt").has_extension();
    const int hx_unc = std::filesystem::path("//net/share").has_extension();
    const int hx_net = std::filesystem::path("//net").has_extension();
    const int hx_parent_ext = std::filesystem::path("a.b/c").has_extension();
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
    std::printf("has_empty=%d\n", has_empty);
    std::printf("has_blank=%d\n", has_blank);
    std::printf("has_root=%d\n", has_root);
    std::printf("has_slash=%d\n", has_slash);
    std::printf("has_rel=%d\n", has_rel);
    std::printf("has_nested=%d\n", has_nested);
    std::printf("has_trail=%d\n", has_trail);
    std::printf("has_trail_b=%d\n", has_trail_b);
    std::printf("has_dot=%d\n", has_dot);
    std::printf("has_dots=%d\n", has_dots);
    std::printf("has_keep=%d\n", has_keep);
    std::printf("has_dotdot=%d\n", has_dotdot);
    std::printf("has_root_dotdot=%d\n", has_root_dotdot);
    std::printf("has_drive=%d\n", has_drive);
    std::printf("has_bslash=%d\n", has_bslash);
    std::printf("has_unc=%d\n", has_unc);
    std::printf("has_net=%d\n", has_net);
    std::printf("has_txt=%d\n", has_txt);
    std::printf("has_hidden=%d\n", has_hidden);
    std::printf("hx_empty=%d\n", hx_empty);
    std::printf("hx_blank=%d\n", hx_blank);
    std::printf("hx_root=%d\n", hx_root);
    std::printf("hx_trail=%d\n", hx_trail);
    std::printf("hx_trail_b=%d\n", hx_trail_b);
    std::printf("hx_plain=%d\n", hx_plain);
    std::printf("hx_nested=%d\n", hx_nested);
    std::printf("hx_dot=%d\n", hx_dot);
    std::printf("hx_dots=%d\n", hx_dots);
    std::printf("hx_dot_file=%d\n", hx_dot_file);
    std::printf("hx_dot_parent=%d\n", hx_dot_parent);
    std::printf("hx_hidden=%d\n", hx_hidden);
    std::printf("hx_hidden_dir=%d\n", hx_hidden_dir);
    std::printf("hx_txt=%d\n", hx_txt);
    std::printf("hx_file=%d\n", hx_file);
    std::printf("hx_multi=%d\n", hx_multi);
    std::printf("hx_later=%d\n", hx_later);
    std::printf("hx_trailing_dot=%d\n", hx_trailing_dot);
    std::printf("hx_three=%d\n", hx_three);
    std::printf("hx_dotdot_file=%d\n", hx_dotdot_file);
    std::printf("hx_dotdot_txt=%d\n", hx_dotdot_txt);
    std::printf("hx_root_txt=%d\n", hx_root_txt);
    std::printf("hx_drive=%d\n", hx_drive);
    std::printf("hx_drive_txt=%d\n", hx_drive_txt);
    std::printf("hx_bslash=%d\n", hx_bslash);
    std::printf("hx_bslash_txt=%d\n", hx_bslash_txt);
    std::printf("hx_unc=%d\n", hx_unc);
    std::printf("hx_net=%d\n", hx_net);
    std::printf("hx_parent_ext=%d\n", hx_parent_ext);
    if (!empty_fn_empty || empty_fn_dot || !fn_b || !ext || !parent || !join || !dotdot || !keep || !stem_empty ||
        stem_empty_dot || !stem_txt || !stem_b || !stem_dot || !stem_dotdot || !stem_hidden || !stem_keep ||
        !stem_multi || !cmp_empty || !cmp_empty_a || !cmp_a_empty || !cmp_aa || !cmp_ab || !cmp_ba || !cmp_prefix ||
        !cmp_longer || !cmp_dotdot || !cmp_keep || !cmp_dots || !repl_md || !repl_clear || !repl_nodot || !repl_dots ||
        !repl_dots_md || !repl_keep_dots || !repl_empty_path || !repl_multi || !repl_hidden || abs_empty ||
        !rel_empty || !abs_root || rel_root || !abs_slash || rel_slash || abs_rel || !rel_rel || abs_nested ||
        !rel_nested || abs_dotdot || !rel_dotdot || !abs_root_dotdot || rel_root_dotdot || abs_blank ||
        !rel_blank || abs_drive || !rel_drive || abs_bslash || !rel_bslash || !abs_unc || rel_unc || root_empty ||
        !root_root || !root_slash || root_rel || root_nested || root_dotdot || !root_root_dotdot || root_blank ||
        root_drive || root_bslash || !root_unc || !root_net || has_empty || has_blank || has_root || !has_slash ||
        !has_rel || !has_nested || has_trail || has_trail_b || !has_dot || !has_dots || !has_keep || !has_dotdot ||
        !has_root_dotdot || !has_drive || !has_bslash || !has_unc || !has_net || !has_txt || !has_hidden ||
        hx_empty || hx_blank || hx_root || hx_trail || hx_trail_b || hx_plain || hx_nested || hx_dot || hx_dots ||
        hx_dot_file || hx_dot_parent || hx_hidden || hx_hidden_dir || !hx_txt || !hx_file || !hx_multi || !hx_later ||
        !hx_trailing_dot || !hx_three || hx_dotdot_file || !hx_dotdot_txt || !hx_root_txt || hx_drive ||
        !hx_drive_txt || hx_bslash || !hx_bslash_txt || hx_unc || hx_net || hx_parent_ext)
        return 1;
    return 0;
}
