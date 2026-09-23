// Questions over findings and the GUI assistant (roadmap 9.4).
//
// A natural-language question becomes a structured Query (stage / status /
// class / file / function / text filters). The translation is ALWAYS shown
// next to the answer so it can be checked. Two translators:
//
//   * a deterministic keyword grammar (parse_question) that always works;
//   * a model, constrained by grammars/ask.gbnf to the Query JSON; its output
//     is re-validated by query_from_json and logged to ai_audit.jsonl. A
//     rejected or missing model falls back to the grammar, and says so.
//
// Answers are computed by run_query over report.json. Nothing here can
// change a status: the report is only read.

#include "ai_internal.hpp"

#include "prism/ai_assist.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace prism::ai {
namespace fs = std::filesystem;

namespace {
#include "grammars_assist.inc"

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

const std::vector<std::string>& all_statuses() {
    static const std::vector<std::string> v = {
        "PROVED-CERTIFIED", "PROVED-UNBOUNDED", "PROVED", "PROVED-ASSUMING", "BOUNDED", "FAILED",
        "UNKNOWN", "TIMEOUT", "ERROR", "NOTRUN", "NEEDS-HARNESS", "CRASH", "CLEAN", "SANFAIL",
        "HYPOTHESIS", "READS", "NOFUNC", "NOSEED"};
    return v;
}
const std::vector<std::string> kProof = {"PROVED-CERTIFIED", "PROVED-UNBOUNDED", "PROVED", "PROVED-ASSUMING"};
const std::vector<std::string> kDefect = {"FAILED", "CRASH", "SANFAIL"};
const std::vector<std::string> kUnproved = {"BOUNDED", "FAILED", "UNKNOWN", "TIMEOUT", "ERROR", "NOTRUN",
                                            "NEEDS-HARNESS", "CRASH", "SANFAIL", "HYPOTHESIS"};

std::vector<std::string> stage_names() {
    std::vector<std::string> v;
    for (auto* p = STAGE_ORDER; *p; ++p) v.emplace_back(*p);
    return v;
}

void add_all(std::vector<std::string>& to, const std::vector<std::string>& from) {
    for (auto& x : from)
        if (std::find(to.begin(), to.end(), x) == to.end()) to.push_back(x);
}

// word -> class substrings (matched case-insensitively against Finding::cls)
const std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>>& cls_lexicon() {
    static const std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> t = {
        {{"memory safety", "memory-safety", "memsafety", "memory"}, {"MEM-", "PTR-", "OOB", "UAF", "NULL", "LEAK"}},
        {{"out of bounds", "out-of-bounds", "oob", "buffer overflow", "buffer", "bounds", "index"}, {"OOB", "INDEX", "BUFFER"}},
        {{"use after free", "use-after-free", "uaf", "dangling"}, {"UAF", "DANGLE", "STACK-ESCAPE"}},
        {{"double free"}, {"DOUBLE-FREE"}},
        {{"null pointer", "null", "nullptr"}, {"NULL"}},
        {{"leak", "leaks"}, {"LEAK"}},
        {{"signed overflow", "integer overflows", "integer overflow", "overflow", "overflows", "wrap", "wraparound"}, {"OVF", "OVERFLOW", "WRAP"}},
        {{"division by zero", "divide by zero", "division", "div0", "divide"}, {"DIV"}},
        {{"shift", "shifts"}, {"SHIFT"}},
        {{"integer", "integers", "arithmetic"}, {"INT-"}},
        {{"uninitialized", "uninitialised", "uninit"}, {"UNINIT"}},
        {{"race", "races", "data race", "concurrency", "thread safety"}, {"RACE", "CONC-", "LOCK-"}},
        {{"deadlock", "lock", "locks", "locking"}, {"LOCK-"}},
        {{"format string", "format"}, {"FMT-"}},
        {{"crypto", "cryptography"}, {"CRYPTO-"}},
        {{"secret", "secrets", "credential", "credentials"}, {"SECRET"}},
        {{"taint", "injection"}, {"TAINT", "INJECT"}},
        {{"contract", "contracts"}, {"CONTRACT"}},
        {{"float", "floating point"}, {"FLOAT"}},
        {{"api misuse", "api"}, {"API-"}},
    };
    return t;
}

const std::map<std::string, std::string>& stage_synonyms() {
    static const std::map<std::string, std::string> m = {
        {"model checking", "bmc"}, {"model checker", "bmc"}, {"bounded model checking", "bmc"},
        {"fuzzer", "fuzz"}, {"fuzzing", "fuzz"}, {"fuzzers", "fuzz"},
        {"sanitizer", "sanitize"}, {"sanitizers", "sanitize"}, {"sanitiser", "sanitize"},
        {"lint", "lints"}, {"linter", "lints"}, {"linters", "polyglot"},
        {"compiler warnings", "warnings"}, {"k-induction", "bmc"},
        {"llvm", "pir"}, {"ir", "pir"}, {"symbolic execution", "concolic"}, {"mutation", "muttest"},
        {"repairs", "repair"}, {"explanations", "repair"}, {"model", "llm"}, {"taxonomy", "unify"}};
    return m;
}

const std::set<std::string>& stopwords() {
    static const std::set<std::string> s = {
        "a", "an", "the", "of", "in", "on", "for", "with", "and", "or", "to", "is", "are", "was", "were", "be",
        "show", "list", "me", "all", "any", "which", "what", "where", "findings", "finding", "results",
        "result", "issues", "issue", "problems", "problem", "that", "there", "do", "does", "we", "have", "has",
        "give", "find", "get", "i", "want", "see", "please", "from", "by", "at", "about", "it", "its",
        "code", "checks", "check", "reported", "report", "status", "verdict", "verdicts", "things", "every",
        "stage", "stages", "safety", "only", "still", "yet", "are", "not", "no", "under", "inside", "within"};
    return s;
}

bool looks_like_path(const std::string& w) {
    if (w.find('/') != std::string::npos) return true;
    for (const char* e : {".c", ".h", ".cc", ".cpp", ".hpp", ".cxx", ".py", ".rs", ".go", ".js", ".ts", ".java"})
        if (w.size() > std::string(e).size() && w.ends_with(e)) return true;
    return false;
}

bool vocab_status(const std::string& s) {
    const auto& v = all_statuses();
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

// ---------------------------------------------------------------- JSON
std::string query_to_json(const Query& q) {
    nlohmann::json j;
    j["stages"] = q.stages;
    j["statuses"] = q.statuses;
    j["cls"] = q.cls;
    if (!q.strengths.empty()) j["strengths"] = q.strengths;
    j["file_glob"] = q.file_glob;
    j["function_glob"] = q.function_glob;
    j["text"] = q.text;
    j["group_by"] = q.group_by;
    j["count_only"] = q.count_only;
    if (q.limit != 50) j["limit"] = q.limit;
    return j.dump();
}

std::optional<Query> query_from_json(const std::string& text, std::string* why) {
    auto fail = [&](std::string m) -> std::optional<Query> {
        if (why) *why = std::move(m);
        return std::nullopt;
    };
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(trim(text));
    } catch (...) {
        return fail("not JSON");
    }
    if (!j.is_object()) return fail("not a JSON object");
    static const std::set<std::string> keys = {"stages", "statuses", "cls", "strengths", "file_glob",
                                               "function_glob", "text", "group_by", "count_only", "limit"};
    Query q;
    auto stages = stage_names();
    auto str_list = [&](const char* k, std::vector<std::string>& out) -> bool {
        if (!j.contains(k)) return true;
        if (!j[k].is_array() || j[k].size() > 20) return false;
        for (auto& x : j[k]) {
            if (!x.is_string() || x.get<std::string>().size() > 80) return false;
            out.push_back(x.get<std::string>());
        }
        return true;
    };
    for (auto& [k, v] : j.items())
        if (!keys.contains(k)) return fail("unknown key '" + k + "'");
    if (!str_list("stages", q.stages)) return fail("stages must be a list of strings");
    for (auto& s : q.stages)
        if (std::find(stages.begin(), stages.end(), s) == stages.end()) return fail("unknown stage '" + s + "'");
    if (!str_list("statuses", q.statuses)) return fail("statuses must be a list of strings");
    for (auto& s : q.statuses)
        if (!vocab_status(s)) return fail("unknown status '" + s + "'");
    if (!str_list("cls", q.cls)) return fail("cls must be a list of strings");
    if (!str_list("strengths", q.strengths)) return fail("strengths must be a list of strings");
    for (auto& s : q.strengths)
        if (s != "PROVES" && s != "FINDS" && s != "SOME" && s != "READS") return fail("unknown strength '" + s + "'");
    for (const char* k : {"file_glob", "function_glob", "text", "group_by"}) {
        if (!j.contains(k)) continue;
        if (!j[k].is_string() || j[k].get<std::string>().size() > 120) return fail(std::string(k) + " must be a short string");
    }
    q.file_glob = j.value("file_glob", "");
    q.function_glob = j.value("function_glob", "");
    q.text = j.value("text", "");
    q.group_by = j.value("group_by", "");
    if (!q.group_by.empty() && q.group_by != "stage" && q.group_by != "status" && q.group_by != "cls" &&
        q.group_by != "file" && q.group_by != "function")
        return fail("group_by must be stage|status|cls|file|function");
    if (j.contains("count_only")) {
        if (!j["count_only"].is_boolean()) return fail("count_only must be a boolean");
        q.count_only = j["count_only"].get<bool>();
    }
    if (j.contains("limit")) {
        if (!j["limit"].is_number_integer() || j["limit"].get<int>() < 1 || j["limit"].get<int>() > 10000)
            return fail("limit must be 1..10000");
        q.limit = j["limit"].get<int>();
    }
    return q;
}

// ---------------------------------------------------------------- keyword grammar
Query parse_question(const std::string& question, std::string* unused) {
    Query q;
    std::string text = lower(question);
    std::string original = question;
    // Quoted text -> message substring.
    for (char quote : {'"', '\''}) {
        auto a = text.find(quote);
        auto b = a == std::string::npos ? a : text.find(quote, a + 1);
        if (a != std::string::npos && b != std::string::npos && b > a + 1) {
            q.text = original.substr(a + 1, b - a - 1);
            text.replace(a, b - a + 1, " ");
            original.replace(a, b - a + 1, " ");
            break;
        }
    }
    // Tokenise keeping path/glob characters and hyphenated words.
    std::vector<std::string> toks, orig;
    {
        std::string cur, cur_o;
        auto flush = [&] {
            while (!cur.empty() && (cur.back() == '.' || cur.back() == '?' || cur.back() == ',' || cur.back() == '!')) {
                cur.pop_back();
                cur_o.pop_back();
            }
            if (!cur.empty()) {
                toks.push_back(cur);
                orig.push_back(cur_o);
            }
            cur.clear();
            cur_o.clear();
        };
        for (std::size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (std::isalnum(static_cast<unsigned char>(c)) || std::string("_-./*+:#()?").find(c) != std::string::npos) {
                cur += c;
                cur_o += original[i];
            } else {
                flush();
            }
        }
        flush();
    }
    std::vector<bool> used(toks.size(), false);
    auto phrase_at = [&](std::size_t i, const std::string& p) -> std::size_t {
        std::istringstream is(p);
        std::vector<std::string> pw;
        for (std::string w; is >> w;) pw.push_back(w);
        if (i + pw.size() > toks.size()) return 0;
        for (std::size_t k = 0; k < pw.size(); ++k)
            if (toks[i + k] != pw[k]) return 0;
        return pw.size();
    };
    auto mark = [&](std::size_t i, std::size_t n) {
        for (std::size_t k = 0; k < n; ++k) used[i + k] = true;
    };
    bool negate_proof = false;
    const auto stages = stage_names();
    for (std::size_t i = 0; i < toks.size(); ++i) {
        if (used[i]) continue;
        const std::string& w = toks[i];
        std::size_t n = 0;
        // negation of proofs: "not proved", "unproved", "unproven", "open", "not verified"
        if ((n = phrase_at(i, "not proved")) || (n = phrase_at(i, "not proven")) || (n = phrase_at(i, "not verified")) ||
            (n = phrase_at(i, "unproved")) || (n = phrase_at(i, "unproven")) || (n = phrase_at(i, "unverified"))) {
            negate_proof = true;
            mark(i, n);
            continue;
        }
        if ((n = phrase_at(i, "how many")) || (n = phrase_at(i, "number of")) || (n = phrase_at(i, "count"))) {
            q.count_only = true;
            mark(i, n);
            continue;
        }
        if ((n = phrase_at(i, "group by")) || (n = phrase_at(i, "grouped by")) || (n = phrase_at(i, "per")) ||
            (n = phrase_at(i, "by"))) {
            if (i + n < toks.size()) {
                static const std::map<std::string, std::string> g = {
                    {"stage", "stage"}, {"stages", "stage"}, {"status", "status"}, {"verdict", "status"},
                    {"class", "cls"}, {"cls", "cls"}, {"category", "cls"}, {"file", "file"}, {"files", "file"},
                    {"module", "file"}, {"function", "function"}, {"functions", "function"}};
                if (auto it = g.find(toks[i + n]); it != g.end()) {
                    q.group_by = it->second;
                    mark(i, n + 1);
                    continue;
                }
            }
        }
        if ((n = phrase_at(i, "top")) || (n = phrase_at(i, "first")) || (n = phrase_at(i, "limit"))) {
            if (i + 1 < toks.size() && std::all_of(toks[i + 1].begin(), toks[i + 1].end(), ::isdigit) &&
                toks[i + 1].size() < 6) {
                q.limit = std::max(1, std::stoi(toks[i + 1]));
                mark(i, 2);
                continue;
            }
        }
        // file / module / directory / function scopes
        if ((n = phrase_at(i, "module")) || (n = phrase_at(i, "file")) || (n = phrase_at(i, "directory")) ||
            (n = phrase_at(i, "dir")) || (n = phrase_at(i, "folder")) || (n = phrase_at(i, "path"))) {
            if (i + 1 < toks.size() && !stopwords().contains(toks[i + 1])) {
                auto v = orig[i + 1];
                q.file_glob = v.find_first_of("*?") != std::string::npos ? v : "*" + v + "*";
                mark(i, 2);
                continue;
            }
        }
        if ((n = phrase_at(i, "function")) || (n = phrase_at(i, "functions")) || (n = phrase_at(i, "fn")) ||
            (n = phrase_at(i, "method"))) {
            if (i + 1 < toks.size() && !stopwords().contains(toks[i + 1]) && !looks_like_path(toks[i + 1])) {
                auto v = orig[i + 1];
                q.function_glob = v;
                mark(i, 2);
                continue;
            }
        }
        if ((n = phrase_at(i, "strength")) && i + 1 < toks.size()) {
            auto s = upper(toks[i + 1]);
            if (s == "PROVES" || s == "FINDS" || s == "SOME" || s == "READS") {
                q.strengths.push_back(s);
                mark(i, 2);
                continue;
            }
        }
        // Stage synonyms (multi-word first), then stage names.
        bool hit = false;
        static const auto synonyms = [] {  // longest phrase first ("model checking" before "model")
            std::vector<std::pair<std::string, std::string>> v(stage_synonyms().begin(), stage_synonyms().end());
            std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
            return v;
        }();
        for (auto& [syn, st] : synonyms)
            if ((n = phrase_at(i, syn))) {
                add_all(q.stages, {st});
                mark(i, n);
                hit = true;
                break;
            }
        if (hit) continue;
        if (std::find(stages.begin(), stages.end(), w) != stages.end() && w != "classify" && w != "optional") {
            add_all(q.stages, {w});
            used[i] = true;
            continue;
        }
        // Class lexicon (longest phrases are listed first per row).
        for (auto& [phrases, subs] : cls_lexicon()) {
            for (auto& p : phrases)
                if ((n = phrase_at(i, p))) {
                    add_all(q.cls, subs);
                    mark(i, n);
                    hit = true;
                    break;
                }
            if (hit) break;
        }
        if (hit) continue;
        // Exact status words (any case) and status vocabulary words.
        if (vocab_status(upper(w)) && orig[i] == upper(orig[i])) {  // spelled as a status word
            add_all(q.statuses, {upper(w)});
            used[i] = true;
            continue;
        }
        static const std::map<std::string, std::vector<std::string>> status_words = {
            {"failed", {"FAILED"}}, {"failing", {"FAILED"}}, {"fail", {"FAILED"}}, {"failures", {"FAILED"}},
            {"failure", {"FAILED"}}, {"counterexample", {"FAILED"}}, {"counterexamples", {"FAILED"}},
            {"bug", kDefect}, {"bugs", kDefect}, {"defect", kDefect}, {"defects", kDefect},
            {"crash", {"CRASH"}}, {"crashes", {"CRASH"}}, {"crashing", {"CRASH"}},
            {"proved", kProof}, {"proven", kProof}, {"proof", kProof}, {"proofs", kProof}, {"verified", kProof},
            {"safe", kProof},
            {"certified", {"PROVED-CERTIFIED"}}, {"unbounded", {"PROVED-UNBOUNDED"}},
            {"assuming", {"PROVED-ASSUMING"}}, {"assumptions", {"PROVED-ASSUMING"}},
            {"bounded", {"BOUNDED"}}, {"timeout", {"TIMEOUT"}}, {"timeouts", {"TIMEOUT"}},
            {"unknown", {"UNKNOWN"}}, {"errors", {"ERROR"}}, {"error", {"ERROR"}},
            {"notrun", {"NOTRUN"}}, {"skipped", {"NOTRUN"}}, {"missing", {"NOTRUN"}}, {"gaps", {"NOTRUN"}},
            {"harness", {"NEEDS-HARNESS"}}, {"harnesses", {"NEEDS-HARNESS"}},
            {"hypothesis", {"HYPOTHESIS"}}, {"hypotheses", {"HYPOTHESIS"}}, {"clean", {"CLEAN"}},
            {"open", kUnproved}};
        if ((n = phrase_at(i, "not run"))) {
            add_all(q.statuses, {"NOTRUN"});
            mark(i, n);
            continue;
        }
        if ((n = phrase_at(i, "needs harness")) || (n = phrase_at(i, "needs-harness")) ||
            (n = phrase_at(i, "need a harness")) || (n = phrase_at(i, "needs a harness")) ||
            (n = phrase_at(i, "need harness"))) {
            add_all(q.statuses, {"NEEDS-HARNESS"});
            mark(i, n);
            continue;
        }
        if (auto it = status_words.find(w); it != status_words.end()) {
            add_all(q.statuses, it->second);
            used[i] = true;
            continue;
        }
        // "in foo.c" / "src/net" : a path-looking word is a file scope.
        if (looks_like_path(w) && q.file_glob.empty()) {
            q.file_glob = orig[i].find_first_of("*?") != std::string::npos ? orig[i] : "*" + orig[i] + "*";
            used[i] = true;
            continue;
        }
        // "f()" style function names
        if (w.size() > 2 && w.ends_with("()")) {
            q.function_glob = orig[i].substr(0, orig[i].size() - 2);
            used[i] = true;
            continue;
        }
    }
    if (negate_proof) {
        // "unproved X": every status that is not a proof (and not clean noise).
        std::vector<std::string> st;
        if (q.statuses.empty()) st = kUnproved;
        else
            for (auto& s : q.statuses)
                if (std::find(kProof.begin(), kProof.end(), s) == kProof.end()) st.push_back(s);
        if (st.empty()) st = kUnproved;
        q.statuses = st;
    }
    if (unused) {
        std::string u;
        for (std::size_t i = 0; i < toks.size(); ++i)
            if (!used[i] && !stopwords().contains(toks[i])) u += (u.empty() ? "" : " ") + orig[i];
        *unused = u;
    }
    return q;
}

bool glob_match(const std::string& pattern, const std::string& text) {
    // Iterative '*' / '?' matcher, case-sensitive; '*' also crosses '/'.
    std::size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::vector<FindingRef> run_query(const RunReport& report, const Query& q) {
    std::vector<FindingRef> out;
    for (auto& r : enumerate_findings(report)) {
        const Finding& f = *r.f;
        if (!q.stages.empty() && std::find(q.stages.begin(), q.stages.end(), f.stage) == q.stages.end()) continue;
        if (!q.statuses.empty() && std::find(q.statuses.begin(), q.statuses.end(), f.status) == q.statuses.end())
            continue;
        if (!q.strengths.empty() && std::find(q.strengths.begin(), q.strengths.end(), f.strength) == q.strengths.end())
            continue;
        if (!q.cls.empty()) {
            auto c = upper(f.cls);
            bool any = false;
            for (auto& s : q.cls) any = any || (!s.empty() && c.find(upper(s)) != std::string::npos);
            if (!any) continue;
        }
        if (!q.file_glob.empty() && !glob_match(q.file_glob, f.file) &&
            !glob_match(lower(q.file_glob), lower(f.file)))
            continue;
        if (!q.function_glob.empty() && !glob_match(q.function_glob, f.function.value_or(""))) continue;
        if (!q.text.empty() && lower(f.message).find(lower(q.text)) == std::string::npos &&
            lower(f.counterexample).find(lower(q.text)) == std::string::npos)
            continue;
        out.push_back(r);
    }
    return out;
}

namespace {

std::string finding_line(const FindingRef& r) {
    const Finding& f = *r.f;
    std::string s = r.id + " " + f.status;
    if (!f.file.empty()) s += " " + f.file + (f.line ? ":" + std::to_string(*f.line) : "");
    if (f.function && !f.function->empty()) s += " `" + *f.function + "`";
    if (!f.cls.empty()) s += " " + f.cls;
    s += " — " + f.message.substr(0, 160);
    if (!f.counterexample.empty()) s += "  cex " + f.counterexample.substr(0, 80);
    return s;
}

std::string answer_text(const AskResult& r) {
    std::ostringstream o;
    o << "query (" << r.translator << "): " << query_to_json(r.query) << "\n";
    if (!r.unused.empty()) o << "not understood (ignored): " << r.unused << "\n";
    if (!r.model_note.empty()) o << "model: " << r.model_note << "\n";
    o << "answer: " << r.matches.size() << (r.matches.size() == 1 ? " finding" : " findings") << "\n";
    if (!r.query.group_by.empty()) {
        std::map<std::string, int> g;
        for (auto& m : r.matches) {
            const Finding& f = *m.f;
            std::string k = r.query.group_by == "stage" ? f.stage
                             : r.query.group_by == "status" ? f.status
                             : r.query.group_by == "cls" ? f.cls
                             : r.query.group_by == "file" ? f.file
                                                          : f.function.value_or("");
            ++g[k.empty() ? "(none)" : k];
        }
        std::vector<std::pair<std::string, int>> v(g.begin(), g.end());
        std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (auto& [k, n] : v) o << "  " << n << "  " << k << "\n";
    }
    if (!r.query.count_only) {
        int shown = 0;
        for (auto& m : r.matches) {
            if (shown++ >= r.query.limit) {
                o << "  ... " << (r.matches.size() - static_cast<std::size_t>(r.query.limit)) << " more\n";
                break;
            }
            o << "  " << finding_line(m) << "\n";
        }
    }
    return o.str();
}

std::string ask_grammar() {
    // Narrow the stage rule to the pipeline's stage names.
    std::string g = GBNF_ASK;
    std::string alts;
    for (auto& s : stage_names()) alts += (alts.empty() ? "" : " | ") + std::string("\"\\\"") + s + "\\\"\"";
    auto p = g.find("stage    ::= ");
    if (p != std::string::npos) {
        auto e = g.find('\n', p);
        g.replace(p, e - p, "stage    ::= " + alts);
    }
    return g;
}

}  // namespace

AskResult ask_question(const RunReport& report, const std::string& question, bool use_model) {
    AskResult r;
    r.query = parse_question(question, &r.unused);
    r.translator = "grammar";
    if (use_model) {
        std::string why;
        auto backend = session_config() ? session_backend(&why) : nullptr;
        if (!session_config()) why = "no AI session";
        if (!backend) {
            r.model_note = "NOTRUN: " + why + " (deterministic keyword grammar used)";
        } else {
            ModelRequest req;
            req.feature = "ask";
            req.grammar = "ask";
            req.grammar_text = ask_grammar();
            req.system = system_prompt(
                "Task: translate the user's question about verification findings into the JSON query described "
                "by the grammar. stages: pipeline stage names; statuses: PRISM verdict words; cls: substrings of "
                "finding classes such as MEM-, INT-SIGNED-OVF, NULL; file_glob / function_glob: shell globs; text: "
                "a message substring. Leave a field empty when the question does not constrain it.");
            req.user = fence_untrusted(question, "QUESTION");
            AuditRecord rec;
            rec.feature = "ask";
            rec.grammar = "ask";
            auto rep = ask(*backend, req, rec);
            std::string reject;
            std::optional<Query> mq;
            if (!rep.error.empty()) reject = rep.error;
            else mq = query_from_json(rep.text, &reject);
            rec.output_valid = mq.has_value();
            rec.rejected_reason = mq ? "" : reject;
            rec.checker = "query-validator";
            rec.checker_result = mq ? "accepted" : "rejected";
            rec.verdict_effect = "none";
            audit_append(rec);
            if (mq) {
                mq->limit = r.query.limit;
                r.query = *mq;
                r.translator = "llm:" + backend->name();
                r.unused.clear();
                r.model_note = "model translation validated (audit " + rec.id + ")";
            } else {
                r.model_note = "model output rejected (" + reject + "); deterministic keyword grammar used";
            }
        }
    }
    r.matches = run_query(report, r.query);
    r.answer = answer_text(r);
    return r;
}

std::string ask_json(const AskResult& r) {
    nlohmann::json j;
    j["query"] = nlohmann::json::parse(query_to_json(r.query));
    j["translator"] = r.translator;
    j["model_note"] = r.model_note;
    j["unused"] = r.unused;
    j["count"] = r.matches.size();
    j["matches"] = nlohmann::json::array();
    for (auto& m : r.matches) {
        const Finding& f = *m.f;
        j["matches"].push_back({{"id", m.id}, {"stage", f.stage}, {"status", f.status}, {"file", f.file},
                                {"line", f.line ? nlohmann::json(*f.line) : nlohmann::json(nullptr)},
                                {"function", f.function.value_or("")}, {"cls", f.cls}, {"message", f.message},
                                {"counterexample", f.counterexample}});
    }
    j["answer"] = r.answer;
    return j.dump(2);
}

// ---------------------------------------------------------------- explain
std::string explain_finding(const RunReport& report, const std::string& id) {
    const Finding* f = find_by_id(report, id);
    if (!f) return {};
    static const std::map<std::string, std::string> meaning = {
        {"PROVED-CERTIFIED", "the property holds for every input the model covers, and the UNSAT answer was "
                             "checked by a verified LRAT checker (cake_lpr) against the exact CNF PRISM produced"},
        {"PROVED-UNBOUNDED", "the property holds for every input and every loop iteration count (k-induction or "
                             "inductive invariants closed); the solver is trusted"},
        {"PROVED", "the property holds for every input the model covers; the solver (Z3) is trusted, no "
                   "certificate was checked"},
        {"PROVED-ASSUMING", "the property holds under the listed assumptions only; review each assumption — a "
                            "wrong one can hide a real bug"},
        {"BOUNDED", "no violation within the unwind bound; longer executions were NOT checked (Law 2: never a proof)"},
        {"FAILED", "a violation was found; the counterexample gives inputs that trigger it"},
        {"CRASH", "the program crashed on a concrete input (fuzzer / execution)"},
        {"SANFAIL", "a sanitizer reported undefined behaviour or a memory error at run time"},
        {"CLEAN", "the tool ran and found nothing; for fuzzers this is NOT a proof (Law 3)"},
        {"NOTRUN", "the check could not run (tool missing, or held back by policy); this is a gap, never a clean "
                   "result (Law 1)"},
        {"NEEDS-HARNESS", "the function takes pointers or structs; it is not model-checked without a harness or "
                          "`// requires:` preconditions (Law 6)"},
        {"HYPOTHESIS", "model output; it can guide a human but cannot cover a defect class or set a verdict (Law 4)"},
        {"READS", "read-only observation (model or reviewer); not a verdict"},
        {"UNKNOWN", "the solver or tool gave no answer; not a proof and not a bug"},
        {"TIMEOUT", "the check ran out of time; no answer"},
        {"ERROR", "the check failed to run correctly; no answer"}};
    static const std::map<std::string, std::string> strength = {
        {"PROVES", "a formal method: a proof or a real counterexample"},
        {"FINDS", "a bug finder: its findings are real, but silence proves nothing"},
        {"SOME", "partial evidence"},
        {"READS", "a reading (model or heuristic), never a verdict"}};
    std::ostringstream o;
    o << id << ": " << f->status << " from stage " << f->stage;
    if (!f->file.empty()) o << " at " << f->file << (f->line ? ":" + std::to_string(*f->line) : "");
    if (f->function && !f->function->empty()) o << " in `" << *f->function << "`";
    o << "\n";
    if (!f->cls.empty()) o << "class: " << f->cls << "\n";
    o << "message: " << f->message << "\n";
    auto it = meaning.find(f->status);
    o << "what " << f->status << " means: " << (it != meaning.end() ? it->second : "not in the verdict vocabulary")
      << "\n";
    if (auto s = strength.find(f->strength); s != strength.end())
        o << "strength " << f->strength << ": " << s->second << "\n";
    if (!f->counterexample.empty()) o << "counterexample: " << f->counterexample << "\n";
    for (const char* k : {"unwind", "assumptions", "certificate", "k_induction", "invariants", "tv", "install",
                          "ai_audit_id", "ai_checker", "ai_checker_result", "audit_original"})
        if (auto e = f->extra.find(k); e != f->extra.end() && !e->second.empty())
            o << k << ": " << e->second.substr(0, 300) << "\n";
    if (f->status == "FAILED" && !f->counterexample.empty())
        o << "next: `prism regress` turns this counterexample into a unit test that fails until it is fixed.\n";
    return o.str();
}

std::string explain_trusted_base() {
    return "Trusted base (docs/TRUSTED_BASE.md): a PROVED verdict trusts the front end (the C parser / Clang->PIR "
           "translation, checked per run by translation validation where it runs), the encoder, and the solver "
           "(Z3). PROVED-CERTIFIED replaces trust in the solver by an LRAT certificate checked by cake_lpr (a "
           "verified checker) against the exact CNF; the bit-blast to CNF and the front end stay trusted. The verdict "
           "lattice and the admission rules (which stage may emit which status) are proved in Lean "
           "(proofs/Prism/Verdict.lean). Model output (LLM invariants, harnesses, explanations, these answers) is "
           "never trusted: it is HYPOTHESIS until a checker accepts it, and every model call is in ai_audit.jsonl.";
}

std::string assistant_reply(const RunReport& report, const std::string& input, bool use_model) {
    auto in = trim(input);
    auto li = lower(in);
    if (li.empty() || li == "help" || li == "?")
        return "Ask about findings in plain words, e.g. \"unproved memory safety in module net\", \"how many "
               "FAILED by stage\", \"bounded functions in file parser.c\". Also: \"explain <id>\" (e.g. explain "
               "bmc#3) and \"trusted base\". The structured query is always shown with the answer.";
    if (li.starts_with("explain ")) {
        auto id = trim(in.substr(8));
        auto e = explain_finding(report, id);
        return e.empty() ? "no finding with id '" + id + "' (ids look like bmc#3; they are listed in answers)" : e;
    }
    if (li.find("trusted base") != std::string::npos || li.find("trusted computing base") != std::string::npos)
        return explain_trusted_base();
    return ask_question(report, in, use_model).answer;
}

// ---------------------------------------------------------------- CLI
int ask_main(int argc, char** argv) {
    fs::path report_path = "prism-out/report.json";
    std::string question;
    bool json = false, use_model = true;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--report") {
            fs::path p(next());
            report_path = fs::is_directory(p) ? p / "report.json" : p;
        } else if (a == "--json") json = true;
        else if (a == "--no-llm") use_model = false;
        else if (a == "-h" || a == "--help") {
            std::cout << "prism ask \"<question>\" [--report OUT/report.json] [--json] [--no-llm]\n"
                         "Translates the question into a structured query over report.json (keyword grammar;\n"
                         "a local model when reachable, grammar-constrained and validated) and prints the\n"
                         "query with the answer. \"explain <id>\" and \"trusted base\" are understood too.\n";
            return 0;
        } else {
            question += (question.empty() ? "" : " ") + a;
        }
    }
    auto report = RunReport::load(report_path);
    if (!report) {
        std::cerr << "ERROR ask: cannot read " << report_path.string() << "\n";
        return 2;
    }
    Config cfg = default_config();
    cfg.llm = use_model;
    cfg.resume = true;  // append to the run's ai_audit.jsonl, never truncate it
    cfg.out = fs::absolute(report_path).parent_path();
    cfg.root = report->root;
    Session session(cfg);
    auto li = lower(trim(question));
    if (json) {
        if (li.starts_with("explain ") || li.find("trusted base") != std::string::npos || li.empty() || li == "help") {
            nlohmann::json j{{"answer", assistant_reply(*report, question, use_model)}};
            std::cout << j.dump(2) << "\n";
        } else {
            std::cout << ask_json(ask_question(*report, question, use_model)) << "\n";
        }
    } else {
        std::cout << "question: " << question << "\n" << assistant_reply(*report, question, use_model);
    }
    return 0;
}

}  // namespace prism::ai
