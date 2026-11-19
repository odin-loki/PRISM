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

#include "prism/exec.hpp"
#include "prism/forge_llm.hpp"
#include "prism/remote_llm.hpp"
#include "prism/exit.hpp"
#include "prism/clock.hpp"
#include "ai_internal.hpp"

#include "prism/log.hpp"
#include "prism/ai_proof.hpp"
#include "prism/laws.hpp"
#include "prism/sandbox.hpp"

#include "prism/exec.hpp"
#include "prism/sandbox.hpp"
#include "prism/pool.hpp"
#include "prism/threads.hpp"

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
#include <utility>

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
    if (!out.is_open()) return false;
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
            while (i < n && text.at(i) != '\n') {
                m.at(i) = ' ';
                ++i;
            }
            continue;
        }
        if (text.compare(i, 2, "/-") == 0) {
            int depth = 0;
            while (i < n) {
                if (text.compare(i, 2, "/-") == 0) {
                    ++depth;
                    m.at(i) = m.at(i + 1) = ' ';
                    i += 2;
                    continue;
                }
                if (text.compare(i, 2, "-/") == 0) {
                    m.at(i) = m.at(i + 1) = ' ';
                    i += 2;
                    if (--depth == 0) break;
                    continue;
                }
                if (text.at(i) != '\n') m.at(i) = ' ';
                ++i;
            }
            continue;
        }
        if (text.at(i) == '"') {
            m.at(i) = ' ';
            ++i;
            while (i < n && text.at(i) != '"') {
                if (text.at(i) == '\\' && i + 1 < n) {
                    m.at(i) = ' ';
                    ++i;
                }
                if (text.at(i) != '\n') m.at(i) = ' ';
                ++i;
            }
            if (i < n) {
                m.at(i) = ' ';
                ++i;
            }
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
        bool left = p == 0 || !ident_char(m.at(p - 1));
        bool right = p + w.size() >= m.size() || !ident_char(m.at(p + w.size()));
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
        char c = m.at(line);
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            for (auto& kw : top_level_keywords()) {
                if (m.compare(line, kw.size(), kw) != 0) continue;
                auto after = line + kw.size();
                if (kw == "#" || kw == "@[" || kw == "/--" || after >= m.size() || !ident_char(m.at(after)))
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
    auto fail = [&](std::string msg) -> std::optional<LeanTarget> {
        if (why) *why = std::move(msg);
        return std::nullopt;
    };
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return fail("no such Lean file: " + file.string());
    auto text = read_text(file);
    auto masked = mask_lean(text);
    std::vector<std::string> names{theorem};
    if (auto dot = theorem.rfind('.'); dot != std::string::npos) names.push_back(theorem.substr(dot + 1));
    for (auto& name : names) {
        for (auto* kw : {"theorem", "lemma"}) {
            for (auto kp : word_hits(masked, kw, 0, masked.size())) {
                auto p = kp + std::strlen(kw);
                while (p < masked.size() && (masked[p] == ' ' || masked[p] == '\t' || masked[p] == '\n')) ++p;
                if (masked.compare(p, name.size(), name) != 0) continue;
                if (p + name.size() < masked.size() && ident_char(masked[p + name.size()])) continue;
                // Declaration start: beginning of the line (modifiers included).
                auto ls = masked.rfind('\n', kp);
                ls = ls == std::string::npos ? 0 : ls + 1;
                auto end = decl_end(masked, p);
                auto sorries = word_hits(masked, "sorry", p, end);
                if (sorries.empty()) return fail("theorem " + name + " has no sorry to replace");
                if (sorries.size() > 1)
                    return fail("theorem " + name + " has " + std::to_string(sorries.size()) +
                                " sorry placeholders; the search replaces exactly one whole proof");
                // First := at bracket depth 0 after the name.
                int depth = 0;
                std::size_t assign = std::string::npos;
                for (auto i = p + name.size(); i + 1 < end; ++i) {
                    char c = masked.at(i);
                    if (c == '(' || c == '[' || c == '{') {
                        ++depth;
                    } else if (c == ')' || c == ']' || c == '}') {
                        --depth;
                    } else if (depth == 0 && c == ':' && masked.at(i + 1) == '=') {
                        assign = i;
                        break;
                    }
                }
                if (assign == std::string::npos || assign > sorries[0])
                    return fail("theorem " + name + ": no `:=` before the sorry");
                auto between = trim(masked.substr(assign + 2, sorries[0] - assign - 2));
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
    // (offset, name): declarations in source order, theorems and lemmas mixed
    std::vector<std::pair<std::size_t, std::string>> hits;
    auto text = read_text(file);
    auto m = mask_lean(text);
    for (auto* kw : {"theorem", "lemma"}) {
        for (auto kp : word_hits(m, kw, 0, m.size())) {
            auto p = kp + std::strlen(kw);
            while (p < m.size() && (m[p] == ' ' || m[p] == '\t')) ++p;
            auto q = p;
            while (q < m.size() && ident_char(m[q])) ++q;
            if (q == p) continue;
            if (!word_hits(m, "sorry", q, decl_end(m, q)).empty()) hits.emplace_back(kp, text.substr(p, q - p));
        }
    }
    std::sort(hits.begin(), hits.end());
    std::vector<std::string> out;
    for (auto& h : hits) out.push_back(std::move(h.second));
    return out;
}

std::vector<std::pair<fs::path, std::string>> lean_sorry_targets(const std::vector<fs::path>& roots) {
    std::set<fs::path> files;
    std::error_code ec;
    for (const auto& r : roots) {
        const auto base = fs::weakly_canonical(fs::absolute(r, ec), ec);
        if (!fs::is_directory(base, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(base, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (it->is_directory(ec) && it->path().filename() == ".lake") {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(ec) && it->path().extension() == ".lean") files.insert(it->path());
        }
    }
    std::vector<std::pair<fs::path, std::string>> out;
    for (const auto& f : files)
        for (auto& t : lean_sorry_theorems(f)) out.emplace_back(f, std::move(t));
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
    std::istringstream in(out);
    std::string line;
    struct Msg {
        std::string sev, text;
    };
    std::vector<Msg> msgs;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch mm;
        static const std::regex re_axioms(R"('(.+)' depends on axioms: \[(.*)\])");
        static const std::regex re_no_axioms(R"('(.+)' does not depend on any axioms)");
        static const std::regex re_msg(R"(^(?:(?:error|warning|info): )?(.*?):(\d+):(\d+): (error|warning|info): ?(.*)$)");
        if (std::regex_search(line, mm, re_axioms)) {
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
        if (std::regex_search(line, mm, re_no_axioms)) {
            auto name = mm[1].str();
            if (name == theorem || name.ends_with("." + theorem)) c.axioms_seen = true;
            continue;
        }
        if (std::regex_match(line, mm, re_msg)) {
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

// ------------------------------------------------------------------ batched checks
namespace {
// The statement with its declaration name replaced (modifiers and attributes
// before the keyword dropped: a candidate is a plain theorem).
std::string candidate_statement(const LeanTarget& t, const std::string& name) {
    auto m = mask_lean(t.statement);
    for (auto* kw : {"theorem", "lemma"}) {
        for (auto kp : word_hits(m, kw, 0, m.size())) {
            auto p = kp + std::strlen(kw);
            while (p < m.size() && (m[p] == ' ' || m[p] == '\t' || m[p] == '\n')) ++p;
            if (m.compare(p, t.theorem.size(), t.theorem) != 0) continue;
            return "theorem " + name + t.statement.substr(p + t.theorem.size());
        }
    }
    return {};
}
int count_lines(const std::string& s) { return static_cast<int>(std::count(s.begin(), s.end(), '\n')); }
}  // namespace

LeanBatch lean_batch_text(const std::string& original, const LeanTarget& t,
                          const std::vector<std::vector<std::string>>& candidates, int first_index) {
    LeanBatch b;
    auto m = mask_lean(original);
    std::string text = original.substr(0, decl_end(m, t.proof_end));
    if (!text.empty() && text.back() != '\n') text += "\n";
    text += "\n";
    const auto dot = t.theorem.rfind('.');
    const std::string last = dot == std::string::npos ? t.theorem : t.theorem.substr(dot + 1);
    for (std::size_t k = 0; k < candidates.size(); ++k) {
        const std::string name = "prism_cand_" + std::to_string(first_index + static_cast<int>(k)) + "_" + last;
        const auto stmt = candidate_statement(t, name);
        const int first = count_lines(text) + 1;
        if (stmt.empty()) {  // cannot be renamed: never checked, never accepted
            b.names.push_back(name);
            b.lines.emplace_back(first, first - 1);
            continue;
        }
        text += stmt + " := by\n";
        for (auto line : candidates[k]) {
            while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
            text += t.indent + line + "\n";
        }
        text += "#print axioms " + name + "\n";
        b.names.push_back(name);
        b.lines.emplace_back(first, count_lines(text));
        text += "\n";
    }
    b.text = std::move(text);
    return b;
}

std::vector<LeanCheck> split_lean_batch(const std::string& out, const LeanBatch& b) {
    static const std::regex re_head(R"(^(?:(?:error|warning|info): )?(?:.*?):(\d+):(\d+): (?:error|warning|info):)");
    static const std::regex re_ax(R"('(.+)' (?:depends on axioms|does not depend on any axioms))");
    const std::size_t n = b.names.size();
    std::vector<std::string> chunk(n);
    std::vector<char> reached(n, 0);
    auto owner_of_line = [&](int line) -> int {
        for (std::size_t k = 0; k < n; ++k)
            if (line >= b.lines[k].first && line <= b.lines[k].second) return static_cast<int>(k);
        return -1;
    };
    auto owner_of_name = [&](const std::string& nm) -> int {
        for (std::size_t k = 0; k < n; ++k)
            if (nm == b.names[k] || nm.ends_with("." + b.names[k])) return static_cast<int>(k);
        return -1;
    };
    int cur = -1;
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        std::smatch mm;
        if (std::regex_search(line, mm, re_ax)) {
            const int k = owner_of_name(mm[1].str());
            if (k >= 0) {
                chunk[static_cast<std::size_t>(k)] += line + "\n";
                reached[static_cast<std::size_t>(k)] = 1;
            }
            cur = -1;
            continue;
        }
        if (std::regex_search(line, mm, re_head)) cur = owner_of_line(std::atoi(mm[1].str().c_str()));
        if (cur >= 0) {
            chunk[static_cast<std::size_t>(cur)] += line + "\n";
            reached[static_cast<std::size_t>(cur)] = 1;
        }
    }
    std::vector<LeanCheck> res;
    res.reserve(n);
    for (std::size_t k = 0; k < n; ++k) {
        if (b.lines[k].second < b.lines[k].first) {
            LeanCheck c;
            c.ran = false;
            c.errors = "candidate not checked: the theorem statement could not be renamed for a batch";
            res.push_back(std::move(c));
            continue;
        }
        if (!reached[k]) {
            LeanCheck c;
            c.ran = false;
            c.errors = "lean stopped before this candidate (no message and no #print axioms line)";
            res.push_back(std::move(c));
            continue;
        }
        res.push_back(parse_lean_output(chunk[k], b.names[k], 0));
    }
    return res;
}

std::vector<std::string> lean_closers(bool mathlib) {
    std::vector<std::string> c{"rfl", "trivial", "decide", "omega", "simp", "simp_all", "assumption",
                               "contradiction", "grind", "intros; omega", "intros; simp_all", "constructor",
                               "constructor <;> simp_all", "constructor <;> omega", "simp +arith"};
    if (mathlib)
        for (auto* x : {"norm_num", "linarith", "nlinarith", "positivity", "ring", "tauto", "aesop", "simp_all; linarith"})
            c.emplace_back(x);
    return c;
}

std::vector<std::string> lean_statement_closers(const std::string& original, const LeanTarget& t) {
    auto m = mask_lean(t.statement);
    // explicit binders "(a b : T)" with T one identifier; all binder names for revert
    std::vector<std::string> vars, all_names;
    {
        auto after = m.find(t.theorem);
        std::size_t i = after == std::string::npos ? 0 : after + t.theorem.size();
        int depth = 0;
        std::size_t open = 0;
        char opener = 0;
        for (; i < m.size(); ++i) {
            char c = m[i];
            if (depth == 0 && (c == '(' || c == '{' || c == '[')) {
                depth = 1;
                open = i;
                opener = c;
                continue;
            }
            if (depth > 0) {
                if (c == '(' || c == '{' || c == '[') ++depth;
                else if (c == ')' || c == '}' || c == ']') {
                    if (--depth == 0 && opener != '[') {
                        auto inner = t.statement.substr(open + 1, i - open - 1);
                        auto colon = inner.find(':');
                        if (colon == std::string::npos) continue;
                        auto names = trim(inner.substr(0, colon));
                        auto type = trim(inner.substr(colon + 1));
                        bool plain = !type.empty();
                        for (char ch : type)
                            if (!(ident_char(ch))) plain = false;
                        std::istringstream ns(names);
                        std::string nm;
                        while (ns >> nm) {
                            all_names.push_back(nm);
                            if (opener == '(' && plain) vars.push_back(nm);
                        }
                    }
                }
                continue;
            }
            if (c == ':') break;  // the statement's type starts
        }
    }
    // the file's own definitions named in the statement (before the target)
    std::set<std::string> defs;
    {
        auto fm = mask_lean(original.substr(0, t.decl_begin));
        for (auto* kw : {"def", "abbrev"})
            for (auto kp : word_hits(fm, kw, 0, fm.size())) {
                auto p = kp + std::strlen(kw);
                while (p < fm.size() && (fm[p] == ' ' || fm[p] == '\t')) ++p;
                auto q = p;
                while (q < fm.size() && ident_char(fm[q])) ++q;
                auto name = fm.substr(p, q - p);
                if (auto d = name.rfind('.'); d != std::string::npos) name = name.substr(d + 1);
                if (!name.empty()) defs.insert(name);
            }
    }
    std::vector<std::string> unfold;
    {
        std::string tok;
        for (std::size_t k = 0; k <= m.size(); ++k) {
            char c = k < m.size() ? m[k] : ' ';
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '\'') {
                tok += c;
                continue;
            }
            if (!tok.empty() && defs.count(tok) && std::find(unfold.begin(), unfold.end(), tok) == unfold.end() &&
                unfold.size() < 6)
                unfold.push_back(tok);
            tok.clear();
        }
    }
    std::string u;
    for (auto& d : unfold) u += (u.empty() ? "" : ", ") + d;
    const std::string su = u.empty() ? "" : " [" + u + "]";
    std::vector<std::string> out;
    auto add = [&](std::string s) {
        if (std::find(out.begin(), out.end(), s) == out.end() && out.size() < 24) out.push_back(std::move(s));
    };
    if (!u.empty()) {
        add("simp" + su);
        add("simp_all" + su);
        add("grind" + su);
    }
    for (std::size_t a = 0; a < vars.size() && a < 2; ++a) {
        const auto& x = vars[a];
        add("cases " + x + " <;> simp_all" + su);
        add("cases " + x + " <;> simp" + su);
        add("cases " + x + " <;> decide");
        add("cases " + x + " <;> rfl");
    }
    if (vars.size() >= 2) {
        add("cases " + vars[0] + " <;> cases " + vars[1] + " <;> simp_all" + su);
        add("cases " + vars[0] + " <;> cases " + vars[1] + " <;> first | rfl | decide | simp_all" + su);
    }
    if (!all_names.empty()) {
        std::string rv;
        for (auto& n : all_names) rv += " " + n;
        add("revert" + rv + "; decide");
    }
    return out;
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
        } catch (const nlohmann::json::exception&) {
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

prism::exec::ProcOut run_lake(const fs::path& lake, const std::vector<std::string>& args, const fs::path& cwd,
                         double timeout, const fs::path& writable) {
    std::vector<std::string> argv{lake.string()};
    argv.insert(argv.end(), args.begin(), args.end());
    return prism::exec::run_process(sandbox::wrap_argv(argv, writable), timeout, cwd);
}

bool copy_project(const fs::path& from, const fs::path& to, std::string* why) {
    std::error_code ec;
    fs::create_directories(to, ec);
    // Lake keeps a project's dependencies (Mathlib, Batteries, ...) in .lake/packages, each with its own lakefile.
    // They are not copied (Mathlib is several GB) and not skipped either: the scratch copy links to them, so
    // `lake build` there finds the sources and the built .olean files instead of fetching and rebuilding them.
    // Without this, a project that depends on Mathlib never builds in the scratch directory. The link is only read.
    const auto packages = from / ".lake" / "packages";
    if (fs::is_directory(packages, ec)) {
        fs::create_directories(to / ".lake", ec);
        fs::create_directory_symlink(fs::absolute(packages, ec), to / ".lake" / "packages", ec);
        if (ec) {
            if (why) *why = "link " + packages.string() + ": " + ec.message();
            return false;
        }
    }
    for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        auto rel = fs::relative(it->path(), from, ec);
        auto name = it->path().filename().string();
        if (it->is_directory(ec)) {
            // Nested lake projects and VCS data are not part of this package.
            bool nested = it->path() != from && (fs::exists(it->path() / "lakefile.toml", ec) ||
                                                 fs::exists(it->path() / "lakefile.lean", ec));
            const bool is_packages = name == "packages" && it->path().parent_path().filename() == ".lake";
            if (name == ".git" || nested || is_packages) {
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
// The prover is a FORGE .fpk (e.g. Goedel-Prover converted with FORGE), run in process.
std::vector<fs::path> default_prover_fpks() {
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
        if (e.path().extension() == ".fpk" && (low.find("prover") != std::string::npos || low.find("goedel") != std::string::npos))
            out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::shared_ptr<ModelBackend> resolve_prover(const ProveOptions& opt, std::string* why) {
    if (opt.backend) return opt.backend;
    auto model = !opt.prover_model.empty() ? opt.prover_model : env_or("PRISM_PROVER_MODEL", "");
    fs::path fpk = !opt.prover_fpk.empty() ? opt.prover_fpk : fs::path(env_or("PRISM_PROVER_FPK"));
    if (fpk.empty()) {
        auto found = default_prover_fpks();
        if (!found.empty()) fpk = found.front();
    }
    // No .fpk named or found: FORGE's remote client when FORGE_AI_* is set
    // (prism/remote_llm.hpp). A named .fpk that is missing stays NOTRUN.
    if (fpk.empty()) {
        std::string rwhy;
        if (auto remote = remote_llm::from_env(&rwhy)) return remote;
        if (!rwhy.empty()) {
            if (why) *why = "remote prover settings: " + rwhy;
            return nullptr;
        }
    }
    if (!forge_llm::is_fpk(fpk)) {
        if (why)
            *why = "no prover model: set PRISM_PROVER_FPK to a FORGE .fpk (Goedel-Prover / DeepSeek-Prover / "
                   "Kimina-Prover converted with FORGE), or put one in ~/.prism/models, or set FORGE_AI_PROVIDER "
                   "for a remote model"
                   + (fpk.empty() ? std::string{} : " (not a .fpk: " + fpk.string() + ")");
        return nullptr;
    }
    return std::make_shared<forge_llm::Backend>(fpk, model.empty() ? fpk.filename().string() : model, 0.6f, 1024);
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
        for (auto& w : words(lib.at(i).statement))
            if (target.count(w)) ++s;
        scored.push_back({-s, lib.size() - i});  // more overlap, then newer first
    }
    std::sort(scored.begin(), scored.end());
    std::vector<LemmaEntry> out;
    for (std::size_t i = 0; i < scored.size() && out.size() < k; ++i)
        out.push_back(lib.at(lib.size() - scored.at(i).second));
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
        auto backend = resolve_prover(opt, &why);
    if (!backend) {
        res.status = std::string(laws::NOTRUN);
        res.reason = why;
        return res;
    }
    res.model = backend->name();
    PathGuard path_guard(*lake);

    // Scratch copy of the package (the source tree is never touched unless --write).
    auto stamp = std::to_string(prism::clock::now_ns());
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

    // Batched kernel checks (lean_batch_text): one Lean run elaborates the
    // file's prefix once for many candidates; a batch is split over up to
    // lean_jobs processes on FORGE's task graph, each with its own file.
    const int lean_jobs = opt.lean_jobs > 0 ? opt.lean_jobs : prism::clamp_jobs(0);
    int batch_seq = 0;
    auto check_batch = [&](const std::vector<std::vector<std::string>>& cands) {
        std::vector<LeanCheck> out(cands.size());
        if (cands.empty()) return out;
        const std::size_t groups =
            std::min<std::size_t>(static_cast<std::size_t>(std::max(1, lean_jobs)), (cands.size() + 2) / 3);
        std::vector<std::vector<std::size_t>> idx(groups);
        for (std::size_t i = 0; i < cands.size(); ++i) idx[i % groups].push_back(i);
        const int seq = batch_seq++;
        std::vector<int> runs(groups, 0);
        prism::pool::TaskGraph g(static_cast<int>(groups));
        for (std::size_t gi = 0; gi < groups; ++gi)
            g.spawn([&, gi] {
                std::vector<std::vector<std::string>> mine;
                for (auto i : idx[gi]) mine.push_back(cands[i]);
                auto batch = lean_batch_text(original, t, mine, static_cast<int>(idx[gi].front()));
                const auto rel = rel_file.parent_path() /
                                 ("PrismBatch" + std::to_string(seq) + "_" + std::to_string(gi) + ".lean");
                write_text(scratch / rel, batch.text);
                auto r = run_lake(*lake, {"env", "lean", rel.string()}, scratch, opt.lean_timeout, scratch);
                runs[gi] = 1;
                std::vector<LeanCheck> cs;
                if (r.failed || r.timed_out) {
                    LeanCheck c;
                    c.ran = !r.failed;
                    c.errors = r.timed_out ? "lean timed out" : "lean could not be started";
                    cs.assign(mine.size(), c);
                } else {
                    cs = split_lean_batch(r.text, batch);
                }
                for (std::size_t j = 0; j < idx[gi].size(); ++j) out[idx[gi][j]] = std::move(cs[j]);
            });
        g.wait();
        for (int x : runs) res.lean_runs += x;
        res.kernel_checks += static_cast<int>(cands.size());
        return out;
    };

    auto lemma_path = !opt.lemmas.empty() ? opt.lemmas : default_lemma_path(t.project);
    auto lemmas = relevant_lemmas(read_lemmas(lemma_path), t.statement, 8);
    const bool mathlib = original.substr(0, t.decl_begin).find("import Mathlib") != std::string::npos;
    auto closers = opt.closers ? lean_closers(mathlib) : std::vector<std::string>{};
    if (opt.closers)
        for (auto& c : lean_statement_closers(original, t)) closers.push_back(std::move(c));
    // the same validator as model output (statement closers carry the file's identifiers)
    std::erase_if(closers, [](const std::string& c) {
        return !validate_lean_tactics(nlohmann::json{{"tactics", {c}}}.dump()).ok;
    });

    // Acceptance gate (unchanged): the axiom audit, then `lake build` of the
    // module with the proof spliced in place. "" when accepted, else why not.
    auto gate = [&](const std::vector<std::string>& cand, const LeanCheck& c) -> std::string {
        if (!c.axioms_ok) {
            ++res.rejected_axioms;
            std::string ax = c.axioms_seen ? join(c.axioms, ", ") : "no #print axioms report";
            res.attempts.push_back("rejected (axioms): " + ax);
            return "Lean accepted the proof but it depends on axioms outside propext, Classical.choice, Quot.sound: " + ax;
        }
        write_text(work_file, splice_proof(original, t, cand, false));
        auto bld = run_lake(*lake, {"build", t.module}, scratch, opt.lean_timeout * 4, scratch);
        ++res.lean_runs;
        if (bld.rc != 0 || bld.failed || bld.timed_out) {
            res.attempts.push_back("rejected (lake build): " + trim(bld.text).substr(0, 200));
            return "lake build failed: " + trim(bld.text).substr(0, 1500);
        }
        return {};
    };
    // After the gate: the result, and --write (in place, re-built, reverted on failure).
    auto finish = [&](const std::vector<std::string>& cand, const LeanCheck& c, const std::string& audit_id,
                      const std::string& source) {
        res.attempts.push_back("accepted");
        res.audit_id = audit_id;
        res.proof = cand;
        res.axioms = c.axioms;
        res.closed_by = source;
        res.status = std::string(laws::PROVED);
        res.reason = "Lean kernel accepted the proof; axioms within {propext, Classical.choice, Quot.sound}; "
                     "lake build " + t.module + " succeeded" + (source == "closer" ? " (closed by a core tactic, no model call)" : "");
        if (!opt.write) return;
        if (sha256_hex(read_text(t.file)) != original_sha) {
            res.reason += "; NOT written: " + t.file.string() + " changed during the search";
            return;
        }
        write_text(t.file, splice_proof(original, t, cand, false));
        auto inplace = run_lake(*lake, {"build", t.module}, t.project, opt.lean_timeout * 4, t.project);
        if (inplace.rc != 0) {
            write_text(t.file, original);
            res.status = std::string(laws::ERROR);
            res.reason = "proof checked in the scratch copy but lake build in place failed; file "
                         "restored: " + trim(inplace.text).substr(0, 500);
            return;
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
        e.model = source == "closer" ? std::string("prism-closer") : backend->name();
        e.audit_id = audit_id;
        append_lemma(lemma_path, e);
    };

    // ---- metric-guided best-first search ----
    // A node is a tactic prefix Lean accepts with only "unsolved goals" left.
    // Its metric (lower is better): goals left, then the size of the goal
    // state, a bonus for depth (accepted steps are progress) and a penalty
    // per expansion (a node that keeps failing falls behind). Two prefixes
    // with the same goal state are the same search state: only the first is
    // kept (transposition table on the whitespace-normalised goals).
    struct Entry {
        ProveNode node;
        int expansions = 0;
    };
    auto goal_metric = [](const ProveNode& n) {
        return n.goal_count * 1000 + static_cast<int>(std::min<std::size_t>(n.goals.size(), 8000) / 8);
    };
    auto score = [&](const Entry& e) { return goal_metric(e.node) + e.expansions * 1500 - e.node.depth * 250; };
    auto norm_goals = [](const std::string& g) {
        std::string o;
        bool sp = false;
        for (char ch : g) {
            if (ch == ' ' || ch == '\n' || ch == '\t') sp = true;
            else {
                if (sp && !o.empty()) o += ' ';
                sp = false;
                o += ch;
            }
        }
        return o;
    };
    std::vector<Entry> frontier;
    std::set<std::string> seen{join_lines({})};
    std::set<std::string> seen_goals;
    auto add_child = [&](const ProveNode& parent, const std::vector<std::string>& cand, const LeanCheck& c,
                         bool need_progress) {
        ProveNode child;
        child.tactics = cand;
        child.goals = c.goals;
        child.goal_count = c.goal_count;
        child.depth = parent.depth + 1;
        if (need_progress && goal_metric(child) >= goal_metric(parent)) return false;
        if (!seen_goals.insert(norm_goals(c.goals)).second) return false;
        frontier.push_back({child, 0});
        return true;
    };
    // Closers on a node: accepted proof, or children that made progress.
    // true when the theorem is proved (res is final).
    auto try_closers = [&](Entry& e, std::vector<LeanCheck>* pre = nullptr) {
        e.node.closers_tried = true;
        if (closers.empty()) return false;
        std::vector<std::vector<std::string>> cands;
        for (auto& cl : closers) {
            auto cand = e.node.tactics;
            cand.push_back(cl);
            if (seen.insert(join_lines(cand)).second) cands.push_back(std::move(cand));
        }
        auto cs = pre ? *pre : check_batch(cands);
        res.closer_checks += static_cast<int>(cands.size());
        for (std::size_t i = 0; i < cands.size(); ++i) {
            const auto& c = cs[i];
            if (c.complete) {
                if (gate(cands[i], c).empty()) {
                    finish(cands[i], c, "", "closer");
                    return true;
                }
                continue;
            }
            if (c.only_unsolved) add_child(e.node, cands[i], c, true);
        }
        res.attempts.push_back("closers: " + std::to_string(cands.size()) + " tried at depth " +
                               std::to_string(e.node.depth));
        return false;
    };

    // Root: the theorem's goal (tactic `skip` leaves it unsolved), checked in
    // the same Lean run as the closers on it.
    {
        std::vector<std::vector<std::string>> first{{"skip"}};
        for (auto& cl : closers) first.push_back({cl});
        for (auto& cand : first) seen.insert(join_lines(cand));
        auto cs = check_batch(first);
        ProveNode root;
        root.goals = cs[0].goals;
        root.goal_count = cs[0].goal_count;
        if (!cs[0].only_unsolved) root.last_error = cs[0].errors;
        seen_goals.insert(norm_goals(root.goals));
        frontier.push_back({root, 0});
        if (!closers.empty()) {
            std::vector<LeanCheck> rest(cs.begin() + 1, cs.end());
            // try_closers re-derives the candidate list; mark them unseen for it
            for (std::size_t i = 1; i < first.size(); ++i) seen.erase(join_lines(first[i]));
            if (try_closers(frontier.front(), &rest)) return res;
        }
    }

    int closer_passes = 0;
    while (res.model_calls < opt.budget && !frontier.empty()) {
        std::sort(frontier.begin(), frontier.end(),
                  [&](const Entry& a, const Entry& b) { return score(a) < score(b); });
        if (static_cast<int>(frontier.size()) > opt.beam) frontier.resize(static_cast<std::size_t>(opt.beam));
        // closers before any model call on a node
        // (bounded: a closer child must lower the goal metric, and at most
        // 4 x budget closer passes run in one search)
        {
            auto it = std::find_if(frontier.begin(), frontier.end(), [](const Entry& e) { return !e.node.closers_tried; });
            if (it != frontier.end() && !closers.empty() && closer_passes++ < 4 * opt.budget) {
                if (try_closers(*it)) return res;
                continue;
            }
        }
        const std::size_t cur_i = 0;
        frontier[cur_i].expansions++;
        auto node = frontier[cur_i].node;

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

        // `samples` replies from this node, then one batch check of the valid, new ones.
        struct Pending {
            AuditRecord rec;
            std::vector<std::string> cand, items;
        };
        std::vector<Pending> pend;
        std::string node_error;
        const int k = std::max(1, std::min(opt.samples, opt.budget - res.model_calls));
        for (int si = 0; si < k; ++si) {
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
                node_error = "Your output was rejected before reaching Lean: " + v.reason;
                continue;
            }
            auto cand = node.tactics;
            cand.insert(cand.end(), v.items.begin(), v.items.end());
            if (!seen.insert(join_lines(cand)).second) {
                rec.checker_result = "duplicate candidate (already checked)";
                audit_append(rec);
                res.attempts.push_back("duplicate");
                continue;
            }
            pend.push_back({rec, cand, v.items});
        }
        std::vector<std::vector<std::string>> cands;
        for (auto& p : pend) cands.push_back(p.cand);
        auto cs = check_batch(cands);
        bool proved = false;
        for (std::size_t i = 0; i < pend.size() && !proved; ++i) {
            auto& rec = pend[i].rec;
            const auto& c = cs[i];
            if (c.complete) {
                auto why_not = gate(pend[i].cand, c);
                if (why_not.empty()) {
                    rec.checker_result = "accepted";
                    rec.verdict_effect = std::string(laws::PROVED);
                    audit_append(rec);
                    finish(pend[i].cand, c, rec.id, "model");
                    proved = true;
                    break;
                }
                rec.checker_result = why_not.starts_with("lake build") ? "rejected: lake build failed"
                                                                       : "rejected: axioms [" + join(c.axioms, ", ") + "]";
                audit_append(rec);
                node_error = why_not;
                continue;
            }
            if (c.only_unsolved && !pend[i].items.empty()) {
                rec.checker_result = "partial: " + std::to_string(c.goal_count) + " goal(s) left";
                audit_append(rec);
                res.attempts.push_back(rec.checker_result);
                if (!add_child(node, pend[i].cand, c, false))
                    res.attempts.push_back("partial: same goal state as a node already in the search");
                continue;
            }
            ++res.rejected_kernel;
            auto first = c.errors.substr(0, c.errors.find('\n'));
            rec.checker_result = "rejected: " + (c.errors.empty() ? std::string("lean error") : first.substr(0, 200));
            audit_append(rec);
            res.attempts.push_back("rejected (kernel): " + first.substr(0, 200));
            node_error = c.errors;
        }
        if (proved) return res;
        // the node's feedback for its next expansion (the frontier may have grown: find it again)
        for (auto& e : frontier)
            if (e.node.tactics == node.tactics) {
                if (!node_error.empty()) e.node.last_error = node_error;
                break;
            }
    }
    res.status = std::string(laws::UNKNOWN);
    res.reason = "search budget exhausted (" + std::to_string(res.model_calls) + " model calls, " +
                 std::to_string(res.kernel_checks) + " Lean checks); no candidate passed the kernel, the axiom "
                 "audit and lake build";
    return res;
}

// ------------------------------------------------------------------ CLI
namespace {
// One theorem through the proof search (NOTRUN without --allow-exec, Law 9),
// its record written to <out>/prove/<theorem>.json.
std::pair<ProveResult, nlohmann::json> prove_one(ProveOptions opt, bool allow_exec) {
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
                     {"lean_runs", r.lean_runs},
                     {"closer_checks", r.closer_checks},
                     {"closed_by", r.closed_by},
                     {"rejected_invalid", r.rejected_invalid},
                     {"rejected_kernel", r.rejected_kernel},
                     {"rejected_axioms", r.rejected_axioms},
                     {"attempts", r.attempts},
                     {"ai_audit_id", r.audit_id}};
    std::error_code ec;
    fs::create_directories(opt.out / "prove", ec);
    std::ofstream(opt.out / "prove" / (opt.theorem + ".json")) << j.dump(2) << "\n";
    return {std::move(r), std::move(j)};
}

std::string rel_path(const fs::path& p) {
    std::error_code ec;
    auto r = fs::relative(p, fs::current_path(ec), ec);
    return ec || r.empty() || r.string().starts_with("..") ? p.string() : r.generic_string();
}

// `prism prove --all [ROOTS...]`: every theorem with a sorry under the roots
// (default proofs, proofs/semantics, proofs/techniques). The summary line is
// the "proofs completed by the prover model" metric (9.7).
int prove_all(const std::vector<std::string>& roots_in, bool list, const ProveOptions& base, bool allow_exec) {
    std::vector<fs::path> roots;
    for (auto& r : roots_in) roots.emplace_back(r);
    if (roots.empty()) roots = {"proofs", "proofs/semantics", "proofs/techniques"};
    const auto targets = lean_sorry_targets(roots);
    if (list) {
        for (auto& [f, t] : targets) std::cout << rel_path(f) << "\t" << t << "\n";
        return 0;
    }
    if (targets.empty()) {
        std::string names;
        for (std::size_t i = 0; i < roots.size(); ++i) names += (i ? ", " : "") + roots.at(i).string();
        std::cout << "no theorem with a sorry under " << names << "\n";
        return 0;
    }
    nlohmann::json results = nlohmann::json::array();
    int proved = 0, notrun = 0;
    for (auto& [f, t] : targets) {
        auto opt = base;
        opt.file = fs::absolute(f);
        opt.theorem = t;
        auto [r, j] = prove_one(opt, allow_exec);
        j["file"] = rel_path(f);
        std::string status = r.status;
        if (status.size() < 10) status.resize(10, ' ');
        std::cout << status << " " << rel_path(f) << ":" << t << "  " << r.reason.substr(0, 120) << "\n";
        if (r.status == laws::PROVED) ++proved;
        if (r.status == laws::NOTRUN) ++notrun;
        results.push_back(std::move(j));
    }
    const int n = static_cast<int>(results.size());
    std::cout << "proved " << proved << "/" << n << " (metric 9.7: proofs completed by the prover model)\n";
    std::error_code ec;
    fs::create_directories(base.out / "prove", ec);
    std::ofstream(base.out / "prove" / "summary.json") << results.dump(2);
    if (proved == n) return 0;
    return notrun == n ? 3 : 1;
}
}  // namespace

int prove_main(int argc, char** argv) {
    ProveOptions opt;
    bool allow_exec = false, json_only = false, list = false, all = false;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--write") opt.write = true;
        else if (a == "--allow-exec") allow_exec = true;
        else if (a == "--json") json_only = true;
        else if (a == "--list") list = true;
        else if (a == "--all") all = true;
        else if (a == "--project") opt.project = next();
        else if (a == "--lemmas") opt.lemmas = next();
        else if (a == "--budget") opt.budget = std::max(1, std::atoi(next().c_str()));
        else if (a == "--beam") opt.beam = std::max(1, std::atoi(next().c_str()));
        else if (a == "--timeout") opt.lean_timeout = std::max(5.0, std::atof(next().c_str()));
        else if (a == "--out") opt.out = next();
        else if (a == "--samples") opt.samples = std::max(1, std::atoi(next().c_str()));
        else if (a == "--lean-jobs") opt.lean_jobs = std::max(1, std::atoi(next().c_str()));
        else if (a == "--no-closers") opt.closers = false;
        else if (a == "--prover-fpk") opt.prover_fpk = next();
        else if (a == "--prover-model") opt.prover_model = next();
        else if (a == "-h" || a == "--help") {
            std::cout << "prism prove FILE.lean THEOREM [--write] [--allow-exec] [--project DIR]\n"
                         "            [--lemmas PATH] [--budget N] [--beam N] [--timeout S] [--out DIR]\n"
                         "            [--prover-fpk PATH] [--prover-model NAME] [--json]\n"
                         "            [--samples N] [--lean-jobs N] [--no-closers]\n"
                         "prism prove FILE.lean --list      theorems with a sorry\n"
                         "prism prove --all [ROOTS...] [--list] [--allow-exec] [--write] [--budget N]\n"
                         "            every theorem with a sorry under ROOTS (default proofs,\n"
                         "            proofs/semantics, proofs/techniques; .lake skipped); writes\n"
                         "            <out>/prove/summary.json\n"
                         "Lean proof search (roadmap 9.2): the prover model proposes tactics, the Lean kernel\n"
                         "checks each one, #print axioms must stay within propext/Classical.choice/Quot.sound,\n"
                         "and lake build of the module gates acceptance. Candidates are checked in batches (one\n"
                         "Lean run elaborates the file prefix once for many; --lean-jobs runs in parallel); core\n"
                         "closers (omega, simp, grind, decide, ...) are tried on every node before the model\n"
                         "(--no-closers: model only); --samples N model replies per node. --write writes the proof back and adds\n"
                         "it to the lemma library (default proofs/lemmas.jsonl). Needs --allow-exec (Law 9:\n"
                         "elaborating Lean runs code). Prover: a FORGE .fpk, PRISM_PROVER_FPK.\n"
                         "Exit 0 proved (--all: every one), 1 not proved, 3 NOTRUN (--all: every one), 2 error.\n";
            return prism::code(prism::Exit::Ok);
        } else if (!a.starts_with("-")) pos.push_back(a);
        else {
            prism::log::error("prove", "unrecognized arguments: " + std::string(a));
            return prism::code(prism::Exit::Usage);
        }
    }
    if (all) {
        opt.out = fs::absolute(opt.out);
        return prove_all(pos, list, opt, allow_exec);
    }
    if (list && !pos.empty()) {
        for (auto& n : lean_sorry_theorems(pos.at(0))) std::cout << n << "\n";
        return prism::code(prism::Exit::Ok);
    }
    if (pos.size() != 2) {
        prism::log::error("prove", "usage: prism prove FILE.lean THEOREM [--write] [--allow-exec] (see --help)");
        return prism::code(prism::Exit::Usage);
    }
    opt.file = fs::absolute(pos.at(0));
    opt.theorem = pos.at(1);
    opt.out = fs::absolute(opt.out);
    auto [r, j] = prove_one(opt, allow_exec);
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
    if (r.status == laws::PROVED) return prism::code(prism::Exit::Ok);
    if (r.status == laws::NOTRUN) return prism::code(prism::Exit::Error);
    if (r.status == laws::ERROR) return prism::code(prism::Exit::Usage);
    return prism::code(prism::Exit::Failed);
}

}  // namespace prism::ai
