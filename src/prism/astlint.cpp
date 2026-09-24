// Roadmap 2.8: C/C++ lints on the Clang AST (include/prism/astlint.hpp).
//
// Two front ends feed one set of checks (astlint_checks.cpp):
//   * libclang (astlint_libclang.cpp), loaded at run time: the unit is parsed
//     in process and only the checked files' declarations are converted;
//   * clang as a process (this file), the fallback:
//     `clang -fsyntax-only -Xclang -ast-dump=json` prints the whole
//     translation unit, headers included (a C++ unit that includes <vector>
//     and <map> is ~200 MB of JSON). The dump is split without a full parse:
//     each top-level declaration is bracket-matched in the text, and only
//     those whose location is in a checked file (the unit's main file or a
//     scanned project header; plus enum declarations from anywhere, for the
//     switch check) are handed to nlohmann::json.
//
// The dumper elides a location's "file" (and "line") when it equals the one
// printed before it, so the current file is state carried in document order;
// every location object gets the resolved canonical file in "_f", and lines
// come from "offset" (always printed) against the file's own bytes.

#include "prism/astlint.hpp"

#include "astlint_internal.hpp"

#include "prism/checkers.hpp"
#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/threads.hpp"

#include "proc.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace prism::astlint {
namespace fs = std::filesystem;
using detail::json;

namespace {

constexpr std::size_t npos = std::string_view::npos;
constexpr const char* kInstall =
    "install libclang (apt install libclang1-18) or clang (apt install clang-18) for the AST lints";

// ---------------------------------------------------------------- dump split

bool is_ws(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

std::size_t skip_ws(std::string_view s, std::size_t i) {
    while (i < s.size() && is_ws(s[i])) ++i;
    return i;
}

// One past the end of the JSON value starting at s[i]; npos when malformed.
std::size_t skip_value(std::string_view s, std::size_t i) {
    if (i >= s.size()) return npos;
    const char c = s[i];
    if (c == '"') {
        for (++i; i < s.size(); ++i) {
            if (s[i] == '\\') ++i;
            else if (s[i] == '"') return i + 1;
        }
        return npos;
    }
    if (c == '{' || c == '[') {
        int depth = 0;
        while (i < s.size()) {
            const char d = s[i];
            if (d == '"') {
                i = skip_value(s, i);
                if (i == npos) return npos;
                continue;
            }
            if (d == '{' || d == '[') ++depth;
            else if ((d == '}' || d == ']') && --depth == 0) return i + 1;
            ++i;
        }
        return npos;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !is_ws(s[i])) ++i;
    return i;
}

std::optional<std::string> json_string_at(std::string_view s, std::size_t i) {
    auto e = skip_value(s, i);
    if (e == npos || s[i] != '"') return std::nullopt;
    try {
        return json::parse(s.substr(i, e - i)).get<std::string>();
    } catch (...) {
        return std::nullopt;
    }
}

// Position of the value of `"key":` searching forward from `from`, or npos.
std::size_t find_key(std::string_view s, std::string_view key, std::size_t from, std::size_t to) {
    std::string pat = "\"" + std::string(key) + "\"";
    for (auto p = s.find(pat, from); p != npos && p < to; p = s.find(pat, p + 1)) {
        auto q = skip_ws(s, p + pat.size());
        if (q < to && s[q] == ':') return skip_ws(s, q + 1);
    }
    return npos;
}

// The last location "file" value in s[b, e) — not an "includedFrom" file.
std::optional<std::string> last_loc_file(std::string_view s, std::size_t b, std::size_t e) {
    constexpr std::string_view pat = "\"file\"";
    std::size_t hi = e;
    while (hi > b) {
        auto p = s.rfind(pat, hi - 1);
        if (p == npos || p < b) return std::nullopt;
        hi = p;
        auto q = skip_ws(s, p + pat.size());
        if (q >= e || s[q] != ':') continue;
        auto v = skip_ws(s, q + 1);
        // "includedFrom": { "file": ... } names the includer, not a location.
        std::size_t k = p;
        while (k > b && is_ws(s[k - 1])) --k;
        if (k > b && s[k - 1] == '{') {
            --k;
            while (k > b && is_ws(s[k - 1])) --k;
            if (k > b && s[k - 1] == ':') {
                --k;
                while (k > b && is_ws(s[k - 1])) --k;
                constexpr std::string_view inc = "\"includedFrom\"";
                if (k >= b + inc.size() && s.substr(k - inc.size(), inc.size()) == inc) continue;
            }
        }
        if (auto str = json_string_at(s, v)) return str;
    }
    return std::nullopt;
}

// Spelled file name -> canonical path (cached per dump).
struct Canon {
    std::unordered_map<std::string, std::string> memo;
    const std::string& operator()(const std::string& spelled) {
        auto it = memo.find(spelled);
        if (it != memo.end()) return it->second;
        return memo.emplace(spelled, detail::canon_path(spelled)).first->second;
    }
};

// Resolve every location object's file ("_f", canonical) in document order.
void annotate(json& o, std::string& cur, Canon& canon) {
    if (o.is_object()) {
        if (o.contains("offset")) {
            auto it = o.find("file");
            if (it != o.end() && it->is_string()) cur = it->get<std::string>();
            o["_f"] = canon(cur);
        }
        for (auto& el : o.items()) {
            if (el.key() == "includedFrom" || el.key() == "_f") continue;
            annotate(el.value(), cur, canon);
        }
    } else if (o.is_array()) {
        for (auto& v : o) annotate(v, cur, canon);
    }
}

// Top-level declarations of the checked files (and every EnumDecl).
std::optional<std::vector<json>> split_dump(std::string_view s, const detail::FileSet& files, std::string& err) {
    auto root = s.find('{');
    if (root == npos) {
        err = "no JSON in clang output";
        return std::nullopt;
    }
    auto root_end = skip_value(s, root);
    if (root_end == npos) {
        err = "truncated AST dump";
        return std::nullopt;
    }
    auto kind_at = find_key(s, "kind", root, root_end);
    auto kind = kind_at == npos ? std::nullopt : json_string_at(s, kind_at);
    if (!kind || *kind != "TranslationUnitDecl") {
        err = "AST dump does not start with a TranslationUnitDecl";
        return std::nullopt;
    }
    std::vector<json> out;
    auto arr = find_key(s, "inner", root, root_end);
    if (arr == npos) return out;  // empty unit
    if (s[arr] != '[') {
        err = "malformed AST dump";
        return std::nullopt;
    }
    Canon canon;
    std::string cur;
    std::size_t i = skip_ws(s, arr + 1);
    while (i < root_end && s[i] != ']') {
        if (s[i] != '{') {
            err = "malformed AST dump";
            return std::nullopt;
        }
        auto e = skip_value(s, i);
        if (e == npos || e > root_end) {
            err = "truncated AST dump";
            return std::nullopt;
        }
        std::string at_start = cur;
        std::optional<std::string> file;
        bool implicit_loc = true;
        if (auto l = find_key(s, "loc", i, e); l != npos && s[l] == '{') {
            auto le = skip_value(s, l);
            if (le != npos && skip_ws(s, l + 1) != le - 1) {
                implicit_loc = false;
                file = last_loc_file(s, l, le);
                if (!file) file = cur;
            }
        }
        auto k = find_key(s, "kind", i, e);
        auto kname = k == npos ? std::nullopt : json_string_at(s, k);
        const bool keep = !implicit_loc && ((file && files.wanted(canon(*file))) || (kname && *kname == "EnumDecl"));
        if (auto last = last_loc_file(s, i, e)) cur = *last;
        if (keep) {
            try {
                auto j = json::parse(s.substr(i, e - i));
                annotate(j, at_start, canon);
                out.push_back(std::move(j));
            } catch (const std::exception& ex) {
                err = std::string("AST dump does not parse: ") + ex.what();
                return std::nullopt;
            }
        }
        i = skip_ws(s, e);
        if (i < root_end && s[i] == ',') i = skip_ws(s, i + 1);
    }
    return out;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string lower_ext(const fs::path& p) {
    auto e = p.extension().string();
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool is_cxx_unit(const fs::path& p) {
    auto e = lower_ext(p);
    return e == ".cc" || e == ".cpp" || e == ".cxx" || e == ".ii" || e == ".c++";
}

// ---------------------------------------------------------------- compile db

std::vector<std::string> shell_split(const std::string& cmd) {
    std::vector<std::string> out;
    std::string cur;
    bool have = false;
    char q = 0;
    for (std::size_t i = 0; i < cmd.size(); ++i) {
        char c = cmd[i];
        if (q) {
            if (c == q) q = 0;
            else if (c == '\\' && q == '"' && i + 1 < cmd.size()) cur += cmd[++i];
            else cur += c;
        } else if (c == '"' || c == '\'') {
            q = c;
            have = true;
        } else if (c == '\\' && i + 1 < cmd.size()) {
            cur += cmd[++i];
            have = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (have || !cur.empty()) out.push_back(cur);
            cur.clear();
            have = false;
        } else {
            cur += c;
        }
    }
    if (have || !cur.empty()) out.push_back(cur);
    return out;
}

std::vector<std::string> keep_flags(const std::vector<std::string>& argv, const fs::path& dir) {
    std::vector<std::string> out;
    auto abs = [&](const std::string& p) {
        fs::path q(p);
        return (q.is_relative() ? dir / q : q).lexically_normal().string();
    };
    static const std::vector<std::string> kPathFlags{"-I", "-isystem", "-iquote", "-idirafter", "-include"};
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const auto& a = argv[i];
        bool done = false;
        for (auto& pf : kPathFlags) {
            if (a == pf && i + 1 < argv.size()) {
                out.push_back(pf);
                out.push_back(abs(argv[++i]));
                done = true;
                break;
            }
            if (pf == "-I" && a.size() > 2 && a.starts_with("-I")) {
                out.push_back("-I" + abs(a.substr(2)));
                done = true;
                break;
            }
        }
        if (done) continue;
        if ((a == "-D" || a == "-U") && i + 1 < argv.size()) {
            out.push_back(a);
            out.push_back(argv[++i]);
        } else if ((a.starts_with("-D") || a.starts_with("-U")) && a.size() > 2) {
            out.push_back(a);
        } else if (a.starts_with("-std=")) {
            out.push_back(a);
        }
    }
    return out;
}

using Db = std::map<std::string, std::vector<std::string>>;

std::shared_ptr<const Db> load_db(const fs::path& db) {
    static std::mutex mu;
    static std::map<std::string, std::pair<fs::file_time_type, std::shared_ptr<const Db>>> cache;
    std::lock_guard<std::mutex> lock(mu);
    auto key = detail::canon_path(db);
    std::error_code ec;
    auto mtime = fs::last_write_time(db, ec);
    if (auto it = cache.find(key); it != cache.end() && it->second.first == mtime) return it->second.second;
    auto m = std::make_shared<Db>();
    try {
        auto j = nlohmann::json::parse(read_file(db));
        for (auto& e : j) {
            if (!e.is_object() || !e.contains("file") || !e["file"].is_string()) continue;
            fs::path dir = e.value("directory", std::string());
            fs::path file = e["file"].get<std::string>();
            if (file.is_relative()) file = dir / file;
            std::vector<std::string> argv;
            if (e.contains("arguments") && e["arguments"].is_array()) {
                for (auto& a : e["arguments"])
                    if (a.is_string()) argv.push_back(a.get<std::string>());
            } else if (e.contains("command") && e["command"].is_string()) {
                argv = shell_split(e["command"].get<std::string>());
            }
            (*m)[detail::canon_path(file)] = keep_flags(argv, dir);
        }
    } catch (...) {
        m->clear();
    }
    cache[key] = {mtime, m};
    return m;
}

// The front-end flags for one unit (both front ends; argv[0] not included).
std::vector<std::string> unit_flags(const fs::path& src, const fs::path& root, bool& has_db) {
    const bool cxx = is_cxx_unit(src);
    std::vector<std::string> argv{"-w"};
    auto db = compile_db_flags(root, src);
    has_db = !db.empty();
    const bool has_std = std::any_of(db.begin(), db.end(), [](auto& f) { return f.starts_with("-std="); });
    argv.push_back(cxx ? "-xc++" : "-xc");
    if (!has_std) argv.push_back(cxx ? "-std=gnu++23" : "-std=gnu17");
    if (!cxx) {
        // Front-end leniency as in the pir stage: old C still parses.
        for (auto* w : {"-Wno-error=implicit-function-declaration", "-Wno-error=implicit-int",
                        "-Wno-error=int-conversion", "-Wno-error=incompatible-pointer-types"})
            argv.push_back(w);
    }
    if (db.empty()) {
        std::error_code ec;
        fs::path base = fs::is_directory(root, ec) ? root : root.parent_path();
        argv.push_back("-I" + src.parent_path().string());
        if (!base.empty()) argv.push_back("-I" + base.string());
        if (fs::is_directory(base / "include", ec)) argv.push_back("-I" + (base / "include").string());
    }
    argv.insert(argv.end(), db.begin(), db.end());
    return argv;
}

std::string name_error(std::string first, const fs::path& src, std::string_view rel, bool has_db) {
    if (auto at = first.find(src.string()); at != std::string::npos) first.replace(at, src.string().size(), std::string(rel));
    if (first.size() > 240) first = first.substr(0, 240) + "...";
    return "does not parse" + (first.empty() ? std::string() : ": " + first) +
           (has_db ? "" : " (no compile_commands.json entry)");
}

detail::FileSet file_set(const fs::path& src, std::string_view rel, const Headers& headers) {
    detail::FileSet fsx;
    fsx.main = detail::canon_path(src);
    fsx.main_rel = std::string(rel);
    for (auto& [c, r] : headers) fsx.rel[c] = r;
    return fsx;
}

}  // namespace

// ---------------------------------------------------------------- public API

FileResult analyze_dump(std::string_view dump, const std::string& main_file, std::string_view source,
                        std::string_view rel, const Headers& headers) {
    FileResult r;
    std::string err;
    auto fsx = file_set(main_file, rel, headers);
    if (!source.empty()) fsx.sources[fsx.main] = std::string(source);
    auto decls = split_dump(dump, fsx, err);
    if (!decls) {
        r.error = err;
        return r;
    }
    detail::Unit u;
    u.decls = std::move(*decls);
    u.cxx = is_cxx_unit(main_file);
    u.backend = "process";
    r = detail::analyze_unit(u, fsx);
    r.backend = "process";
    return r;
}

std::vector<std::string> compile_db_flags(const fs::path& root, const fs::path& src) {
    std::error_code ec;
    fs::path base = fs::is_directory(root, ec) ? root : root.parent_path();
    for (auto& cand : {base / "compile_commands.json", base / "build" / "compile_commands.json"}) {
        if (!fs::is_regular_file(cand, ec)) continue;
        auto db = load_db(cand);
        if (auto it = db->find(detail::canon_path(src)); it != db->end()) return it->second;
    }
    return {};
}

bool libclang_available(std::string* why, std::string* where) {
    Config cfg;
    auto fe = pir::find_frontend(cfg);
    return detail::libclang_load(fe.clang ? fe.clang : fe.clangxx, why, where);
}

FileResult run_file(const std::optional<fs::path>& clang, const fs::path& src, const fs::path& root,
                    std::string_view rel, double timeout_s, Backend backend, const Headers& headers) {
    FileResult r;
    bool has_db = false;
    auto flags = unit_flags(src, root, has_db);
    std::string why;
    const bool use_lib = backend != Backend::Process && detail::libclang_load(clang, &why);
    if (backend == Backend::LibClang && !use_lib) {
        r.error = why;
        return r;
    }
    if (use_lib) {
        auto fsx = file_set(src, rel, headers);
        std::string err;
        auto u = detail::libclang_unit(flags, src.string(), fsx, is_cxx_unit(src), err);
        if (!u) {
            r.error = err.starts_with("does not parse: ") ? name_error(err.substr(16), src, rel, has_db) : err;
            return r;
        }
        r = detail::analyze_unit(*u, fsx);
        r.backend = "libclang";
        return r;
    }
    if (!clang) {
        r.error = "clang not found";
        return r;
    }
    // -fsyntax-only + the JSON dump is a parse, not a check: -w keeps clang's
    // warnings (the `warnings` stage reports them) out of the merged
    // stdout/stderr so the dump stays valid JSON. Errors still fail the parse.
    std::vector<std::string> argv{clang->string(), "-fsyntax-only", "-Xclang", "-ast-dump=json"};
    argv.insert(argv.end(), flags.begin(), flags.end());
    argv.push_back(src.string());
    auto pr = ::prism::detail::run_process(argv, timeout_s);
    if (pr.failed) {
        r.error = "could not start " + clang->string();
        return r;
    }
    if (pr.timed_out) {
        r.error = "clang timed out after " + std::to_string(static_cast<int>(timeout_s)) + " s";
        return r;
    }
    if (pr.rc != 0) {
        std::string first;
        std::size_t from = pr.text.size() > (1u << 20) ? pr.text.size() - (1u << 20) : 0;
        std::istringstream ls(pr.text.substr(from));
        std::string line;
        while (std::getline(ls, line)) {
            if (line.find("error:") != std::string::npos) {
                first = line;
                break;
            }
        }
        if (first.empty() && pr.rc == 127) first = "could not start " + clang->string();
        r.error = name_error(first, src, rel, has_db);
        return r;
    }
    return analyze_dump(pr.text, src.string(), {}, rel, headers);
}

void supersede(std::vector<Finding>& regex, std::vector<Finding>& ast) {
    std::map<std::tuple<std::string, int, std::string>, Finding*> idx;
    for (auto& f : ast)
        if (f.line) idx[{f.file, *f.line, f.cls}] = &f;
    std::vector<Finding> kept;
    kept.reserve(regex.size());
    for (auto& f : regex) {
        if (f.status == laws::FAILED && f.line) {
            if (auto it = idx.find({f.file, *f.line, f.cls}); it != idx.end()) {
                auto& a = *it->second;
                if (!a.extra.contains("supersedes")) {
                    a.extra["supersedes"] = "regex";
                    if (f.function) a.function = f.function;
                }
                continue;
            }
        }
        kept.push_back(std::move(f));
    }
    regex = std::move(kept);
}

bool is_layer_row(const Finding& f) { return f.status == laws::NOTRUN && f.extra.contains(std::string(LAYER_KEY)); }

std::vector<Finding> run_lints_ast(const std::vector<fs::path>& paths, const fs::path& root, const Config& cfg) {
    auto fe = pir::find_frontend(cfg);
    return run_lints_ast(paths, root, cfg, fe.clang ? fe.clang : fe.clangxx, Backend::Auto);
}

std::vector<Finding> run_lints_ast(const std::vector<fs::path>& paths, const fs::path& root, const Config& cfg,
                                   const std::optional<fs::path>& clang, Backend backend) {
    auto out = run_lints(paths, root, cfg.jobs);
    std::vector<fs::path> units;
    Headers headers;  // scanned project headers, canonical -> report path
    auto rel_of = [&](const fs::path& p) {
        std::error_code ec;
        if (fs::is_directory(root, ec)) {
            auto r = fs::relative(p, root, ec);
            return ec ? p.filename().string() : r.generic_string();
        }
        return p.filename().string();
    };
    for (auto& p : paths) {
        auto e = lower_ext(p);
        if (is_tu_ext(e)) units.push_back(p);
        else if (is_c_ext(e)) headers[detail::canon_path(p)] = rel_of(p);
    }
    auto layer_row = [&](std::string file, std::string msg) {
        Finding f;
        f.stage = "lints";
        f.status = std::string(laws::NOTRUN);
        f.file = std::move(file);
        f.message = std::move(msg);
        f.strength = std::string(laws::STRENGTH_FINDS);
        f.extra[std::string(LAYER_KEY)] = std::string(ENGINE);
        return f;
    };
    std::string lib_why;
    const bool lib = backend != Backend::Process && detail::libclang_load(clang, &lib_why);
    std::vector<Finding> ast, notes;
    std::set<std::string> headers_checked;
    std::string used;
    if (!units.empty() && ((backend == Backend::LibClang && !lib) || (!lib && !clang))) {
        auto f = layer_row("", (backend == Backend::LibClang ? lib_why : std::string("clang not found")) +
                                   ": the Clang-AST lints did not run on " + std::to_string(units.size()) +
                                   " translation unit(s); regex lints only");
        f.extra["install"] = kInstall;
        notes.push_back(std::move(f));
    } else if (!units.empty()) {
        std::vector<FileResult> res(units.size());
        const double timeout = std::max(30.0, cfg.timeout);
        parallel_for(cfg.jobs, units, [&](std::size_t i, const fs::path& p) {
            try {
                res[i] = run_file(clang, p, root, rel_of(p), timeout, lib ? Backend::LibClang : Backend::Process,
                                  headers);
            } catch (const std::exception& ex) {
                res[i] = FileResult{};
                res[i].error = std::string("internal error: ") + ex.what();
            }
        });
        std::set<std::tuple<std::string, int, std::string>> seen;  // a header's rows once
        for (std::size_t i = 0; i < units.size(); ++i) {
            if (!res[i].ran) {
                auto f = layer_row(rel_of(units[i]), "Clang-AST lints did not run: " + res[i].error + "; regex lints only");
                f.extra["install"] = kInstall;
                notes.push_back(std::move(f));
                continue;
            }
            if (used.empty()) used = res[i].backend;
            for (auto& h : res[i].headers) headers_checked.insert(h);
            for (auto& f : res[i].findings) {
                f.extra["ast_backend"] = res[i].backend;
                if (seen.insert({f.file, f.line.value_or(0), f.cls}).second) ast.push_back(std::move(f));
            }
        }
    }
    std::size_t unchecked = 0;
    for (auto& [c, r] : headers)
        if (!headers_checked.contains(r)) ++unchecked;
    if (unchecked > 0) {
        auto f = layer_row("", std::to_string(unchecked) + " of " + std::to_string(headers.size()) +
                                   " header file(s) declare nothing a parsed unit includes: the Clang-AST lints "
                                   "check a header through the units that include it; these get regex lints only");
        f.extra["files"] = std::to_string(unchecked);
        notes.push_back(std::move(f));
    }
    supersede(out, ast);
    out.insert(out.end(), ast.begin(), ast.end());
    out.insert(out.end(), notes.begin(), notes.end());
    return out;
}

}  // namespace prism::astlint
