// Fetching pinned archives with the git CLI, building external tools with
// their recipe, and the linked-library checks.
//
// Archive format: the uncompressed tar of `git archive --format=tar
// --prefix=<name>-<commit>/ <commit>` after `git fetch --depth 1 <url> <commit>`.
#include "prism/deps.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#  include <process.h>
#else
#  include <fcntl.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace prism::deps {

namespace {

std::string slurp_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    std::size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n')) ++b;
    return s.substr(b);
}

// A fresh private directory under parent (mkdtemp).
fs::path make_temp_dir(const fs::path& parent, const std::string& prefix) {
    fs::create_directories(parent);
#ifdef _WIN32
    for (int i = 0; i < 1000; ++i) {
        auto cand = parent / (prefix + std::to_string(std::rand()));
        std::error_code ec;
        if (fs::create_directory(cand, ec) && !ec) return cand;
    }
    throw FetchError("cannot create a temporary directory in " + parent.string());
#else
    std::string tmpl = (parent / (prefix + "XXXXXX")).string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!::mkdtemp(buf.data())) throw FetchError("cannot create a temporary directory in " + parent.string());
    return fs::path(buf.data());
#endif
}

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& prefix) : path(make_temp_dir(fs::temp_directory_path(), prefix)) {}
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

ProcResult git(const std::vector<std::string>& args, const fs::path& cwd,
               const fs::path* stdout_file = nullptr) {
    std::vector<std::string> argv{"git"};
    argv.insert(argv.end(), args.begin(), args.end());
    return run_process(argv, cwd, true, stdout_file);
}

}  // namespace

// ------------------------------------------------------------------ process

std::optional<fs::path> which(std::string_view name) {
    std::string n(name);
    if (n.find('/') != std::string::npos) {
        std::error_code ec;
        if (fs::is_regular_file(n, ec)) return fs::path(n);
        return std::nullopt;
    }
    const char* path = std::getenv("PATH");
    if (!path) return std::nullopt;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string p(path);
    std::size_t b = 0;
    while (b <= p.size()) {
        auto e = p.find(sep, b);
        if (e == std::string::npos) e = p.size();
        std::string dir = p.substr(b, e - b);
        b = e + 1;
        if (dir.empty()) dir = ".";
        std::error_code ec;
        for (const char* ext : {"", ".exe"}) {
#ifndef _WIN32
            if (*ext) continue;
#endif
            fs::path cand = fs::path(dir) / (n + ext);
            if (!fs::is_regular_file(cand, ec)) continue;
#ifndef _WIN32
            if (::access(cand.c_str(), X_OK) != 0) continue;
#endif
            return cand;
        }
    }
    return std::nullopt;
}

ProcResult run_process(const std::vector<std::string>& argv, const fs::path& cwd, bool capture,
                       const fs::path* stdout_file) {
    ProcResult r;
    if (argv.empty()) return r;
    TempDir td("prism-deps-proc-");
    const fs::path out_path = stdout_file ? *stdout_file : td.path / "out";
    const fs::path err_path = td.path / "err";
#ifdef _WIN32
    auto quote = [](const std::string& a) {
        std::string q = "\"";
        for (char c : a) {
            if (c == '"') q += '\\';
            q += c;
        }
        return q + "\"";
    };
    std::string cmd = "cd /d " + quote(cwd.string()) + " && ";
    for (std::size_t i = 0; i < argv.size(); ++i) cmd += (i ? " " : "") + quote(argv[i]);
    if (capture || stdout_file) cmd += " > " + quote(out_path.string());
    if (capture) cmd += " 2> " + quote(err_path.string());
    r.code = std::system(("\"" + cmd + "\"").c_str());
#else
    std::cout.flush();
    std::cerr.flush();
    pid_t pid = ::fork();
    if (pid < 0) throw FetchError("fork failed: " + std::string(std::strerror(errno)));
    if (pid == 0) {
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) _exit(127);
        if (capture || stdout_file) {
            int fd = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) _exit(127);
            ::dup2(fd, 1);
            ::close(fd);
        }
        if (capture) {
            int fd = ::open(err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) _exit(127);
            ::dup2(fd, 2);
            ::close(fd);
        }
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            ::dup2(devnull, 0);
            ::close(devnull);
        }
        std::vector<char*> cargv;
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        ::execvp(cargv[0], cargv.data());
        const char msg[] = "prism-deps: cannot execute\n";
        [[maybe_unused]] auto n = ::write(2, msg, sizeof(msg) - 1);
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) throw FetchError("waitpid failed");
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
    if (capture) {
        if (!stdout_file) r.out = slurp_file(out_path);
        r.err = slurp_file(err_path);
    }
    return r;
}

// -------------------------------------------------------------------- fetch

void fetch_archive(const Table& comp, const fs::path& dest_tar) {
    const auto name = comp.str("name"), url = comp.str("url"), commit = comp.str("commit");
    {
        TempDir td("prism-fetch-");
        const fs::path& repo = td.path;
        auto r = git({"init", "-q", "--bare", "."}, repo);
        if (r.code) throw FetchError(name + ": git init failed: " + r.err);
        r = git({"fetch", "-q", "--depth", "1", url, commit}, repo);
        if (r.code) throw FetchError(name + ": git fetch " + url + " " + commit + " failed: " + trim(r.err));
        r = git({"rev-parse", "FETCH_HEAD^{commit}"}, repo);
        if (r.code || trim(r.out) != commit)
            throw FetchError(name + ": fetched '" + trim(r.out) + "', pinned " + commit);
        r = git({"archive", "--format=tar", "--prefix=" + name + "-" + commit + "/", commit}, repo, &dest_tar);
        if (r.code) {
            std::error_code ec;
            fs::remove(dest_tar, ec);
            throw FetchError(name + ": git archive failed: " + r.err);
        }
    }
    const auto expected = comp.str("archive_sha256");
    const auto got = sha256_file(dest_tar);
    if (got != expected) {
        std::error_code ec;
        fs::remove(dest_tar, ec);
        throw HashMismatch(name + ": archive sha256 mismatch\n  expected " + expected + "\n  got      " + got +
                           "\nrefusing to use it (fail closed). If upstream really changed, re-pin "
                           "third_party/MANIFEST.toml in a reviewed commit.");
    }
}

// ------------------------------------------------------------------ recipes

namespace {

void run_step(const std::vector<std::string>& cmd, const fs::path& cwd) {
    std::cout << "  $ " << join(cmd, " ") << std::endl;
    auto r = run_process(cmd, cwd, false);
    if (r.code) throw FetchError("build step failed (" + std::to_string(r.code) + "): " + join(cmd, " "));
}

std::string jobs() { return std::to_string(std::max(1u, std::thread::hardware_concurrency())); }

fs::path home_dir() {
    const char* h = std::getenv("HOME");
    return h ? fs::path(h) : fs::path();
}

constexpr const char* kBitwuzlaCadicalTag = "rel-2.1.2";
constexpr const char* kBitwuzlaCadicalCommit = "3ff42f04384489916f017acd6d5e7cbfa7257be7";

void recipe(const std::string& name, const std::string& recipe, const fs::path& src) {
    if (recipe == "configure-make") {
        run_step({"sh", "./configure"}, src);
        run_step({"make", "-j", jobs()}, src);
    } else if (recipe == "cake_lpr") {
        std::optional<fs::path> cc = which("cc");
        if (!cc) cc = which("gcc");
        if (!cc) cc = which("clang");
        if (!cc) throw FetchError("cake_lpr: no C compiler (cc/gcc/clang) on PATH");
        // The Makefile's default target: the CakeML-compiled checker
        // (cake_lpr.S, shipped verified) linked with its C FFI shim.
        run_step({cc->string(), "-O2", "basis_ffi.c", "cake_lpr.S", "-o", "cake_lpr", "-std=c99"}, src);
    } else if (recipe == "bitwuzla") {
        // Bitwuzla builds with meson. Its CaDiCaL subproject is a wrap-file
        // whose source is a GitHub archive download, which some proxies
        // refuse (403); meson uses an already-present subproject directory
        // instead, so clone the wrap's tag with git and check the commit.
        auto sub = src / "subprojects" / (std::string("cadical-") + kBitwuzlaCadicalTag);
        if (!fs::is_directory(sub)) {
            auto r = git({"clone", "-q", "--depth", "1", "--branch", kBitwuzlaCadicalTag,
                          "https://github.com/arminbiere/cadical", sub.string()},
                         src);
            if (r.code) throw FetchError("bitwuzla: cadical subproject clone failed: " + r.err);
            auto head = trim(git({"rev-parse", "HEAD"}, sub).out);
            if (head != kBitwuzlaCadicalCommit) {
                std::error_code ec;
                fs::remove_all(sub, ec);
                throw FetchError("bitwuzla: cadical " + std::string(kBitwuzlaCadicalTag) + " is " + head +
                                 ", expected " + kBitwuzlaCadicalCommit);
            }
            auto overlay = src / "subprojects" / "packagefiles" / "cadical";
            if (fs::is_directory(overlay))
                fs::copy(overlay, sub, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        }
        // configure.py is upstream's own meson front end.
        auto py = which("python3");
        if (!py) throw FetchError("bitwuzla: its configure.py needs python3 on PATH");
        run_step({py->string(), "configure.py", "release"}, src);
        run_step({"ninja", "-C", "build", "-j", jobs()}, src);
    } else if (recipe == "lake") {
        auto lake = which("lake");
        if (!lake) {
            auto h = home_dir() / ".elan" / "bin" / "lake";
            if (fs::is_regular_file(h)) lake = h;
        }
        if (!lake) throw FetchError("lake not found (install elan: https://github.com/leanprover/elan)");
        run_step({lake->string(), "build"}, src);
    } else if (recipe == "cargo") {
        auto cargo = which("cargo");
        if (!cargo) {
            auto h = home_dir() / ".cargo" / "bin" / "cargo";
            if (fs::is_regular_file(h)) cargo = h;
        }
        if (!cargo) throw FetchError("cargo not found (install a Rust toolchain: https://rustup.rs)");
        run_step({cargo->string(), "build", "--release", "--locked"}, src);
    } else {
        throw FetchError(name + ": unknown recipe '" + recipe + "'");
    }
}

std::string json_str(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (u < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", u);
            out += b;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

// The value of "key": "..." / true / false in a stamp this tool wrote.
std::optional<std::string> stamp_field(const std::string& text, const std::string& key) {
    auto k = text.find("\"" + key + "\":");
    if (k == std::string::npos) return std::nullopt;
    auto p = k + key.size() + 3;
    while (p < text.size() && text[p] == ' ') ++p;
    if (text.compare(p, 4, "true") == 0) return "true";
    if (text.compare(p, 5, "false") == 0) return "false";
    if (p < text.size() && text[p] == '"') {
        auto e = text.find('"', p + 1);
        if (e != std::string::npos) return text.substr(p + 1, e - p - 1);
    }
    return std::nullopt;
}

}  // namespace

fs::path fetch_tool(const Table& comp, const fs::path& base, bool build) {
    const auto name = comp.str("name"), kind = comp.str("kind");
    if (kind == "system")
        throw FetchError(name + " is a system tool: " +
                         (comp.truthy("install") ? comp.str("install") : std::string("install it on PATH")));
    if (kind != "external") throw FetchError(name + " is " + kind + "; use `prism-deps linked`");
    const fs::path final_dir = install_dir(comp, base);
    const auto sha = comp.str("archive_sha256");
    const auto recipe_name = comp.str("recipe");
    std::error_code ec;
    if (fs::is_regular_file(final_dir / kStamp, ec)) {
        auto text = slurp_file(final_dir / kStamp);
        if (stamp_field(text, "archive_sha256") == sha &&
            (stamp_field(text, "built") == "true" || !build || recipe_name.empty())) {
            std::cout << name << ": already installed at " << final_dir.string() << "\n";
            return final_dir;
        }
    }
    fs::create_directories(final_dir.parent_path());
    const fs::path stage = make_temp_dir(final_dir.parent_path(), "." + comp.str("commit").substr(0, 12) + "-");
    bool built = false;
    std::vector<std::string> bins;
    try {
        auto tar = stage / "source.tar";
        std::cout << name << ": fetching " << comp.str("url") << " @ " << comp.str("commit") << std::endl;
        fetch_archive(comp, tar);
        std::cout << name << ": sha256 OK " << sha << "\n";
        auto top = extract_tar(tar, stage / "x");
        fs::remove(tar);
        auto src = stage / "src";
        fs::rename(top, src);
        fs::remove(stage / "x");
        if (build && !recipe_name.empty()) {
            recipe(name, recipe_name, src);
            fs::create_directory(stage / "bin");
            for (const auto& rel : comp.list("recipe_out")) {
                auto exe = src / rel;
                if (!fs::is_regular_file(exe)) throw FetchError(name + ": build did not produce " + rel);
                fs::copy_file(exe, stage / "bin" / exe.filename(), fs::copy_options::overwrite_existing);
                fs::permissions(stage / "bin" / exe.filename(), fs::status(exe).permissions(),
                                fs::perm_options::replace);
                bins.push_back(exe.filename().string());
            }
            built = true;
        }
        std::string b = "[";
        if (!bins.empty()) {
            b = "[\n";
            for (std::size_t i = 0; i < bins.size(); ++i)
                b += "    " + json_str(bins[i]) + (i + 1 < bins.size() ? ",\n" : "\n");
            b += "  ]";
        } else {
            b += "]";
        }
        std::ofstream o(stage / kStamp, std::ios::binary);
        o << "{\n"
          << "  \"name\": " << json_str(name) << ",\n"
          << "  \"version\": " << json_str(comp.str("version")) << ",\n"
          << "  \"url\": " << json_str(comp.str("url")) << ",\n"
          << "  \"commit\": " << json_str(comp.str("commit")) << ",\n"
          << "  \"archive_sha256\": " << json_str(sha) << ",\n"
          << "  \"spdx\": " << json_str(comp.str("spdx")) << ",\n"
          << "  \"built\": " << (built ? "true" : "false") << ",\n"
          << "  \"bins\": " << b << "\n"
          << "}\n";
        o.close();
        if (!o) throw FetchError(name + ": cannot write " + (stage / kStamp).string());
        if (fs::exists(final_dir)) fs::remove_all(final_dir);
        fs::rename(stage, final_dir);
    } catch (...) {
        fs::remove_all(stage, ec);
        throw;
    }
    if (built) {
        for (const auto& b : bins) std::cout << name << ": installed " << (final_dir / "bin" / b).string() << "\n";
    } else {
        auto hint = comp.truthy("build_hint") ? comp.str("build_hint") : "see the upstream build instructions";
        auto names = comp.list("bins");
        if (names.empty()) names = {name};
        std::cout << name << ": verified source at " << (final_dir / "src").string() << "\n"
                  << "  no automatic build recipe. Build it yourself: " << hint << "\n"
                  << "  <prefix> = " << final_dir.string() << "  (PRISM looks for "
                  << (final_dir / "bin").string() << "/" << join(names, "|") << ")\n";
    }
    return final_dir;
}

// ----------------------------------------------------------- linked checks

int run_linked(const Manifest& m, bool refetch) {
    int bad = 0;
    for (const Table* comp : components(m, "linked")) {
        auto errs = check_linked(*comp, m.root);
        if (refetch && errs.empty()) {
            try {
                TempDir td("prism-linked-");
                auto tar = td.path / (comp->str("name") + ".tar");
                fetch_archive(*comp, tar);
                auto top = extract_tar(tar, td.path / "x");
                auto drift = diff_against_upstream(*comp, top, m.root);
                auto want = comp->list("drift");
                sort_drift(want);
                if (drift != want) {
                    auto show = [](const std::vector<std::string>& v) {
                        std::string s = "[";
                        for (std::size_t i = 0; i < v.size(); ++i) s += (i ? ", '" : "'") + v[i] + "'";
                        return s + "]";
                    };
                    errs.push_back(comp->str("name") +
                                   ": in-tree copy differs from pinned upstream beyond the recorded drift:\n"
                                   "  recorded " + show(want) + "\n  actual   " + show(drift));
                }
            } catch (const FetchError& e) {
                errs.push_back(e.what());
            }
        }
        std::string status = errs.empty() ? "OK  " : "FAIL";
        std::cout << status << " linked " << comp->str("name") << " " << comp->str("version") << " "
                  << comp->str("commit").substr(0, 12)
                  << (refetch && errs.empty() ? " (archive re-fetched, sha256 verified)" : "") << "\n";
        for (const auto& e : errs) {
            std::string indented = "     ";
            for (char c : e) {
                indented += c;
                if (c == '\n') indented += "     ";
            }
            std::cout << indented << "\n";
        }
        bad += errs.empty() ? 0 : 1;
    }
    return bad ? 1 : 0;
}

}  // namespace prism::deps
