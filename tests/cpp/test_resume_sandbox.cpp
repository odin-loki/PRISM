// --resume replays a journal only for the inputs that wrote it (run_key.json),
// --allow-exec runs scanned code only in an isolating sandbox unless
// --allow-exec=unsandboxed says otherwise, the bwrap jail binds only what a
// run needs, temporary directories are private, and libclang candidates are
// ordered by version number.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/journal.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"
#include "prism/sandbox.hpp"
#include "prism/svcomp.hpp"

#include "../../src/prism/astlint_internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct Tree {
    fs::path dir, root, out;
    explicit Tree(const std::string& tag) {
        dir = prism::sandbox::private_temp_dir("prism_rs_" + tag + "_");
        REQUIRE_FALSE(dir.empty());
        root = dir / "src";
        out = dir / "out";
        fs::create_directories(root);
        put("add.c", "int add(int x) { return x; }\n");
    }
    void put(const std::string& name, const std::string& text) const {
        std::ofstream(root / name, std::ios::binary) << text;
    }
    prism::Config cfg(std::vector<std::string> stages, bool resume = false) const {
        auto c = prism::default_config();
        c.root = root;
        c.out = out;
        c.llm = false;
        c.unwind = 2;
        c.fuzz_budget = 0.1;
        c.fuzz_iters = 1;
        c.repair_rounds = 1;
        c.stages = std::move(stages);
        c.skip.clear();
        c.resume = resume;
        return c;
    }
    ~Tree() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

const prism::StageResult& stage_of(const prism::RunReport& r, const std::string& name) {
    auto it = std::find_if(r.stages.begin(), r.stages.end(), [&](auto& s) { return s.name == name; });
    REQUIRE(it != r.stages.end());
    return *it;
}

bool has_fn(const prism::RunReport& r, const std::string& name) {
    return std::any_of(r.functions.begin(), r.functions.end(), [&](auto& f) { return f.name == name; });
}

std::string note_with(const prism::RunReport& r, const std::string& needle) {
    for (auto& n : r.notes)
        if (n.find(needle) != std::string::npos) return n;
    return {};
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

// ---- run key

TEST_CASE("run key: same inputs match; an edit or a general option re-runs every stage") {
    Tree t("key");
    auto cfg = t.cfg({"inventory"});
    auto a = prism::run_key(cfg);
    auto b = prism::run_key(cfg);
    CHECK(a.tree == b.tree);
    CHECK(prism::diff_run_key(a, b).rerun.empty());

    // Same size, same name, different bytes: a content digest sees it.
    t.put("add.c", "int add(int y) { return y; }\n");
    auto c = prism::run_key(cfg);
    CHECK(c.tree != a.tree);
    auto d = prism::diff_run_key(a, c);
    CHECK(d.rerun.contains("inventory"));
    CHECK(d.rerun.contains("unify"));
    CHECK(d.rerun.size() == std::size(prism::STAGE_ORDER) - 1);

    // --out and the skipped vendor/build dirs are not part of the tree.
    fs::create_directories(t.root / "build");
    std::ofstream(t.root / "build" / "x.c") << "int junk;\n";
    auto in_root = cfg;
    in_root.out = t.root / "out";
    fs::create_directories(in_root.out);
    std::ofstream(in_root.out / "report.json") << "{}";
    CHECK(prism::run_key(in_root).tree == c.tree);

    // (With --out outside the root again, root/out is part of the tree.)
    auto unwind = in_root;
    unwind.unwind = 5;
    auto e = prism::diff_run_key(prism::run_key(in_root), prism::run_key(unwind));
    CHECK(e.rerun.size() == std::size(prism::STAGE_ORDER) - 1);
    REQUIRE_FALSE(e.why.empty());
    CHECK(e.why.front().find("unwind") != std::string::npos);
}

TEST_CASE("run key: only the exec options changed re-runs the exec stages only") {
    Tree t("key_exec");
    auto off = t.cfg({"inventory"});
    auto on = off;
    on.allow_exec = true;
    on.allow_unsandboxed = true;
    auto d = prism::diff_run_key(prism::run_key(off), prism::run_key(on));
    std::set<std::string> want;
    for (auto& [s, how] : prism::exec_stages()) want.insert(s);
    CHECK(d.rerun == want);
    CHECK_FALSE(d.rerun.contains("inventory"));
    CHECK_FALSE(d.rerun.contains("classify"));
}

TEST_CASE("run key: saved and loaded back; a broken file is no key") {
    Tree t("key_io");
    auto k = prism::run_key(t.cfg({"inventory"}));
    prism::save_run_key(t.out, k);
    auto back = prism::load_run_key(t.out);
    REQUIRE(back);
    CHECK(back->tree == k.tree);
    CHECK(back->fields == k.fields);
    std::ofstream(t.out / prism::RUN_KEY_JSON) << "{\"version\":1,\"tree\":3}";
    CHECK_FALSE(prism::load_run_key(t.out));
}

TEST_CASE("run key: installing a tool re-runs the stages that use it, nothing else") {
    Tree t("key_tool");
    auto k = prism::run_key(t.cfg({"inventory"}));
    REQUIRE(k.fields.contains("tool:cppcheck"));
    REQUIRE(k.fields.contains("tool:afl-fuzz"));
    auto before = k, after = k;
    before.fields["tool:cppcheck"] = "missing";
    after.fields["tool:cppcheck"] = "/opt/cppcheck/bin/cppcheck";
    auto d = prism::diff_run_key(before, after);
    CHECK(d.rerun == std::set<std::string>{"cppcheck"});
    REQUIRE_FALSE(d.why.empty());
    CHECK(d.why.front().find("tool:cppcheck 'missing'") != std::string::npos);
    before = k;
    before.fields["tool:afl-fuzz"] = "missing";
    after = k;
    after.fields["tool:afl-fuzz"] = "/x/afl-fuzz";
    CHECK(prism::diff_run_key(before, after).rerun == std::set<std::string>{"fuzz", "optional"});
    // A compiler appearing changes almost everything.
    before = k;
    before.fields["tool:cc"] = "missing";
    after = k;
    after.fields["tool:cc"] = "/usr/bin/gcc";
    CHECK(prism::diff_run_key(before, after).rerun.size() == std::size(prism::STAGE_ORDER) - 1);
    // --tool NAME=PATH resolves it: the key follows.
    auto c = t.cfg({"inventory"});
    auto fake = t.dir / "cppcheck";
    std::ofstream(fake) << "#!/bin/sh\n";
    c.tools["cppcheck"] = fake;
    CHECK(prism::run_key(c).fields.at("tool:cppcheck") == fake.generic_string());
}

// ---- --resume

TEST_CASE("resume: an unchanged tree replays, an edited one re-runs and says so") {
    Tree t("resume_edit");
    auto first = prism::run_pipeline(t.cfg({"inventory", "classify"}));
    CHECK(fs::is_regular_file(t.out / prism::RUN_KEY_JSON));
    auto same = prism::run_pipeline(t.cfg({"inventory", "classify"}, true));
    CHECK(stage_of(same, "inventory").started == stage_of(first, "inventory").started);
    CHECK(note_with(same, "re-run").empty());

    t.put("sub.c", "int sub(int x) { return x - 1; }\n");
    auto edited = prism::run_pipeline(t.cfg({"inventory", "classify"}, true));
    CHECK(stage_of(edited, "inventory").started != stage_of(first, "inventory").started);
    CHECK(has_fn(edited, "sub"));  // not the stale function list
    auto n = note_with(edited, "re-run");
    CHECK(n.find("every stage") != std::string::npos);
    CHECK(n.find("files under the scan root changed") != std::string::npos);

    // The journal was restarted: one more resume replays the new rows.
    auto again = prism::run_pipeline(t.cfg({"inventory", "classify"}, true));
    CHECK(stage_of(again, "inventory").started == stage_of(edited, "inventory").started);
    CHECK(has_fn(again, "sub"));
}

TEST_CASE("resume: a journal without run_key.json is not trusted") {
    Tree t("resume_nokey");
    prism::run_pipeline(t.cfg({"inventory", "classify"}));
    fs::remove(t.out / prism::RUN_KEY_JSON);
    auto r = prism::run_pipeline(t.cfg({"inventory", "classify"}, true));
    CHECK(note_with(r, "re-run").find(prism::RUN_KEY_JSON) != std::string::npos);
    CHECK(fs::is_regular_file(t.out / prism::RUN_KEY_JSON));
}

TEST_CASE("resume: turning --allow-exec on re-runs the exec stages, not the rest") {
    Tree t("resume_exec");
    t.put("run.c", "int add2(int x) { return x + 2; }\n// prism: run\nint selftest(void) { return add2(1) - 3; }\n");
    std::vector<std::string> st{"inventory", "classify", "sanitize"};
    auto first = prism::run_pipeline(t.cfg(st));
    CHECK(stage_of(first, "sanitize").status == "NOTRUN");
    auto c = t.cfg(st, true);
    c.allow_exec = true;
    c.allow_unsandboxed = true;  // whatever the host's sandbox is
    auto second = prism::run_pipeline(c);
    CHECK(stage_of(second, "inventory").started == stage_of(first, "inventory").started);
    CHECK(stage_of(second, "sanitize").started != stage_of(first, "sanitize").started);
    auto n = note_with(second, "re-run");
    CHECK(n.find("sanitize") != std::string::npos);
    CHECK(n.find("allow_exec") != std::string::npos);
    // The sanitize stage was not held back by Law 9 this time.
    for (auto& f : stage_of(second, "sanitize").findings) {
        auto it = f.extra.find("reason");
        CHECK_FALSE((it != f.extra.end() && it->second == prism::sandbox::EXEC_REASON));
    }
}

// ---- the sandbox gate

TEST_CASE("sandbox gate: --allow-exec without an isolating sandbox needs the explicit opt-in") {
    using prism::sandbox::exec_gate;
    CHECK(prism::sandbox::isolates("bwrap"));
    CHECK_FALSE(prism::sandbox::isolates("rlimits-only"));
    CHECK_FALSE(prism::sandbox::isolates("job-object"));

    auto off = exec_gate(false, true, "bwrap");
    CHECK_FALSE(off.run);
    CHECK(off.note.empty());
    auto jail = exec_gate(true, false, "bwrap");
    CHECK(jail.run);
    CHECK(jail.note.empty());
    for (auto* weak : {"rlimits-only", "job-object"}) {
        auto held = exec_gate(true, false, weak);
        CHECK_FALSE(held.run);
        CHECK(held.held_no_sandbox);
        CHECK(held.note.find(prism::sandbox::EXEC_UNSANDBOXED_FLAG) != std::string::npos);
        auto opted = exec_gate(true, true, weak);
        CHECK(opted.run);
        CHECK_FALSE(opted.held_no_sandbox);
        CHECK(opted.note.find("without file-system or network isolation") != std::string::npos);
    }
    auto k = prism::sandbox::kind();
    CHECK((k == "bwrap" || k == "rlimits-only" || k == "job-object"));
}

TEST_CASE("sandbox gate: a held --allow-exec is NOTRUN with the sandbox hint, in the pipeline") {
    if (prism::sandbox::isolates(prism::sandbox::kind())) {
        MESSAGE("bwrap works here: the held path is not reachable on this host");
        return;
    }
    Tree t("held");
    t.put("run.c", "// prism: run\nint selftest(void) { return 0; }\n");
    auto c = t.cfg({"inventory", "classify", "sanitize"});
    c.allow_exec = true;
    auto r = prism::run_pipeline(c);
    CHECK_FALSE(prism::sandbox::allowed());
    CHECK_FALSE(prism::sandbox::held_no_sandbox());  // restored after the run
    auto& san = stage_of(r, "sanitize");
    CHECK(san.status == "NOTRUN");
    REQUIRE_FALSE(san.findings.empty());
    CHECK(san.findings[0].extra.at("install") == prism::sandbox::NO_SANDBOX_INSTALL);
    CHECK(san.findings[0].extra.at("reason") == prism::sandbox::EXEC_REASON);
    CHECK_FALSE(note_with(r, "held back").empty());
}

TEST_CASE("sandbox gate: subcommands parse --allow-exec and --allow-exec=unsandboxed alike") {
    bool allow = false, weak = false;
    CHECK_FALSE(prism::sandbox::parse_allow_exec("--allow-exec=yes", allow, weak));
    CHECK_FALSE(prism::sandbox::parse_allow_exec("--allow", allow, weak));
    CHECK_FALSE(allow);
    CHECK(prism::sandbox::parse_allow_exec("--allow-exec", allow, weak));
    CHECK(allow);
    CHECK_FALSE(weak);
    allow = false;
    CHECK(prism::sandbox::parse_allow_exec("--allow-exec=unsandboxed", allow, weak));
    CHECK(allow);
    CHECK(weak);
    CHECK_FALSE(prism::sandbox::exec_gate_here(false, true).run);
}

TEST_CASE("sandbox gate: svcomp replay held for want of a sandbox says so") {
    auto dir = prism::sandbox::private_temp_dir("prism_rs_replay_");
    REQUIRE_FALSE(dir.empty());
    std::ofstream(dir / "t.c") << "int main(void) { return 0; }\n";
    std::string plain, held;
    {
        prism::sandbox::Policy p(false, false);
        plain = prism::svcomp::replay(dir / "t.c", "no-overflow", false, std::nullopt, dir / "a")["why"].get<std::string>();
    }
    {
        prism::sandbox::Policy p(false, true);
        held = prism::svcomp::replay(dir / "t.c", "no-overflow", false, std::nullopt, dir / "b")["why"].get<std::string>();
    }
    CHECK(plain.find("--allow-exec") != std::string::npos);
    CHECK(held.find("bubblewrap") != std::string::npos);
    CHECK(held.find("--allow-exec=unsandboxed") != std::string::npos);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- the jail

TEST_CASE("sandbox jail: the inherited environment is scrubbed, HOME and TMPDIR are the scratch") {
    auto a = prism::sandbox::jail_env({"PATH", "GITHUB_TOKEN", "SSH_AUTH_SOCK", "AWS_SECRET_ACCESS_KEY",
                                       "ASAN_OPTIONS", "UBSAN_OPTIONS", "AFL_SKIP_CPUFREQ", "LC_ALL", "HOME", "PRISM_FUZZ_SEED"},
                                      "/s");
    auto unset = [&](const std::string& n) {
        for (std::size_t i = 0; i + 1 < a.size(); ++i)
            if (a[i] == "--unsetenv" && a[i + 1] == n) return true;
        return false;
    };
    for (auto* gone : {"GITHUB_TOKEN", "SSH_AUTH_SOCK", "AWS_SECRET_ACCESS_KEY"}) CHECK_MESSAGE(unset(gone), gone);
    for (auto* kept : {"PATH", "ASAN_OPTIONS", "UBSAN_OPTIONS", "AFL_SKIP_CPUFREQ", "LC_ALL", "PRISM_FUZZ_SEED"})
        CHECK_MESSAGE(!unset(kept), kept);
    const std::vector<std::string> tail{"--setenv", "HOME", "/s", "--setenv", "TMPDIR", "/s"};
    REQUIRE(a.size() >= tail.size());
    CHECK(std::vector<std::string>(a.end() - 6, a.end()) == tail);
    CHECK_FALSE(unset("HOME"));  // replaced, not removed
}

TEST_CASE("sandbox jail: no whole-root bind, no home, the scratch dir writable") {
    auto m = prism::sandbox::jail_mounts({"/usr/bin/true"});
    for (std::size_t i = 0; i + 2 < m.size(); ++i)
        CHECK_FALSE((m[i] == "--ro-bind" && m[i + 1] == "/"));
    std::error_code ec;
    if (fs::is_directory("/usr", ec)) {
        bool usr = false;
        for (std::size_t i = 0; i + 1 < m.size(); ++i) usr = usr || (m[i] == "--ro-bind" && m[i + 1] == "/usr");
        CHECK(usr);
    }
    CHECK(contains(m, "/etc/ld.so.cache"));
    CHECK_FALSE(contains(m, "/etc"));
    if (const char* h = std::getenv("HOME"); h && *h) {
        const fs::path home = fs::path(h).lexically_normal();
        for (auto& a : m) {
            if (a.empty() || a[0] != '/') continue;
            // No bound path is the home directory or one of its ancestors.
            auto rel = home.lexically_relative(fs::path(a).lexically_normal());
            CHECK_MESSAGE((rel.empty() || *rel.begin() == ".."), a);
        }
    }
    auto argv = prism::sandbox::bwrap_argv("/usr/bin/bwrap", {"/s/a.out"}, "/s", {"--ro-bind", "/usr", "/usr"});
    const std::vector<std::string> want{"/usr/bin/bwrap", "--ro-bind", "/usr", "/usr", "--dev", "/dev", "--proc",
                                        "/proc", "--tmpfs", "/tmp", "--bind", "/s", "/s", "--unshare-all",
                                        "--die-with-parent", "--", "/s/a.out"};
    CHECK(argv == want);
}

TEST_CASE("sandbox jail: bwrap's 128+N exit is the child's signal N") {
    CHECK(prism::sandbox::jail_exit_signal(139) == 11);
    CHECK(prism::sandbox::jail_exit_signal(134) == 6);
    CHECK(prism::sandbox::jail_exit_signal(128) == 0);
    CHECK(prism::sandbox::jail_exit_signal(1) == 0);
    CHECK(prism::sandbox::jail_exit_signal(255) == 0);
    CHECK_FALSE(prism::sandbox::jailed({"/s/a.out"}));
}

TEST_CASE("private_temp_dir: a new directory each call, owner-only") {
    auto a = prism::sandbox::private_temp_dir("prism_ptd_");
    auto b = prism::sandbox::private_temp_dir("prism_ptd_");
    REQUIRE_FALSE(a.empty());
    REQUIRE_FALSE(b.empty());
    CHECK(a != b);
    CHECK(fs::is_directory(a));
#ifndef _WIN32
    auto p = fs::status(a).permissions();
    CHECK((p & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif
    std::error_code ec;
    fs::remove_all(a, ec);
    fs::remove_all(b, ec);
}

// ---- libclang

TEST_CASE("libclang: versioned names are tried newest first, by number") {
    std::vector<std::string> v{"/l/libclang-9.so.9", "/l/libclang-18.so.18", "/l/libclang.so.18.1",
                               "/l/libclang-17.so.17"};
    prism::astlint::detail::sort_newest_first(v);
    CHECK(v == std::vector<std::string>{"/l/libclang-18.so.18", "/l/libclang.so.18.1", "/l/libclang-17.so.17",
                                        "/l/libclang-9.so.9"});
    std::vector<std::string> d{"/usr/lib/llvm-9", "/usr/lib/llvm-18", "/usr/lib/llvm-23"};
    prism::astlint::detail::sort_newest_first(d);
    CHECK(d == std::vector<std::string>{"/usr/lib/llvm-23", "/usr/lib/llvm-18", "/usr/lib/llvm-9"});
}
