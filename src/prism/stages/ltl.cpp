// Stage ltl: LTL specs checked on the state machine extracted from a switch.
#include "common.hpp"

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
constexpr int F_BOUND = 8;

struct PredFail : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// --- LTL ---
std::string strip_parens(std::string s) {
    s = strip(s);
    while (s.starts_with("(") && s.ends_with(")")) {
        int depth = 0;
        bool ok = true;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '(') ++depth;
            else if (s[i] == ')') {
                --depth;
                if (depth == 0 && i + 1 != s.size()) {
                    ok = false;
                    break;
                }
            }
        }
        if (!ok || depth != 0) break;
        s = strip(s.substr(1, s.size() - 2));
    }
    return s;
}

std::vector<std::string> split_top(const std::string& s, const std::string& sep) {
    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '(') {
            ++depth;
            cur.push_back(s[i++]);
        } else if (s[i] == ')') {
            --depth;
            cur.push_back(s[i++]);
        } else if (depth == 0 && i + sep.size() <= s.size() && s.compare(i, sep.size(), sep) == 0) {
            parts.push_back(cur);
            cur.clear();
            i += sep.size();
        } else {
            cur.push_back(s[i++]);
        }
    }
    parts.push_back(cur);
    std::vector<std::string> out;
    for (auto& p : parts) {
        auto t = strip(p);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

bool eval_pred(std::string pred, const std::string& state) {
    pred = strip_parens(strip(pred));
    if (pred.empty()) throw PredFail("empty predicate");
    auto low = lower_copy(pred);
    if (low == "true" || low == "1") return true;
    if (low == "false" || low == "0") return false;
    auto imps = split_top(pred, "->");
    if (imps.size() > 1) {
        std::string rest;
        for (std::size_t i = 1; i < imps.size(); ++i) {
            if (i > 1) rest += "->";
            rest += imps[i];
        }
        return !eval_pred(imps[0], state) || eval_pred(rest, state);
    }
    auto ors = split_top(pred, "||");
    if (ors.size() > 1) {
        for (auto& p : ors)
            if (eval_pred(p, state)) return true;
        return false;
    }
    auto ands = split_top(pred, "&&");
    if (ands.size() > 1) {
        for (auto& p : ands)
            if (!eval_pred(p, state)) return false;
        return true;
    }
    if (pred.starts_with("!")) return !eval_pred(pred.substr(1), state);
    static Regex eq("^state\\s*==\\s*([A-Za-z_]\\w*|\\d+)$");
    if (auto m = match_at(eq, pred)) return state == m->group(1);
    static Regex ne("^state\\s*!=\\s*([A-Za-z_]\\w*|\\d+)$");
    if (auto m = match_at(ne, pred)) {
        auto tokv = m->group(1);
        auto up = tokv;
        for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (up == "BAD" || up == "ERROR") {
            auto su = state;
            for (auto& c : su) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return su.find("BAD") == std::string::npos && su.find("ERROR") == std::string::npos;
        }
        return state != tokv;
    }
    static Regex ident("^[A-Za-z_]\\w*$");
    if (fullmatch(ident, pred)) {
        auto up = pred;
        for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (up == "BAD" || up == "ERROR") {
            auto su = state;
            for (auto& c : su) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return su.find("BAD") != std::string::npos || su.find("ERROR") != std::string::npos;
        }
        return state == pred;
    }
    throw PredFail(pred);
}

struct Fsm {
    std::vector<std::string> states, cases, assigns;
    std::vector<std::pair<std::string, std::string>> transitions;
};

std::optional<Fsm> extract_fsm(const std::string& body) {
    if (body.find("switch") == std::string::npos || body.find("state") == std::string::npos) return std::nullopt;
    static Regex case_re("case\\s+([A-Za-z_]\\w*|\\d+)\\s*:");
    static Regex asg_re("\\bstate\\s*=\\s*([A-Za-z_]\\w*|\\d+)");
    std::vector<std::string> cases;
    for (auto& m : case_re.finditer(body)) cases.push_back(m.group(1));
    std::vector<std::string> assigns;
    for (auto& m : asg_re.finditer(body)) assigns.push_back(m.group(1));
    if (cases.size() < 2) return std::nullopt;
    std::vector<std::pair<std::string, std::string>> trans;
    static Regex split_re("\\bcase\\s+([A-Za-z_]\\w*|\\d+)\\s*:");
    std::vector<std::pair<std::string, std::string>> chunks;
    std::size_t last = 0;
    std::string last_lab;
    bool have = false;
    for (auto& m : split_re.finditer(body)) {
        if (have) chunks.push_back({last_lab, body.substr(last, static_cast<std::size_t>(m.spans[0].first) - last)});
        last_lab = m.group(1);
        last = static_cast<std::size_t>(m.spans[0].second);
        have = true;
    }
    if (have) chunks.push_back({last_lab, body.substr(last)});
    for (auto& [lab, content0] : chunks) {
        auto content = content0;
        auto def = content.find("default");
        static Regex defre("\\bdefault\\s*:");
        if (auto dm = defre.search_match(content))
            content = content.substr(0, static_cast<std::size_t>(dm->spans[0].first));
        std::vector<std::string> dests;
        for (auto& m : asg_re.finditer(content)) dests.push_back(m.group(1));
        if (dests.empty()) trans.emplace_back(lab, lab);
        else {
            for (auto& d : dests) trans.emplace_back(lab, d);
            if (re_search("\\bif\\b", content) && !re_search("\\belse\\b", content)) trans.emplace_back(lab, lab);
        }
    }
    std::set<std::string> stset(cases.begin(), cases.end());
    for (auto& a : assigns) stset.insert(a);
    for (auto& [a, b] : trans) {
        stset.insert(a);
        stset.insert(b);
    }
    std::set<std::string> has_out;
    for (auto& [s, _] : trans) has_out.insert(s);
    std::vector<std::string> states(stset.begin(), stset.end());
    for (auto& s : states)
        if (!has_out.contains(s)) trans.emplace_back(s, s);
    return Fsm{states, cases, assigns, trans};
}

enum class LtlKind {
    Invariant, Next, BoundedF, Nonsafety, GfApprox, FgApprox, UntilApprox, FApprox
};

struct LtlClass {
    LtlKind kind = LtlKind::Nonsafety;
    std::string a, b;
    int k = 0;
    std::string raw;
};

constexpr std::string_view kStrixNote =
    "strix realizability is not PROVED unless the formula is in PRISM's "
    "safety fragment (G p, G (p -> X q), G (req -> F_k ack), G (F_k p))";

bool ltl_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

const Regex& ltl_until_re() {
    static Regex until("(?<![A-Za-z_])U(?![A-Za-z_])");
    return until;
}

const Regex& ltl_gflive_re() {
    static Regex gflive("\\bG\\s*F\\b|\\bF\\s*G\\b|\\bGF\\b|\\bFG\\b");
    return gflive;
}

std::optional<std::pair<std::string, std::string>> split_until(const std::string& s) {
    int depth = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') --depth;
        else if (depth == 0 && s[i] == 'U') {
            bool prev_ok = i == 0 || !ltl_word_char(s[i - 1]);
            bool next_ok = i + 1 >= s.size() || !ltl_word_char(s[i + 1]);
            if (prev_ok && next_ok) {
                auto left = strip(s.substr(0, i));
                auto right = strip(s.substr(i + 1));
                if (!left.empty() && !right.empty() && !ltl_until_re().search(left) &&
                    !ltl_until_re().search(right))
                    return std::pair{left, right};
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

std::optional<LtlClass> g_f_inner(const std::string& inner0, const std::string& raw) {
    auto inner = strip_parens(inner0);
    if (split_top(inner, "->").size() == 2) return std::nullopt;
    static Regex fm("^F(?:_(\\d+))?\\s*(.+)$", false, true);
    auto m = match_at(fm, inner);
    if (!m) return std::nullopt;
    auto rest = strip_parens(strip(m->group(2)));
    if (rest.empty() || ltl_until_re().search(rest) || ltl_gflive_re().search(rest))
        return std::nullopt;
    if (!m->group(1).empty())
        return LtlClass{LtlKind::BoundedF, "true", rest, std::stoi(m->group(1)), raw};
    return LtlClass{LtlKind::GfApprox, rest, {}, F_BOUND, raw};
}

std::optional<LtlClass> liveness_approx(const std::string& raw) {
    auto s = strip_parens(strip(raw));
    static Regex gf("^(?:G\\s*F|GF)\\s*(.+)$");
    if (auto m = match_at(gf, s)) {
        auto inner = strip_parens(strip(m->group(1)));
        if (!inner.empty() && !ltl_until_re().search(inner) && !ltl_gflive_re().search(inner))
            return LtlClass{LtlKind::GfApprox, inner, {}, F_BOUND, raw};
    }
    static Regex fg("^(?:F\\s*G|FG)\\s*(.+)$");
    if (auto m = match_at(fg, s)) {
        auto inner = strip_parens(strip(m->group(1)));
        if (!inner.empty() && !ltl_until_re().search(inner) && !ltl_gflive_re().search(inner))
            return LtlClass{LtlKind::FgApprox, inner, {}, F_BOUND, raw};
    }
    static Regex gstart("^G\\b");
    if (match_at(gstart, s)) {
        static Regex safety("^G\\s*\\((.+)\\)$");
        static Regex gbare("^G\\s+(.+)$");
        auto m = match_at(safety, s);
        if (!m) m = match_at(gbare, s);
        if (m) return g_f_inner(strip_parens(m->group(1)), raw);
        return std::nullopt;
    }
    if (auto parts = split_until(s)) {
        if (!ltl_gflive_re().search(parts->first) && !ltl_gflive_re().search(parts->second))
            return LtlClass{LtlKind::UntilApprox, parts->first, parts->second, F_BOUND, raw};
    }
    return std::nullopt;
}

bool is_approx_kind(LtlKind k) {
    return k == LtlKind::GfApprox || k == LtlKind::FgApprox || k == LtlKind::UntilApprox ||
           k == LtlKind::FApprox;
}

LtlClass classify_ltl(const std::string& formula) {
    auto raw = strip(formula);
    if (ltl_until_re().search(raw) || ltl_gflive_re().search(raw)) {
        if (auto a = liveness_approx(raw)) return *a;
        return {LtlKind::Nonsafety, {}, {}, 0, raw};
    }
    static Regex safety("^G\\s*\\((.+)\\)$");
    static Regex gbare("^G\\s+(.+)$");
    auto m = match_at(safety, raw);
    if (!m) m = match_at(gbare, raw);
    if (!m) {
        static Regex fx("^F\\b|^X\\b");
        if (match_at(fx, raw)) {
            if (auto a = liveness_approx(raw)) return *a;
        }
        return {LtlKind::Nonsafety, {}, {}, 0, raw};
    }
    auto inner = strip_parens(m->group(1));
    auto parts = split_top(inner, "->");
    if (parts.size() == 2) {
        auto lhs = strip(parts[0]);
        auto rhs = strip(parts[1]);
        static Regex xm("^X\\s*\\(?(.+?)\\)?$", false, true);
        if (auto xm_ = match_at(xm, rhs))
            return {LtlKind::Next, lhs, strip_parens(xm_->group(1)), 0, raw};
        static Regex fm("^F(?:_(\\d+))?\\s*\\(?(.+?)\\)?$", false, true);
        if (auto fm_ = match_at(fm, rhs)) {
            auto ack = strip_parens(fm_->group(2));
            if (!fm_->group(1).empty())
                return {LtlKind::BoundedF, lhs, ack, std::stoi(fm_->group(1)), raw};
            return {LtlKind::FApprox, lhs, ack, F_BOUND, raw};
        }
        return {LtlKind::Invariant, inner, {}, 0, raw};
    }
    if (auto gf = g_f_inner(inner, raw)) return *gf;
    static Regex lonef("(?<![A-Za-z_])F(?:_\\d+)?(?![A-Za-z_])");
    if (lonef.search(inner)) return {LtlKind::Nonsafety, {}, {}, 0, raw};
    return {LtlKind::Invariant, inner, {}, 0, raw};
}

Finding ltl_finding(std::string_view status, const std::string& formula, const std::string& message,
                   std::map<std::string, std::string> extra, std::string_view strength = laws::STRENGTH_PROVES) {
    Finding f;
    f.stage = "ltl";
    f.status = std::string(status);
    f.cls = "LTL-SAFETY";
    f.message = message;
    f.strength = std::string(strength);
    extra["formula"] = formula;
    f.extra = std::move(extra);
    return f;
}

Finding check_g(const std::string& pred, const Fsm& fsm, const std::string& formula) {
    std::vector<std::string> bad;
    for (auto& s : fsm.states)
        if (!eval_pred(pred, s)) bad.push_back(s);
    if (!bad.empty()) {
        std::set<std::string> live(fsm.assigns.begin(), fsm.assigns.end());
        live.insert(fsm.cases.begin(), fsm.cases.end());
        std::vector<std::string> live_bad;
        if (!live.empty()) {
            for (auto& s : bad)
                if (live.contains(s)) live_bad.push_back(s);
        } else {
            live_bad = bad;
        }
        if (!live_bad.empty()) {
            bool errst = false;
            for (auto& x : live_bad) {
                auto u = x;
                for (auto& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (u.find("BAD") != std::string::npos || u.find("ERROR") != std::string::npos) errst = true;
            }
            std::string msg = errst ? "formula " + formula + " violated: FSM assigns an error state"
                                    : "formula " + formula + " violated on states " + join_sv(live_bad, ", ");
            return ltl_finding(laws::FAILED, formula, msg, {});
        }
    }
    return ltl_finding(laws::PROVED, formula, "safety " + formula + " holds on extracted FSM", {});
}

std::optional<Finding> synthesize_missing(const Fsm& fsm, const std::string& formula) {
    auto cl = classify_ltl(formula);
    if (cl.kind != LtlKind::Next) return std::nullopt;
    try {
        for (auto& [s, sp] : fsm.transitions)
            if (eval_pred(cl.a, s) && !eval_pred(cl.b, sp)) return std::nullopt;
        std::map<std::string, std::vector<std::string>> succ;
        for (auto& [s, sp] : fsm.transitions) succ[s].push_back(sp);
        std::vector<std::string> incomplete, q_states;
        std::vector<std::pair<std::string, std::string>> synthesis;
        for (auto& t : fsm.states)
            if (eval_pred(cl.b, t)) q_states.push_back(t);
        for (auto& s : fsm.states) {
            if (!eval_pred(cl.a, s)) continue;
            auto& outs = succ[s];
            bool any = false;
            for (auto& sp : outs)
                if (eval_pred(cl.b, sp)) any = true;
            if (outs.empty() || !any) {
                incomplete.push_back(s);
                for (auto& t : q_states) synthesis.emplace_back(s, t);
            }
        }
        if (incomplete.empty()) return std::nullopt;
        nlohmann::json syn = nlohmann::json::array();
        for (std::size_t i = 0; i < synthesis.size() && i < 24; ++i)
            syn.push_back(nlohmann::json::array({synthesis[i].first, synthesis[i].second}));
        std::vector<std::string> inc = incomplete;
        if (inc.size() > 12) inc.resize(12);
        auto msg_inc = incomplete;
        if (msg_inc.size() > 6) msg_inc.resize(6);
        return ltl_finding(laws::HYPOTHESIS, formula,
                           "missing transition from " + join_sv(msg_inc, ", ") +
                               " to a state satisfying q; synthesis is HYPOTHESIS",
                           {{"synthesis", syn.dump()}, {"incomplete", nlohmann::json(inc).dump()}},
                           laws::STRENGTH_READS);
    } catch (const PredFail&) {
        return std::nullopt;
    }
}

Finding check_next(const std::string& p, const std::string& q, const Fsm& fsm, const std::string& formula) {
    std::vector<std::pair<std::string, std::string>> viol;
    for (auto& [s, sp] : fsm.transitions)
        if (eval_pred(p, s) && !eval_pred(q, sp)) viol.emplace_back(s, sp);
    if (!viol.empty()) {
        nlohmann::json j = nlohmann::json::array();
        for (std::size_t i = 0; i < viol.size() && i < 12; ++i)
            j.push_back(nlohmann::json::array({viol[i].first, viol[i].second}));
        std::string vs;
        for (std::size_t i = 0; i < viol.size() && i < 6; ++i) {
            if (i) vs += ", ";
            vs += viol[i].first + "->" + viol[i].second;
        }
        return ltl_finding(laws::FAILED, formula, "formula " + formula + " violated on transitions " + vs,
                           {{"violations", j.dump()}});
    }
    if (auto syn = synthesize_missing(fsm, formula)) return *syn;
    return ltl_finding(laws::PROVED, formula, "safety " + formula + " holds on extracted FSM", {});
}

bool avoids_ack(const std::string& start, const std::string& ack,
                const std::map<std::string, std::vector<std::string>>& succ, int k) {
    if (eval_pred(ack, start)) return false;
    std::deque<std::pair<std::string, int>> q;
    std::set<std::pair<std::string, int>> seen;
    q.push_back({start, 0});
    seen.insert({start, 0});
    while (!q.empty()) {
        auto [s, d] = q.front();
        q.pop_front();
        if (d >= k) return true;
        auto it = succ.find(s);
        std::vector<std::string> nxt = it == succ.end() || it->second.empty() ? std::vector<std::string>{s}
                                                                             : it->second;
        bool progressed = false;
        for (auto& sp : nxt) {
            if (eval_pred(ack, sp)) {
                progressed = true;
                continue;
            }
            progressed = true;
            auto key = std::pair<std::string, int>{sp, d + 1};
            if (!seen.contains(key)) {
                seen.insert(key);
                q.push_back(key);
            }
        }
        if (!progressed && d < k) return true;
    }
    return false;
}

Finding check_bounded_f(const std::string& req, const std::string& ack, int k, const Fsm& fsm,
                        const std::string& formula) {
    std::map<std::string, std::vector<std::string>> succ;
    for (auto& [s, sp] : fsm.transitions) succ[s].push_back(sp);
    for (auto& s : fsm.states)
        if (!succ.contains(s)) succ[s] = {s};
    std::vector<std::string> viol;
    for (auto& s : fsm.states) {
        if (!eval_pred(req, s)) continue;
        if (avoids_ack(s, ack, succ, k)) viol.push_back(s);
    }
    if (!viol.empty())
        return ltl_finding(laws::FAILED, formula,
                           "formula " + formula + " violated: from " + join_sv(viol, ", ") +
                               " ack is avoidable within F_" + std::to_string(k),
                           {});
    return ltl_finding(laws::PROVED, formula,
                       "safety " + formula + " holds on extracted FSM (F bound " + std::to_string(k) + ")",
                       {{"k", std::to_string(k)}});
}

std::map<std::string, std::vector<std::string>> ltl_succ_map(const Fsm& fsm) {
    std::map<std::string, std::vector<std::string>> succ;
    for (auto& [s, sp] : fsm.transitions) succ[s].push_back(sp);
    for (auto& s : fsm.states)
        if (!succ.contains(s)) succ[s] = {s};
    return succ;
}

bool invariant_from(const std::string& pred, const std::string& start,
                    const std::map<std::string, std::vector<std::string>>& succ) {
    std::set<std::string> seen;
    std::vector<std::string> stack{start};
    while (!stack.empty()) {
        auto s = stack.back();
        stack.pop_back();
        if (seen.contains(s)) continue;
        seen.insert(s);
        if (!eval_pred(pred, s)) return false;
        auto it = succ.find(s);
        auto nxt = it == succ.end() || it->second.empty() ? std::vector<std::string>{s} : it->second;
        stack.insert(stack.end(), nxt.begin(), nxt.end());
    }
    return true;
}

bool avoids_states(const std::string& start, const std::set<std::string>& targets,
                   const std::map<std::string, std::vector<std::string>>& succ, int k) {
    if (targets.contains(start)) return false;
    std::deque<std::pair<std::string, int>> q;
    std::set<std::pair<std::string, int>> seen;
    q.push_back({start, 0});
    seen.insert({start, 0});
    while (!q.empty()) {
        auto [s, d] = q.front();
        q.pop_front();
        if (d >= k) return true;
        auto it = succ.find(s);
        auto nxt = it == succ.end() || it->second.empty() ? std::vector<std::string>{s} : it->second;
        bool progressed = false;
        for (auto& sp : nxt) {
            progressed = true;
            if (targets.contains(sp)) continue;
            auto key = std::pair<std::string, int>{sp, d + 1};
            if (!seen.contains(key)) {
                seen.insert(key);
                q.push_back(key);
            }
        }
        if (!progressed && d < k) return true;
    }
    return false;
}

Finding approx_finding(Finding f, const std::string& original, const std::string& approx,
                       const std::string& kind) {
    f.extra["formula"] = original;
    f.extra["safety_approx"] = approx;
    f.extra["approx_kind"] = kind;
    f.extra["strix_not_proved"] = "true";
    f.extra["strix_note"] = std::string(kStrixNote);
    f.extra["approx_note"] =
        "safety approximation of unbounded " + kind + "; not PROVED of the original formula";
    if (f.status == laws::PROVED) {
        f.status = std::string(laws::BOUNDED);
        f.message = "safety approximation " + approx + " of " + original +
                    " holds on extracted FSM; not a proof of unbounded " + kind;
    } else if (f.status == laws::FAILED) {
        f.message = "safety approximation " + approx + " of " + original + " violated";
    }
    return f;
}

Finding check_fg_approx(const std::string& pred, int k, const Fsm& fsm, const std::string& formula) {
    auto succ = ltl_succ_map(fsm);
    std::set<std::string> good;
    for (auto& s : fsm.states)
        if (invariant_from(pred, s, succ)) good.insert(s);
    std::vector<std::string> viol;
    for (auto& s : fsm.states)
        if (avoids_states(s, good, succ, k)) viol.push_back(s);
    auto approx = "F_" + std::to_string(k) + " (G (" + pred + "))";
    std::map<std::string, std::string> extra{
        {"safety_approx", approx},
        {"approx_kind", "FG"},
        {"k", std::to_string(k)},
        {"strix_not_proved", "true"},
        {"strix_note", std::string(kStrixNote)},
        {"approx_note", "safety approximation of unbounded FG; not PROVED of the original formula"},
    };
    if (!viol.empty())
        return ltl_finding(laws::FAILED, formula,
                           "safety approximation " + approx + " of " + formula + " violated from " +
                               join_sv(viol, ", "),
                           extra);
    return ltl_finding(laws::BOUNDED, formula,
                       "safety approximation " + approx + " of " + formula +
                           " holds on extracted FSM; not a proof of unbounded FG",
                       extra);
}

std::string until_from(const std::string& start, const std::string& p, const std::string& q,
                       const std::map<std::string, std::vector<std::string>>& succ, int k) {
    if (eval_pred(q, start)) return "ok";
    if (!eval_pred(p, start)) return "real";
    std::deque<std::pair<std::string, int>> dq;
    std::set<std::pair<std::string, int>> seen;
    dq.push_back({start, 0});
    seen.insert({start, 0});
    bool saw_bound = false;
    while (!dq.empty()) {
        auto [s, d] = dq.front();
        dq.pop_front();
        if (d >= k) {
            saw_bound = true;
            continue;
        }
        auto it = succ.find(s);
        auto nxt = it == succ.end() || it->second.empty() ? std::vector<std::string>{s} : it->second;
        for (auto& sp : nxt) {
            if (eval_pred(q, sp)) continue;
            if (!eval_pred(p, sp)) return "real";
            auto key = std::pair<std::string, int>{sp, d + 1};
            if (!seen.contains(key)) {
                seen.insert(key);
                dq.push_back(key);
            }
        }
    }
    return saw_bound ? "bound" : "ok";
}

Finding check_until_approx(const std::string& p, const std::string& q, int k, const Fsm& fsm,
                           const std::string& formula) {
    auto succ = ltl_succ_map(fsm);
    std::vector<std::string> real, bound;
    for (auto& s : fsm.states) {
        auto hit = until_from(s, p, q, succ, k);
        if (hit == "real") real.push_back(s);
        else if (hit == "bound") bound.push_back(s);
    }
    auto approx = "(" + p + ") U_" + std::to_string(k) + " (" + q + ")";
    std::map<std::string, std::string> extra{
        {"safety_approx", approx},
        {"approx_kind", "UNTIL"},
        {"k", std::to_string(k)},
        {"strix_not_proved", "true"},
        {"strix_note", std::string(kStrixNote)},
        {"approx_note", "safety approximation of unbounded U; not PROVED of the original formula"},
    };
    if (!real.empty())
        return ltl_finding(laws::FAILED, formula,
                           "formula " + formula + " violated: " + p + " U " + q + " fails from " +
                               join_sv(real, ", ") + " (\xC2\xACp \xE2\x88\xA7 \xC2\xACq before q)",
                           extra);
    if (!bound.empty())
        return ltl_finding(laws::FAILED, formula,
                           "safety approximation " + approx + " of " + formula + " violated from " +
                               join_sv(bound, ", "),
                           extra);
    return ltl_finding(laws::BOUNDED, formula,
                       "safety approximation " + approx + " of " + formula +
                           " holds on extracted FSM; not a proof of unbounded U",
                       extra);
}

std::optional<Finding> check_safety(const std::string& formula, const Fsm& fsm) {
    auto cl = classify_ltl(formula);
    try {
        if (cl.kind == LtlKind::Invariant) return check_g(cl.a, fsm, formula);
        if (cl.kind == LtlKind::Next) return check_next(cl.a, cl.b, fsm, formula);
        if (cl.kind == LtlKind::BoundedF) return check_bounded_f(cl.a, cl.b, cl.k, fsm, formula);
        if (cl.kind == LtlKind::FApprox) {
            auto f = check_bounded_f(cl.a, cl.b, cl.k, fsm, formula);
            return approx_finding(f, formula,
                                  "G ((" + cl.a + ") -> F_" + std::to_string(cl.k) + " (" + cl.b + "))",
                                  "F");
        }
        if (cl.kind == LtlKind::GfApprox) {
            auto f = check_bounded_f("true", cl.a, cl.k, fsm, formula);
            return approx_finding(f, formula, "G (F_" + std::to_string(cl.k) + " (" + cl.a + "))", "GF");
        }
        if (cl.kind == LtlKind::FgApprox) return check_fg_approx(cl.a, cl.k, fsm, formula);
        if (cl.kind == LtlKind::UntilApprox) return check_until_approx(cl.a, cl.b, cl.k, fsm, formula);
    } catch (const PredFail&) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::vector<std::string> parse_ltl_file(const fs::path& path) {
    if (!fs::exists(path)) return {};
    std::vector<std::string> out;
    std::istringstream ss(read_text_file(path));
    std::string ln;
    while (std::getline(ss, ln)) {
        ln = strip(ln);
        if (ln.empty() || ln.starts_with("#") || ln.starts_with("//")) continue;
        out.push_back(ln);
    }
    return out;
}

}  // namespace

std::vector<Finding> run_ltl(const std::vector<FunctionInfo>& functions, const std::vector<fs::path>& specs) {
    std::vector<std::string> formulas;
    for (auto& p : specs)
        for (auto& f : parse_ltl_file(p)) formulas.push_back(f);
    if (formulas.empty()) {
        Finding f;
        f.stage = "ltl";
        f.status = std::string(laws::NOTRUN);
        f.cls = "LTL-SAFETY";
        f.message = "no .ltl spec next to the sources";
        f.strength = std::string(laws::STRENGTH_PROVES);
        f.extra["install"] = "add a file with G (...)";
        return {f};
    }
    std::vector<std::pair<FunctionInfo, Fsm>> fsms;
    for (auto& fn : functions)
        if (auto fsm = extract_fsm(fn.body)) fsms.emplace_back(fn, *fsm);
    Config cfg;
    auto strix = cfg.which({"strix"});
    std::vector<Finding> out;
    for (auto& formula : formulas) {
        bool decided = false;
        auto kind = classify_ltl(formula);
        for (auto& [fn, fsm] : fsms) {
            auto f = check_safety(formula, fsm);
            if (!f) f = synthesize_missing(fsm, formula);
            if (f) {
                f->file = fn.file;
                f->function = fn.name;
                f->line = fn.line;
                out.push_back(*f);
                decided = true;
            }
        }
        if (!decided) {
            std::string msg;
            if (is_approx_kind(kind.kind))
                msg = formula + ": known liveness pattern but predicates were not evaluable on this FSM";
            else if (kind.kind != LtlKind::Nonsafety) {
                if (fsms.empty())
                    msg = formula + ": in the safety fragment but no switch(state) FSM extracted";
                else
                    msg = formula + ": in the safety fragment but predicates were not evaluable on this FSM";
            } else if (strix)
                msg = formula +
                      ": not in the safety fragment PRISM decides "
                      "(G p, G (p -> X q), G (req -> F_" +
                      std::to_string(F_BOUND) + " ack), G (F_" + std::to_string(F_BOUND) +
                      " p)); strix is present but PRISM does not treat strix output as PROVED";
            else
                msg = formula + ": not in the safety fragment PRISM decides; missing Strix binary";
            Finding f;
            f.stage = "ltl";
            f.status = std::string(laws::NOTRUN);
            f.cls = "LTL-SAFETY";
            f.message = msg;
            f.strength = std::string(laws::STRENGTH_PROVES);
            f.extra["formula"] = formula;
            f.extra["install"] = "https://github.com/meyerphi/strix";
            f.extra["strix"] = strix ? strix->string() : "";
            f.extra["strix_not_proved"] = "true";
            f.extra["strix_note"] = std::string(kStrixNote);
            out.push_back(std::move(f));
        }
    }
    return out;
}

}  // namespace prism
