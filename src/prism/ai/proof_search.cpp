// Lean proof search (roadmap 9.2). The prover model proposes tactic steps or
// whole proofs for one `sorry`; the Lean kernel decides.
//
//   1. The target is a theorem whose proof is exactly `sorry` / `by sorry`.
//   2. The project is copied to a scratch directory and the module is built
//      once (`lake build <Module>`), so imports resolve.
//   3. Best-first search: a node is a tactic prefix that Lean accepts with
//      only "unsolved goals" left. The model sees the statement, the prefix,
//      the goal state, the last Lean error of that node and lemmas from the
//      library, and proposes lines (grammar lean_proof.gbnf, re-validated).
//   4. Each candidate is elaborated by `lake env lean` with `#print axioms`
//      appended. Accepted only when there is no error, the axioms are within
//      {propext, Classical.choice, Quot.sound}, and `lake build <Module>` of
//      the spliced file succeeds. --write writes it back (and re-builds in
//      place; reverted if that fails) and appends it to the lemma library.
//
// Law 9: elaborating the project and model tactics runs code (Lean
// elaborators, the project's own macros), so `prism prove` needs
// --allow-exec and runs Lean in the bwrap jail when one is available.
// Every model call is in <out>/ai_audit.jsonl.

#include "ai_internal.hpp"

#include "prism/ai_proof.hpp"
#include "prism/laws.hpp"
#include "prism/sandbox.hpp"

#include "../proc.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#ifndef _WIN32
#  include <csignal>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace prism::ai {
namespace fs = std::filesystem;

namespace {

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_text(const fs::path& p, const std::string& s) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << s;
    return static_cast<bool>(out);
}

std::string env_or(const char* name, const std::string& dflt = {}) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : dflt;
}

// Same length as `text`; Lean comments (`-- ...`, nested `/- ... -/`) and
// string literals blanked, so searches for sorry / := / declarations only see code.
std::string mask_lean(const std::string& text) {
    std::string m = text;
    std::size_t i = 0, n = text.size();
    while (i < n) {
        if (text.compare(i, 2, "--") == 0) {
            while (i < n && text[i] != '\n') m[i++] = ' ';
            continue;
        }
        if (text.compare(i, 2, "/-") == 0) {
            int depth = 0;
            while (i < n) {
                if (text.compare(i, 2, "/-") == 0) {
                    ++depth;
                    m[i] = m[i + 1] = ' ';
                    i += 2;
                    continue;
                }
                if (text.compare(i, 2, "-/") == 0) {
                    m[i] = m[i + 1] = ' ';
                    i += 2;
                    if (--depth == 0) break;
                    continue;
                }
                if (text[i] != '\n') m[i] = ' ';
                ++i;
            }
            continue;
        }
        if (text[i] == '"') {
            m[i++] = ' ';
            while (i < n && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < n) m[i++] = ' ';
                if (text[i] != '\n') m[i] = ' ';
                ++i;
            }
            if (i < n) m[i++] = ' ';
            continue;
        }
        ++i;
    }
    return m;
}

bool ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '\'' ||
           (static_cast<unsigned char>(c) >= 0x80);
}

// Word occurrences of `w` in masked text between [a, b).
std::vector<std::size_t> word_hits(const std::string& m, const std::string& w, std::size_t a, std::size_t b) {
    std::vector<std::size_t> out;
    for (std::size_t p = m.find(w, a); p != std::string::npos && p < b; p = m.find(w, p + 1)) {
        bool left = p == 0 || !ident_char(m[p - 1]);
        bool right = p + w.size() >= m.size() || !ident_char(m[p + w.size()]);
        if (left && right) out.push_back(p);
    }
    return out;
}

const std::vector<std::string>& top_level_keywords() {
    static const std::vector<std::string> k{
        "theorem", "lemma",   "def",      "instance",  "example",     "structure", "inductive",
        "class",   "namespace", "end",    "section",   "open",        "abbrev",    "axiom",
        "noncomputable", "private", "protected", "variable", "attribute", "set_option", "@[",
        "#",       "/--",     "universe", "mutual",    "opaque",      "macro",     "syntax",
        "elab",    "deriving", "import",  "partial",   "unsafe",      "local",     "scoped"};
    return k;
}

// Byte offset where the declaration starting at `from` ends: the next line
// that begins at column 0 with a top-level keyword, or EOF.
std::size_t decl_end(const std::string& m, std::size_t from) {
    std::size_t p = m.find('\n', from);
    while (p != std::string::npos) {
        std::size_t line = p + 1;
        if (line >= m.size()) return m.size();
        char c = m[line];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            for (auto& kw : top_level_keywords()) {
                if (m.compare(line, kw.size(), kw) != 0) continue;
                auto after = line + kw.size();
                if (kw == "#" || kw == "@[" || kw == "/--" || after >= m.size() || !ident_char(m[after]))
                    return line;
            }
        }
        p = m.find('\n', line);
    }
    return m.size();
}

std::string module_of(const fs::path& project, const fs::path& file) {
    std::error_code ec;
    auto rel = fs::relative(file, project, ec);
    if (ec || rel.empty() || rel.string().rfind("..", 0) == 0 || rel.extension() != ".lean") return {};
    std::string out;
    for (auto& part : rel.replace_extension()) {
        if (!out.empty()) out += ".";
        out += part.string();
    }
    return out;
}

fs::path find_project(const fs::path& file) {
    std::error_code ec;
    for (auto d = fs::absolute(file, ec).parent_path(); !d.empty(); d = d.parent_path()) {
        if (fs::exists(d / "lakefile.toml", ec) || fs::exists(d / "lakefile.lean", ec)) return d;
        if (d == d.parent_path()) break;
    }
    return {};
}

}  // namespace

// ------------------------------------------------------------------ targets
std::optional<LeanTarget> find_lean_target(const fs::path& file, const std::string& theorem, std::string* why) {
    auto fail = [&](std::string m) -> std::optional<LeanTarget> {
        if (why) *why = std::move(m);
        return std::nullopt;
    };
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return fail("no such Lean file: " + file.string());
    auto text = read_text(file);
    auto m = mask_lean(text);
    std::vector<std::string> names{theorem};
    if (auto dot = theorem.rfind('.'); dot != std::string::npos) names.push_back(theorem.substr(dot + 1));
    for (auto& name : names) {
        for (auto* kw : {"theorem", "lemma"}) {
            for (auto kp : word_hits(m, kw, 0, m.size())) {
                auto p = kp + std::strlen(kw);
                while (p < m.size() && (m[p] == ' ' || m[p] == '\t' || m[p] == '\n')) ++p;
                if (m.compare(p, name.size(), name) != 0) continue;
                if (p + name.size() < m.size() && ident_char(m[p + name.size()])) continue;
                // Declaration start: beginning of the line (modifiers included).
                auto ls = m.rfind('\n', kp);
                ls = ls == std::string::npos ? 0 : ls + 1;
                auto end = decl_end(m, p);
                auto sorries = word_hits(m, "sorry", p, end);
                if (sorries.empty()) return fail("theorem " + name + " has no sorry to replace");
                if (sorries.size() > 1)
                    return fail("theorem " + name + " has " + std::to_string(sorries.size()) +
                                " sorry placeholders; the search replaces exactly one whole proof");
                // First := at bracket depth 0 after the name.
                int depth = 0;
                std::size_t assign = std::string::npos;
                for (auto i = p + name.size(); i + 1 < end; ++i) {
                    char c = m[i];
                    if (c == '(' || c == '[' || c == '{') {
                        ++depth;
                    } else if (c == ')' || c == ']' || c == '}') {
                        --depth;
                    } else if (depth == 0 && c == ':' && m[i + 1] == '=') {
                        assign = i;
                        break;
                    }
                }
                if (assign == std::string::npos || assign > sorries[0])
                    return fail("theorem " + name + ": no `:=` before the sorry");
                auto between = trim(m.substr(assign + 2, sorries[0] - assign - 2));
                if (!between.empty() && between != "by")
                    return fail("theorem " + name +
                                ": the sorry is not the whole proof (only `:= sorry` / `:= by sorry` are searched)");
                LeanTarget t;
                t.project = find_project(file);
                t.file = fs::absolute(file, ec);
                t.module = t.project.empty() ? std::string{} : module_of(t.project, t.file);
                t.theorem = name;
                t.statement = trim(text.substr(ls, assign - ls));
                t.decl_begin = ls;
                t.proof_begin = assign + 2;
                t.proof_end = sorries[0] + 5;
                return t;
            }
        }
    }
    return fail("theorem " + theorem + " not found in " + file.string());
}

std::vector<std::string> lean_sorry_theorems(const fs::path& file) {
    std::vector<std::string> out;
    auto text = read_text(file);
    auto m = mask_lean(text);
    for (auto* kw : {"theorem", "lemma"}) {
        for (auto kp : word_hits(m, kw, 0, m.size())) {
            auto p = kp + std::strlen(kw);
            while (p < m.size() && (m[p] == ' ' || m[p] == '\t')) ++p;
            auto q = p;
            while (q < m.size() && ident_char(m[q])) ++q;
            if (q == p) continue;
            if (!word_hits(m, "sorry", q, decl_end(m, q)).empty()) out.push_back(text.substr(p, q - p));
        }
    }
    return out;
}

// ------------------------------------------------------------------ validation
Validated validate_lean_tactics(const std::string& raw) {
    Validated v;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(trim(raw));
    } catch (...) {
        v.reason = "not JSON";
        return v;
    }
    if (!j.is_object() || j.size() != 1 || !j.contains("tactics") || !j["tactics"].is_array()) {
        v.reason = "expected {\"tactics\": [str, ...]}";
        return v;
    }
    auto& arr = j["tactics"];
    if (arr.empty() || arr.size() > 40) return (v.reason = "need 1..40 tactic lines", v);
    // Commands and escape hatches: nothing that is not an ordinary tactic
    // script may reach Lean (the axiom audit would catch sorry/native_decide,
    // but not a command that runs code while elaborating).
    static const std::vector<std::string> banned_words{
        "sorry", "admit", "native_decide", "set_option", "axiom", "unsafe", "implemented_by", "extern",
        "run_tac", "run_cmd", "run_elab", "by_elab", "elab", "elab_rules", "macro", "macro_rules", "syntax",
        "import", "initialize", "builtin_initialize", "theorem", "lemma", "def", "instance", "namespace",
        "section", "opaque", "ofReduceBool", "unsafeCast", "unsafeIO", "unsafeBaseIO", "IO", "EIO", "BaseIO",
        "Elab", "Meta", "Compiler", "trustCompiler", "decide!", "debug", "attribute", "export", "mutual",
        "universe", "variable", "open", "evalExpr", "Environment", "implementedBy"};
    for (auto& it : arr) {
        if (!it.is_string()) return (v.reason = "tactic line is not a string", v.items.clear(), v);
        auto line = it.get<std::string>();
        if (trim(line).empty() || line.size() > 400)
            return (v.reason = "tactic line empty or longer than 400 bytes", v.items.clear(), v);
        for (unsigned char c : line)
            if (c < 0x20 || c == 0x7f) return (v.reason = "control character in tactic line", v.items.clear(), v);
        if (line.find('#') != std::string::npos)
            return (v.reason = "'#' commands are not tactics", v.items.clear(), v);
        if (line.find("/-") != std::string::npos || line.find("-/") != std::string::npos)
            return (v.reason = "block comments are not allowed", v.items.clear(), v);
        if (line.find("@[") != std::string::npos)
            return (v.reason = "attributes are not tactics", v.items.clear(), v);
        auto m = mask_lean(line);
        // Tokens split at '.', so IO.println or Lean.Elab.Command hit too.
        {
            std::string tok;
            for (std::size_t k = 0; k <= m.size(); ++k) {
                char c = k < m.size() ? m[k] : ' ';
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '!' || c == '\'') {
                    tok += c;
                    continue;
                }
                for (auto& w : banned_words) {
                    if (tok == w) {
                        v.reason = "'" + w + "' is not allowed in a proof";
                        v.items.clear();
                        return v;
                    }
                }
                tok.clear();
            }
        }
        // `end` as a first token would close a namespace/section.
        auto t = trim(m);
        if (t == "end" || t.rfind("end ", 0) == 0) return (v.reason = "'end' is not a tactic", v.items.clear(), v);
        v.items.push_back(line);
    }
    v.ok = true;
    return v;
}

std::string splice_proof(const std::string& text, const LeanTarget& t, const std::vector<std::string>& tactics,
                         bool print_axioms) {
    std::string proof = " by\n";
    for (auto& line : tactics) {
        // Keep the model's relative indentation (focusing dots, nested blocks).
        std::string l = line;
        while (!l.empty() && (l.back() == ' ' || l.back() == '\r')) l.pop_back();
        proof += t.indent + l + "\n";
    }
    auto m = mask_lean(text);
    auto end = decl_end(m, t.proof_end);
    std::string rest_of_decl = text.substr(t.proof_end, end - t.proof_end);
    // The sorry's line remainder (usually just "\n") follows the new block.
    auto first_nl = rest_of_decl.find('\n');
    std::string tail_line = first_nl == std::string::npos ? rest_of_decl : rest_of_decl.substr(0, first_nl);
    std::string after = first_nl == std::string::npos ? std::string{} : rest_of_decl.substr(first_nl + 1);
    std::string out = text.substr(0, t.proof_begin) + proof;
    if (!trim(tail_line).empty()) out += t.indent + trim(tail_line) + "\n";
    out += after;
    if (print_axioms) {
        if (!out.empty() && out.back() != '\n') out += "\n";
        out += "#print axioms " + t.theorem + "\n";
    }
    out += text.substr(end);
    return out;
}

// ------------------------------------------------------------------ Lean output
LeanCheck parse_lean_output(const std::string& out, const std::string& theorem, int rc) {
    LeanCheck c;
    c.ran = true;
    c.log = out;
    static const std::regex head(R"(^(?:(?:error|warning|info): )?(.*?):(\d+):(\d+): (error|warning|info): ?(.*)$)");
    static const std::regex ax(R"('([^']+)' depends on axioms: \[(.*)\])");
    static const std::regex noax(R"('([^']+)' does not depend on any axioms)");
    std::istringstream in(out);
    std::string line;
    struct Msg {
        std::string sev, text;
    };
    std::vector<Msg> msgs;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch mm;
        if (std::regex_search(line, mm, ax)) {
            auto name = mm[1].str();
            if (name == theorem || name.ends_with("." + theorem)) {
                c.axioms_seen = true;
                std::stringstream as(mm[2].str());
                std::string a;
                while (std::getline(as, a, ',')) {
                    a = trim(a);
                    if (!a.empty()) c.axioms.push_back(a);
                }
            }
            continue;
        }
        if (std::regex_search(line, mm, noax)) {
            auto name = mm[1].str();
            if (name == theorem || name.ends_with("." + theorem)) c.axioms_seen = true;
            continue;
        }
        if (std::regex_match(line, mm, head)) {
            msgs.push_back({mm[4].str(), mm[5].str()});
            continue;
        }
        if (!msgs.empty()) msgs.back().text += "\n" + line;
    }
    int errors = 0, unsolved = 0;
    std::string first_error;
    for (auto& m : msgs) {
        if (m.sev != "error") continue;
        ++errors;
        auto body = trim(m.text);
        if (body.rfind("unsolved goals", 0) == 0) {
            ++unsolved;
            auto g = trim(body.substr(14));
            if (!c.goals.empty()) c.goals += "\n\n";
            c.goals += g;
        } else if (first_error.empty()) {
            first_error = body;
        }
        if (!c.errors.empty()) c.errors += "\n";
        c.errors += body;
    }
    if (c.goals.empty() && !first_error.empty()) {
        // Lean prints the goal under most tactic errors: keep it as context.
        auto p = first_error.find("\n");
        if (p != std::string::npos) c.goals = trim(first_error.substr(p + 1));
    }
    for (std::size_t p = c.goals.find("⊢"); p != std::string::npos; p = c.goals.find("⊢", p + 1))
        ++c.goal_count;
    if (c.errors.size() > 4000) c.errors = c.errors.substr(0, 4000) + "\n...";
    if (c.goals.size() > 4000) c.goals = c.goals.substr(0, 4000) + "\n...";
    // Another theorem's sorry in the same file is only a warning; this
    // theorem's own soundness is the axiom audit's job (sorryAx).
    c.complete = rc == 0 && errors == 0;
    c.only_unsolved = errors > 0 && errors == unsolved;
    c.axioms_ok = c.axioms_seen;
    for (auto& a : c.axioms) {
        bool ok = false;
        for (auto* allowed : LEAN_ALLOWED_AXIOMS)
            if (a == allowed) ok = true;
        if (!ok) c.axioms_ok = false;
    }
    return c;
}

// ------------------------------------------------------------------ lemma library
std::vector<LemmaEntry> read_lemmas(const fs::path& path) {
    std::vector<LemmaEntry> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (trim(line).empty()) continue;
        try {
            auto j = nlohmann::json::parse(line);
            LemmaEntry e;
            e.theorem = j.value("theorem", "");
            e.file = j.value("file", "");
            e.statement = j.value("statement", "");
            e.proof = j.value("proof", "");
            e.model = j.value("model", "");
            e.audit_id = j.value("audit_id", "");
            if (!e.theorem.empty() && !e.proof.empty()) out.push_back(std::move(e));
        } catch (...) {
            // A corrupt line is skipped; the library is advisory prompt context only.
        }
    }
    return out;
}

void append_lemma(const fs::path& path, const LemmaEntry& e) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    nlohmann::json j{{"theorem", e.theorem},     {"file", e.file},
                     {"statement", e.statement}, {"proof", e.proof},
                     {"model", e.model},         {"audit_id", e.audit_id},
                     {"checked", "lean kernel + #print axioms + lake build"}};
    std::ofstream(path, std::ios::app | std::ios::binary) << j.dump() << "\n";
}

// ------------------------------------------------------------------ tools
std::optional<fs::path> find_lake() {
    std::error_code ec;
    auto env = env_or("PRISM_LAKE");
    if (!env.empty()) {
        if (fs::is_regular_file(env, ec)) return fs::path(env);
        return std::nullopt;  // an explicit but missing PRISM_LAKE is not silently replaced
    }
    if (auto p = Config{}.which({"lake"})) return *p;
    auto home = env_or("HOME");
    if (!home.empty() && fs::is_regular_file(fs::path(home) / ".elan" / "bin" / "lake", ec))
        return fs::path(home) / ".elan" / "bin" / "lake";
    return std::nullopt;
}

namespace {

// Environment for lake: elan's bin directory on PATH so `lean` resolves.
struct PathGuard {
    std::string old;
    bool set = false;
    explicit PathGuard(const fs::path& lake) {
#ifndef _WIN32
        old = env_or("PATH");
        auto dir = lake.parent_path().string();
        if (old.find(dir) == std::string::npos) {
            ::setenv("PATH", (dir + ":" + old).c_str(), 1);
            set = true;
        }
#endif
    }
    ~PathGuard() {
#ifndef _WIN32
        if (set) ::setenv("PATH", old.c_str(), 1);
#endif
    }
};

detail::ProcOut run_lake(const fs::path& lake, const std::vector<std::string>& args, const fs::path& cwd,
                         double timeout, const fs::path& writable) {
    std::vector<std::string> argv{lake.string()};
    argv.insert(argv.end(), args.begin(), args.end());
    return detail::run_process(sandbox::wrap_argv(argv, writable), timeout, cwd);
}

bool copy_project(const fs::path& from, const fs::path& to, std::string* why) {
    std::error_code ec;
    fs::create_directories(to, ec);
    for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        auto rel = fs::relative(it->path(), from, ec);
        auto name = it->path().filename().string();
        if (it->is_directory(ec)) {
            // Nested lake projects and VCS data are not part of this package.
            bool nested = it->path() != from && (fs::exists(it->path() / "lakefile.toml", ec) ||
                                                 fs::exists(it->path() / "lakefile.lean", ec));
            if (name == ".git" || nested) {
                it.disable_recursion_pending();
                continue;
            }
            fs::create_directories(to / rel, ec);
            continue;
        }
        if (it->is_symlink(ec)) continue;
        fs::copy_file(it->path(), to / rel, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            if (why) *why = "copy " + it->path().string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- prover backends
class ProverServer final : public ModelBackend {
public:
    ProverServer(std::string url, std::string model) : url_(std::move(url)), model_(std::move(model)) {}
    std::string name() const override { return "llama-server:" + model_; }
    std::string model_sha256() const override { return sha_.empty() ? "unknown" : sha_; }
    void set_sha(std::string s) { sha_ = std::move(s); }
    ModelReply complete(const ModelRequest& req) override {
        nlohmann::json body{{"prompt", req.system + "\n\n" + req.user + "\n"},
                            {"n_predict", 1024},
                            {"temperature", 0.6},
                            {"cache_prompt", false},
                            {"grammar", req.grammar_text}};
        auto resp = http_request_raw("POST", url_ + "/completion", body.dump(), 300000);
        if (!resp) return {"", "HTTP error (prover llama-server /completion)"};
        try {
            auto j = nlohmann::json::parse(*resp);
            return {j.value("content", ""), ""};
        } catch (const std::exception& ex) {
            return {"", std::string("bad prover reply: ") + ex.what()};
        }
    }

private:
    std::string url_, model_, sha_;
};

// A llama-server this process started for the prover GGUF (swapped out at exit).
struct SpawnedServer {
    int pid = -1;
    ~SpawnedServer() {
#ifndef _WIN32
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int st = 0;
            for (int i = 0; i < 50 && ::waitpid(pid, &st, WNOHANG) == 0; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &st, WNOHANG);
        }
#endif
    }
};

std::vector<fs::path> default_prover_ggufs() {
    std::vector<fs::path> out;
    auto home = env_or("HOME");
    if (home.empty()) return out;
    std::error_code ec;
    auto dir = fs::path(home) / ".prism" / "models";
    if (!fs::is_directory(dir, ec)) return out;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        auto n = e.path().filename().string();
        std::string low;
        for (char c : n) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (e.path().extension() == ".gguf" && low.find("prover") != std::string::npos) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::shared_ptr<ModelBackend> resolve_prover(const ProveOptions& opt, SpawnedServer& spawned, std::string* why) {
    if (opt.backend) return opt.backend;
    auto model = !opt.prover_model.empty() ? opt.prover_model : env_or("PRISM_PROVER_MODEL", "prover");
    auto url = !opt.prover_server.empty() ? opt.prover_server : env_or("PRISM_PROVER_SERVER");
    if (!url.empty()) {
        if (http_request_raw("GET", url + "/health", {}, 2000) || http_request_raw("GET", url + "/v1/models", {}, 2000))
            return std::make_shared<ProverServer>(url, model);
        if (why) *why = "prover server " + url + " not reachable";
        return nullptr;
    }
    fs::path gguf = !opt.prover_gguf.empty() ? opt.prover_gguf : fs::path(env_or("PRISM_PROVER_GGUF"));
    if (gguf.empty()) {
        auto found = default_prover_ggufs();
        if (!found.empty()) gguf = found.front();
    }
    std::error_code ec;
    if (gguf.empty() || !fs::is_regular_file(gguf, ec)) {
        if (why)
            *why = "no prover model: set PRISM_PROVER_SERVER (llama-server URL) or PRISM_PROVER_GGUF (a "
                   "DeepSeek-Prover / Goedel-Prover / Kimina-Prover GGUF), or put one in ~/.prism/models"
                   + (gguf.empty() ? std::string{} : " (missing: " + gguf.string() + ")");
        return nullptr;
    }
#ifdef _WIN32
    if (why) *why = "spawning llama-server for the prover GGUF is POSIX only; set PRISM_PROVER_SERVER";
    return nullptr;
#else
    auto bin_env = env_or("PRISM_LLAMA_SERVER_BIN");
    std::optional<fs::path> bin = bin_env.empty() ? Config{}.which({"llama-server"}) : std::optional<fs::path>(bin_env);
    if (!bin || !fs::is_regular_file(*bin, ec)) {
        if (why) *why = "prover GGUF " + gguf.string() + " found but no llama-server binary (PATH or PRISM_LLAMA_SERVER_BIN)";
        return nullptr;
    }
    int port = 18000 + static_cast<int>(::getpid() % 2000);
    auto port_s = std::to_string(port);
    pid_t pid = ::fork();
    if (pid == 0) {
        auto m = gguf.string();
        auto b = bin->string();
        ::execl(b.c_str(), b.c_str(), "-m", m.c_str(), "--host", "127.0.0.1", "--port", port_s.c_str(), "-c",
                "8192", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    if (pid < 0) {
        if (why) *why = "fork failed for llama-server";
        return nullptr;
    }
    spawned.pid = pid;
    auto base = "http://127.0.0.1:" + port_s;
    for (int i = 0; i < 600; ++i) {
        int st = 0;
        if (::waitpid(pid, &st, WNOHANG) == pid) {
            spawned.pid = -1;
            if (why) *why = "llama-server exited while loading " + gguf.string();
            return nullptr;
        }
        if (http_request_raw("GET", base + "/health", {}, 1000)) {
            auto be = std::make_shared<ProverServer>(base, model.empty() || model == "prover"
                                                               ? gguf.filename().string()
                                                               : model);
            be->set_sha(sha256_file(gguf));
            return be;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (why) *why = "llama-server did not become healthy within 300 s";
    return nullptr;
#endif
}

std::vector<LemmaEntry> relevant_lemmas(const std::vector<LemmaEntry>& lib, const std::string& statement, std::size_t k) {
    auto words = [](const std::string& s) {
        std::set<std::string> w;
        std::string cur;
        for (char c : s) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.') cur += c;
            else {
                if (cur.size() > 2) w.insert(cur);
                cur.clear();
            }
        }
        if (cur.size() > 2) w.insert(cur);
        return w;
    };
    auto target = words(statement);
    std::vector<std::pair<int, std::size_t>> scored;
    for (std::size_t i = 0; i < lib.size(); ++i) {
        int s = 0;
        for (auto& w : words(lib[i].statement))
            if (target.count(w)) ++s;
        scored.push_back({-s, lib.size() - i});  // more overlap, then newer first
    }
    std::sort(scored.begin(), scored.end());
    std::vector<LemmaEntry> out;
    for (std::size_t i = 0; i < scored.size() && out.size() < k; ++i) out.push_back(lib[lib.size() - scored[i].second]);
    return out;
}

fs::path default_lemma_path(const fs::path& project) {
    // proofs/, proofs/semantics and proofs/techniques share proofs/lemmas.jsonl.
    for (auto d = project; !d.empty(); d = d.parent_path()) {
        if (d.filename() == "proofs") return d / "lemmas.jsonl";
        if (d == d.parent_path()) break;
    }
    return project / "lemmas.jsonl";
}

std::string join_lines(const std::vector<std::string>& v) {
    std::string s;
    for (auto& l : v) s += l + "\n";
    return s;
}

}  // namespace

// ------------------------------------------------------------------ search
ProveResult prove_theorem(const ProveOptions& opt) {
    ProveResult res;
    res.status = std::string(laws::ERROR);
    std::string why;
    auto lake = find_lake();
    if (!lake) {
        res.status = std::string(laws::NOTRUN);
        res.reason = "Lean not installed (lake not found: install elan, or set PRISM_LAKE)";
        return res;
    }
    auto target = find_lean_target(opt.file, opt.theorem, &why);
    if (!target) {
        res.reason = why;
        return res;
    }
    auto t = *target;
    if (!opt.project.empty()) {
        std::error_code ec;
        t.project = fs::absolute(opt.project, ec);
        t.module = module_of(t.project, t.file);
    }
    if (t.project.empty()) {
        res.reason = "no lakefile.toml / lakefile.lean above " + t.file.string();
        return res;
    }
    if (t.module.empty()) {
        res.reason = t.file.string() + " is not a module of the lake package at " + t.project.string() +
                     " (acceptance needs `lake build <Module>`)";
        return res;
    }
    // Audit log: reuse an open session, else open one that appends.
    std::optional<Session> own_session;
    if (!session_config()) {
        Config c = default_config();
        c.out = opt.out;
        c.resume = true;  // append to ai_audit.jsonl, never truncate a pipeline run's log
        own_session.emplace(c);
    }
    SpawnedServer spawned;
    auto backend = resolve_prover(opt, spawned, &why);
    if (!backend) {
        res.status = std::string(laws::NOTRUN);
        res.reason = why;
        return res;
    }
    res.model = backend->name();
    PathGuard path_guard(*lake);

    // Scratch copy of the package (the source tree is never touched unless --write).
    auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto scratch = fs::temp_directory_path() / ("prism-prove-" + sha256_hex(t.file.string() + stamp).substr(0, 12));
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
    } cleanup{scratch};
    if (!copy_project(t.project, scratch, &why)) {
        res.reason = why;
        return res;
    }
    auto rel_file = fs::relative(t.file, t.project);
    auto work_file = scratch / rel_file;
    const auto original = read_text(t.file);
    const auto original_sha = sha256_hex(original);

    auto base = run_lake(*lake, {"build", t.module}, scratch, opt.lean_timeout * 4, scratch);
    if (base.failed) {
        res.status = std::string(laws::NOTRUN);
        res.reason = "lake could not be started";
        return res;
    }
    if (base.rc != 0) {
        res.reason = "the project does not build before the search (lake build " + t.module + "): " +
                     trim(base.text).substr(0, 1500);
        return res;
    }

    auto check = [&](const std::vector<std::string>& tactics) {
        write_text(work_file, splice_proof(original, t, tactics, true));
        auto r = run_lake(*lake, {"env", "lean", rel_file.string()}, scratch, opt.lean_timeout, scratch);
        ++res.kernel_checks;
        if (r.failed || r.timed_out) {
            LeanCheck c;
            c.ran = !r.failed;
            c.errors = r.timed_out ? "lean timed out" : "lean could not be started";
            return c;
        }
        return parse_lean_output(r.text, t.theorem, r.rc);
    };

    // Root: the goal of the theorem (tactic `skip` leaves it unsolved).
    auto root_check = check({"skip"});
    ProveNode root;
    root.goals = root_check.goals;
    root.goal_count = root_check.goal_count;
    if (!root_check.only_unsolved) root.last_error = root_check.errors;

    auto lemma_path = !opt.lemmas.empty() ? opt.lemmas : default_lemma_path(t.project);
    auto lemmas = relevant_lemmas(read_lemmas(lemma_path), t.statement, 8);

    struct Entry {
        ProveNode node;
        int expansions = 0;
    };
    std::vector<Entry> frontier{{root, 0}};
    std::set<std::string> seen{join_lines({})};
    // Lower is better: deeper accepted prefixes first (they made progress),
    // fewer / shorter goals, and nodes already expanded fall behind.
    auto score = [](const Entry& e) {
        return e.node.goal_count * 200 + static_cast<int>(e.node.goals.size() / 20) + e.expansions * 1500 -
               e.node.depth * 300;
    };

    for (int call = 0; call < opt.budget && !frontier.empty(); ++call) {
        std::sort(frontier.begin(), frontier.end(),
                  [&](const Entry& a, const Entry& b) { return score(a) < score(b); });
        if (static_cast<int>(frontier.size()) > opt.beam) frontier.resize(static_cast<std::size_t>(opt.beam));
        auto& cur = frontier.front();
        cur.expansions++;
        auto node = cur.node;

        std::string user = "Prove this Lean 4 theorem (core Lean 4, no Mathlib unless the file imports it).\n";
        user += fence_untrusted(t.statement + " := by\n  sorry", "LEAN THEOREM");
        user += "\nFile imports and context:\n";
        {
            auto head = original.substr(0, t.decl_begin);
            if (head.size() > 3000) head = head.substr(head.size() - 3000);
            user += fence_untrusted(head, "LEAN CONTEXT");
        }
        if (!node.tactics.empty()) {
            user += "\nThese tactic lines are already accepted by Lean (continue after them; reply only the NEW lines):\n";
            user += fence_untrusted(join_lines(node.tactics), "ACCEPTED PREFIX");
        }
        if (!node.goals.empty()) user += "\nCurrent goal state:\n" + fence_untrusted(node.goals, "GOALS");
        if (!node.last_error.empty())
            user += "\nThe previous attempt from this state failed with this Lean error:\n" +
                    fence_untrusted(node.last_error, "LEAN ERROR");
        if (!lemmas.empty()) {
            user += "\nLemma library (proved earlier by PRISM; statements and proofs you may reuse as patterns):\n";
            std::string lib;
            for (auto& l : lemmas) lib += l.statement + " := by\n  " + l.proof + "\n\n";
            user += fence_untrusted(lib, "LEMMA LIBRARY");
        }
        user += "\nReply with {\"tactics\": [...]}: tactic lines only (no `theorem`, no `by`), either the whole "
                "rest of the proof or the next step.";

        ModelRequest req;
        req.feature = "lean-proof";
        req.system = system_prompt(
            "You write Lean 4 tactic proofs. The Lean kernel checks every line you propose; a proof "
            "is accepted only if it closes every goal, uses no axiom beyond propext, Classical.choice "
            "and Quot.sound, and `lake build` succeeds. sorry, admit, native_decide and commands are rejected.");
        req.user = user;
        req.grammar = "lean_proof";
        req.grammar_text = grammar_text("lean_proof");
        AuditRecord rec;
        rec.function = t.theorem;
        rec.file = t.file.string();
        rec.checker = "lean-kernel(lake env lean)+#print axioms+lake build";
        auto reply = ask(*backend, req, rec);
        ++res.model_calls;
        if (!reply.error.empty()) {
            rec.checker_result = "no output: " + reply.error;
            audit_append(rec);
            res.attempts.push_back("model error: " + reply.error);
            continue;
        }
        auto v = validate_lean_tactics(reply.text);
        rec.output_valid = v.ok;
        if (!v.ok) {
            ++res.rejected_invalid;
            rec.rejected_reason = v.reason;
            rec.checker = "grammar-validator";
            rec.checker_result = "rejected";
            audit_append(rec);
            res.attempts.push_back("rejected (validator): " + v.reason);
            cur.node.last_error = "Your output was rejected before reaching Lean: " + v.reason;
            continue;
        }
        auto cand = node.tactics;
        cand.insert(cand.end(), v.items.begin(), v.items.end());
        auto key = join_lines(cand);
        if (seen.count(key)) {
            rec.checker_result = "duplicate candidate (already checked)";
            audit_append(rec);
            res.attempts.push_back("duplicate");
            continue;
        }
        seen.insert(key);
        auto c = check(cand);
        if (c.complete) {
            if (!c.axioms_ok) {
                ++res.rejected_axioms;
                std::string ax = c.axioms_seen ? join(c.axioms, ", ") : "no #print axioms report";
                rec.checker_result = "rejected: axioms [" + ax + "]";
                audit_append(rec);
                res.attempts.push_back("rejected (axioms): " + ax);
                cur.node.last_error = "Lean accepted the proof but it depends on axioms outside propext, "
                                      "Classical.choice, Quot.sound: " + ax;
                continue;
            }
            // Acceptance gate: lake build of the module with the proof in place.
            write_text(work_file, splice_proof(original, t, cand, false));
            auto b = run_lake(*lake, {"build", t.module}, scratch, opt.lean_timeout * 4, scratch);
            if (b.rc != 0 || b.failed || b.timed_out) {
                rec.checker_result = "rejected: lake build failed";
                audit_append(rec);
                res.attempts.push_back("rejected (lake build): " + trim(b.text).substr(0, 200));
                cur.node.last_error = "lake build failed: " + trim(b.text).substr(0, 1500);
                continue;
            }
            rec.checker_result = "accepted";
            rec.verdict_effect = std::string(laws::PROVED);
            audit_append(rec);
            res.audit_id = rec.id;
            res.attempts.push_back("accepted");
            res.proof = cand;
            res.axioms = c.axioms;
            res.status = std::string(laws::PROVED);
            res.reason = "Lean kernel accepted the proof; axioms within {propext, Classical.choice, Quot.sound}; "
                         "lake build " + t.module + " succeeded";
            if (opt.write) {
                if (sha256_hex(read_text(t.file)) != original_sha) {
                    res.reason += "; NOT written: " + t.file.string() + " changed during the search";
                } else {
                    auto final_text = splice_proof(original, t, cand, false);
                    write_text(t.file, final_text);
                    auto inplace = run_lake(*lake, {"build", t.module}, t.project, opt.lean_timeout * 4, t.project);
                    if (inplace.rc != 0) {
                        write_text(t.file, original);
                        res.status = std::string(laws::ERROR);
                        res.reason = "proof checked in the scratch copy but lake build in place failed; file "
                                     "restored: " + trim(inplace.text).substr(0, 500);
                        return res;
                    }
                    res.written = true;
                    LemmaEntry e;
                    e.theorem = t.theorem;
                    std::error_code ec;
                    e.file = fs::relative(t.file, lemma_path.parent_path(), ec).generic_string();
                    e.statement = t.statement;
                    std::string pr;
                    for (std::size_t i = 0; i < cand.size(); ++i) pr += (i ? "\n  " : "") + cand[i];
                    e.proof = pr;
                    e.model = backend->name();
                    e.audit_id = rec.id;
                    append_lemma(lemma_path, e);
                }
            }
            return res;
        }
        if (c.only_unsolved && !v.items.empty()) {
            rec.checker_result = "partial: " + std::to_string(c.goal_count) + " goal(s) left";
            audit_append(rec);
            res.attempts.push_back(rec.checker_result);
            ProveNode child;
            child.tactics = cand;
            child.goals = c.goals;
            child.goal_count = c.goal_count;
            child.depth = node.depth + 1;
            frontier.push_back({child, 0});
            continue;
        }
        ++res.rejected_kernel;
        auto first = c.errors.substr(0, c.errors.find('\n'));
        rec.checker_result = "rejected: " + (c.errors.empty() ? std::string("lean error") : first.substr(0, 200));
        audit_append(rec);
        res.attempts.push_back("rejected (kernel): " + first.substr(0, 200));
        cur.node.last_error = c.errors;
    }
    res.status = std::string(laws::UNKNOWN);
    res.reason = "search budget exhausted (" + std::to_string(res.model_calls) + " model calls, " +
                 std::to_string(res.kernel_checks) + " Lean checks); no candidate passed the kernel, the axiom "
                 "audit and lake build";
    return res;
}

// ------------------------------------------------------------------ CLI
int prove_main(int argc, char** argv) {
    ProveOptions opt;
    bool allow_exec = false, json_only = false, list = false;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--write") opt.write = true;
        else if (a == "--allow-exec") allow_exec = true;
        else if (a == "--json") json_only = true;
        else if (a == "--list") list = true;
        else if (a == "--project") opt.project = next();
        else if (a == "--lemmas") opt.lemmas = next();
        else if (a == "--budget") opt.budget = std::max(1, std::atoi(next().c_str()));
        else if (a == "--beam") opt.beam = std::max(1, std::atoi(next().c_str()));
        else if (a == "--timeout") opt.lean_timeout = std::max(5.0, std::atof(next().c_str()));
        else if (a == "--out") opt.out = next();
        else if (a == "--prover-server") opt.prover_server = next();
        else if (a == "--prover-gguf") opt.prover_gguf = next();
        else if (a == "--prover-model") opt.prover_model = next();
        else if (a == "-h" || a == "--help") {
            std::cout << "prism prove FILE.lean THEOREM [--write] [--allow-exec] [--project DIR]\n"
                         "            [--lemmas PATH] [--budget N] [--beam N] [--timeout S] [--out DIR]\n"
                         "            [--prover-server URL] [--prover-gguf PATH] [--prover-model NAME] [--json]\n"
                         "prism prove FILE.lean --list      theorems with a sorry\n"
                         "Lean proof search (roadmap 9.2): the prover model proposes tactics, the Lean kernel\n"
                         "checks each one, #print axioms must stay within propext/Classical.choice/Quot.sound,\n"
                         "and lake build of the module gates acceptance. --write writes the proof back and adds\n"
                         "it to the lemma library (default proofs/lemmas.jsonl). Needs --allow-exec (Law 9:\n"
                         "elaborating Lean runs code). Prover: PRISM_PROVER_SERVER or PRISM_PROVER_GGUF.\n"
                         "Exit 0 proved, 1 not proved, 3 NOTRUN, 2 error.\n";
            return 0;
        } else if (!a.starts_with("-")) pos.push_back(a);
        else {
            std::cerr << "prism prove: unknown option " << a << "\n";
            return 2;
        }
    }
    if (list && !pos.empty()) {
        for (auto& n : lean_sorry_theorems(pos[0])) std::cout << n << "\n";
        return 0;
    }
    if (pos.size() != 2) {
        std::cerr << "usage: prism prove FILE.lean THEOREM [--write] [--allow-exec] (see --help)\n";
        return 2;
    }
    opt.file = fs::absolute(pos[0]);
    opt.theorem = pos[1];
    opt.out = fs::absolute(opt.out);
    ProveResult r;
    if (!allow_exec) {
        r.status = std::string(laws::NOTRUN);
        r.reason = sandbox::exec_message("prove (Lean elaboration of the project and of model tactics)");
    } else {
        sandbox::Policy policy(true);
        r = prove_theorem(opt);
    }
    nlohmann::json j{{"theorem", opt.theorem},
                     {"file", opt.file.string()},
                     {"status", r.status},
                     {"reason", r.reason},
                     {"proof", r.proof},
                     {"axioms", r.axioms},
                     {"written", r.written},
                     {"model", r.model},
                     {"model_calls", r.model_calls},
                     {"kernel_checks", r.kernel_checks},
                     {"rejected_invalid", r.rejected_invalid},
                     {"rejected_kernel", r.rejected_kernel},
                     {"rejected_axioms", r.rejected_axioms},
                     {"attempts", r.attempts},
                     {"ai_audit_id", r.audit_id}};
    std::error_code ec;
    fs::create_directories(opt.out / "prove", ec);
    std::ofstream(opt.out / "prove" / (opt.theorem + ".json")) << j.dump(2) << "\n";
    if (json_only) {
        std::cout << j.dump(2) << "\n";
    } else {
        std::cout << r.status << " " << opt.theorem << ": " << r.reason << "\n";
        for (auto& a : r.attempts) std::cout << "  " << a << "\n";
        if (!r.proof.empty()) {
            std::cout << "proof:\n";
            for (auto& l : r.proof) std::cout << "  " << l << "\n";
        }
    }
    if (r.status == laws::PROVED) return 0;
    if (r.status == laws::NOTRUN) return 3;
    if (r.status == laws::ERROR) return 2;
    return 1;
}

}  // namespace prism::ai
