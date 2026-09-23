// solve(): query cache, scheduler, parallel portfolio (Z3 in-process, Bitwuzla
// on SMT-LIB2, CaDiCaL / Kissat on DIMACS, ProbSAT walker), and certified mode
// (CaDiCaL LRAT proof of the exact CNF, checked by cake_lpr).
//
// Rules this file keeps (roadmap 3.1-3.3, Laws 1 and 7):
//  - a SAT answer from ANY member is accepted only after its model evaluates
//    the original formula to true in Z3 (validate_model);
//  - the ProbSAT walker can only ever report Sat (a counterexample);
//  - certified=true only when a solver said UNSAT AND cake_lpr printed
//    "s VERIFIED UNSAT" for the LRAT proof against the exact CNF file whose
//    sha256 is recorded; every other path leaves certified=false with a note;
//  - a cached plain Unsat never answers a certified request, and a cached
//    certified Unsat is re-checked by cake_lpr against a freshly bit-blasted CNF;
//  - a missing solver binary does not participate and is named in `missing`.

#ifdef PRISM_HAS_Z3

#include "prism/solver.hpp"
#include "internal.hpp"
#include "query.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace prism::solver {

namespace {

using detail::collect_consts;
using detail::const_name;
using detail::now_s;

using Kind = SolveResult::Kind;

std::string join(const std::vector<std::string>& v, std::string_view sep = ", ") {
    std::string out;
    for (const auto& s : v) {
        if (!out.empty()) out += sep;
        out += s;
    }
    return out;
}

// ------------------------------------------------------------------ cache
fs::path cache_root(const SolveOptions& o) {
    return o.cache_dir.empty() ? fs::path(default_cache_dir()) : fs::path(o.cache_dir);
}
fs::path entry_path(const fs::path& root, const std::string& h) {
    return root / "queries" / h.substr(0, 2) / (h + ".json");
}

std::optional<json> cache_load(const fs::path& root, const std::string& h) {
    auto txt = detail::read_file(entry_path(root, h));
    if (txt.empty()) return std::nullopt;
    try {
        auto j = json::parse(txt);
        if (j.value("schema", 0) != 1 || j.value("hash", "") != h) return std::nullopt;
        return j;
    } catch (...) {
        return std::nullopt;
    }
}

// ------------------------------------------------------------------ history
std::mutex g_history_mu;

json history_load(const fs::path& root) {
    auto txt = detail::read_file(root / "solve_times.json");
    try {
        if (!txt.empty()) {
            auto j = json::parse(txt);
            if (j.is_object() && j.value("schema", 0) == 1) return j;
        }
    } catch (...) {
    }
    return json{{"schema", 1}, {"buckets", json::object()}};
}

// ------------------------------------------------------------------ SMT-LIB output
struct Sexp {
    std::string atom;
    std::vector<Sexp> kids;
    bool list = false;
    std::string text() const {
        if (!list) return atom;
        std::string s = "(";
        for (std::size_t i = 0; i < kids.size(); ++i) s += (i ? " " : "") + kids[i].text();
        return s + ")";
    }
};

bool parse_sexp(std::string_view s, std::size_t& i, Sexp& out, int depth = 0) {
    if (depth > 64) return false;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size()) return false;
    if (s[i] == '(') {
        out.list = true;
        ++i;
        for (;;) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            if (i >= s.size()) return false;
            if (s[i] == ')') { ++i; return true; }
            Sexp k;
            if (!parse_sexp(s, i, k, depth + 1)) return false;
            out.kids.push_back(std::move(k));
        }
    }
    if (s[i] == ')') return false;
    if (s[i] == '|') {
        auto e = s.find('|', i + 1);
        if (e == std::string_view::npos) return false;
        out.atom = std::string(s.substr(i + 1, e - i - 1));
        i = e + 1;
        return true;
    }
    std::size_t b = i;
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '(' && s[i] != ')') ++i;
    out.atom = std::string(s.substr(b, i - b));
    return true;
}

struct SmtAnswer {
    std::string status;  // sat / unsat / unknown / "" (none)
    std::map<std::string, std::string> model;
};

SmtAnswer parse_smt_output(std::string_view out) {
    SmtAnswer a;
    std::size_t s = 0;
    while (s < out.size()) {
        std::size_t e = out.find('\n', s);
        if (e == std::string_view::npos) e = out.size();
        std::string line(out.substr(s, e - s));
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        std::size_t next = e + 1;
        if (a.status.empty() && (line == "sat" || line == "unsat" || line == "unknown")) {
            a.status = line;
            if (line == "sat") {
                std::size_t i = next;
                Sexp root;
                if (parse_sexp(out, i, root) && root.list)
                    for (const auto& pair : root.kids)
                        if (pair.list && pair.kids.size() == 2 && !pair.kids[0].list)
                            a.model[pair.kids[0].atom] = pair.kids[1].text();
            }
            break;
        }
        s = next;
    }
    return a;
}

bool plain_symbol(const std::string& n) {
    return !n.empty() && n.find('|') == std::string::npos && n.find('\\') == std::string::npos;
}

// ------------------------------------------------------------------ the board
enum class MemberKind { Z3, Smt2, Dimacs, Sls };

struct Member {
    std::string name;
    MemberKind kind;
    std::vector<std::string> argv;  // "{input}" / "{proof}" placeholders
    std::string version;
    double est = 1.0;
    double not_before = 0.0;        // scheduler head start for a historic winner
    bool lrat = false;              // certified CaDiCaL: writes the LRAT proof
    // runtime
    bool started = false, finished = false;
    z3::context* zctx = nullptr;    // in-process Z3 job: interrupted on stop
    std::unique_ptr<std::atomic<bool>> stop = std::make_unique<std::atomic<bool>>(false);
};

struct Msg {
    enum Type { Result, CnfReady, CnfFailed } type = Result;
    std::size_t member = 0;
    Kind kind = Kind::Unknown;
    std::map<std::string, std::string> model;
    std::string detail;
    double secs = 0;
};

class Board {
public:
    void post(Msg m) {
        {
            std::lock_guard<std::mutex> g(mu_);
            q_.push_back(std::move(m));
        }
        cv_.notify_all();
    }
    // false on deadline
    bool wait(Msg& out, double deadline) {
        std::unique_lock<std::mutex> g(mu_);
        for (;;) {
            if (!q_.empty()) {
                out = std::move(q_.front());
                q_.pop_front();
                return true;
            }
            double left = deadline - now_s();
            if (left <= 0) return false;
            cv_.wait_for(g, std::chrono::duration<double>(std::min(left, 0.05)));
        }
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Msg> q_;
};

std::string subst(const std::string& a, const fs::path& input, const fs::path& proof) {
    std::string s = a;
    for (auto [k, v] : {std::pair<std::string, std::string>{"{input}", input.string()},
                        {"{proof}", proof.string()}}) {
        for (std::size_t p; (p = s.find(k)) != std::string::npos;) s.replace(p, k.size(), v);
    }
    return s;
}

// Map a solver's SAT assignment back to names, checking it against the CNF first.
Msg dimacs_result(std::size_t idx, const detail::Proc& p, const Cnf& cnf) {
    Msg m;
    m.member = idx;
    m.secs = p.secs;
    bool says_sat = p.rc == 10 || p.out.find("\ns SATISFIABLE") != std::string::npos ||
                    p.out.rfind("s SATISFIABLE", 0) == 0;
    bool says_unsat = p.rc == 20 || p.out.find("s UNSATISFIABLE") != std::string::npos;
    if (p.failed) { m.kind = Kind::Error; m.detail = p.out; return m; }
    if (p.cancelled) { m.kind = Kind::Unknown; m.detail = "cancelled"; return m; }
    if (p.timed_out) { m.kind = Kind::Timeout; m.detail = "timed out"; return m; }
    if (says_sat && !says_unsat) {
        auto a = parse_sat_values(p.out, cnf.num_vars);
        if (!a) { m.kind = Kind::Error; m.detail = "SAT without a readable assignment"; return m; }
        if (!assignment_satisfies(cnf, *a)) {
            m.kind = Kind::Error;
            m.detail = "reported assignment does not satisfy the CNF";
            return m;
        }
        m.kind = Kind::Sat;
        m.model = model_from_assignment(cnf, *a);
        return m;
    }
    if (says_unsat && !says_sat) { m.kind = Kind::Unsat; return m; }
    m.kind = Kind::Unknown;
    m.detail = "no answer (rc " + std::to_string(p.rc) + ")";
    return m;
}

Msg smt_result(std::size_t idx, const detail::Proc& p) {
    Msg m;
    m.member = idx;
    m.secs = p.secs;
    if (p.failed) { m.kind = Kind::Error; m.detail = p.out; return m; }
    if (p.cancelled) { m.kind = Kind::Unknown; m.detail = "cancelled"; return m; }
    if (p.timed_out) { m.kind = Kind::Timeout; m.detail = "timed out"; return m; }
    auto a = parse_smt_output(p.out);
    if (a.status == "sat") { m.kind = Kind::Sat; m.model = std::move(a.model); return m; }
    if (a.status == "unsat") { m.kind = Kind::Unsat; return m; }
    m.kind = Kind::Unknown;
    m.detail = a.status.empty() ? "no answer (rc " + std::to_string(p.rc) + ")" : a.status;
    return m;
}

// Rule-based expected seconds (lower starts first), before history.
double rule_estimate(const std::string& solver, const Features& ft) {
    const bool wide = ft.max_bv_width > 32;
    const bool big = ft.nodes > 2000;
    if (solver == "z3") return (ft.arrays || ft.uf || ft.arith || ft.quant) ? 0.5 : (big || wide) ? 1.2 : 0.6;
    if (solver == "bitwuzla") return (ft.fp || wide || ft.bv_mul_div || big) ? 0.4 : 0.8;
    if (solver == "cadical") return (ft.bv_mul_div && wide) ? 2.0 : big ? 0.9 : 1.1;
    if (solver == "kissat") return (ft.bv_mul_div && wide) ? 2.2 : big ? 0.95 : 1.2;
    if (solver == "sls") return 3.0;  // a counterexample finder only
    return 1.5;
}

struct Certify {
    bool certified = false;
    std::string info;
    std::string note;
};

bool has_empty_clause(const Cnf& cnf) {
    return std::any_of(cnf.clauses.begin(), cnf.clauses.end(), [](const auto& c) { return c.empty(); });
}

// cake_lpr decides. lrat-check (drat-trim), when present, is a second opinion
// that can only veto. It insists on the empty clause being derived in the
// proof, so for a CNF that already contains the empty clause (the simplifier
// decided the goal) it is not applicable and says so in the info.
Certify run_checkers(const SolveOptions& opt, const fs::path& cnf_path, const std::string& cnf_sha,
                     const fs::path& lrat, const std::string& solver_desc, double budget,
                     bool empty_clause) {
    Certify c;
    auto cake = find_tool("cake_lpr", opt);
    if (!cake) {
        c.note = "not certified: cake_lpr not found (NOTRUN)";
        return c;
    }
    std::error_code ec;
    if (!fs::is_regular_file(lrat, ec)) {
        c.note = "not certified: no LRAT proof file";
        return c;
    }
    auto o = check_lrat(*cake, cnf_path, lrat, budget);
    // The CNF the checker read must still be the CNF that was bit-blasted.
    const auto after = sha256_file(cnf_path);
    if (after != cnf_sha) {
        c.note = "not certified: CNF file changed during checking";
        return c;
    }
    if (!o.ran || !o.verified) {
        c.note = "not certified: cake_lpr " + (o.ran ? o.detail : "did not run: " + o.detail);
        return c;
    }
    std::string second;
    if (empty_clause) {
        second = "; lrat-check not applicable (the CNF contains the empty clause)";
    } else if (auto lc = find_tool("lrat-check", opt)) {
        auto o2 = check_lrat(*lc, cnf_path, lrat, budget);
        if (o2.ran && !o2.verified) {
            c.note = "not certified: cake_lpr accepted but lrat-check rejected (" + o2.detail + ")";
            return c;
        }
        if (o2.ran) second = "; lrat-check " + lc->version + " agrees";
    }
    c.certified = true;
    c.info = solver_desc + " lrat " + std::to_string(lrat_steps(lrat)) + " steps, checked by cake_lpr " +
             cake->version + second + "; cnf sha256 " + cnf_sha;
    return c;
}

std::atomic<unsigned long long> g_dir_counter{0};

fs::path make_work_dir(const SolveOptions& opt, const std::string& h) {
    std::error_code ec;
    if (!opt.work_dir.empty()) {
        fs::create_directories(opt.work_dir, ec);
        return opt.work_dir;
    }
#ifndef _WIN32
    const auto pid = static_cast<long long>(getpid());
#else
    const long long pid = 0;
#endif
    fs::path d = fs::temp_directory_path(ec) /
                 ("prism-solve-" + h.substr(0, 12) + "-" + std::to_string(pid) + "-" +
                  std::to_string(g_dir_counter.fetch_add(1)));
    fs::create_directories(d, ec);
    return d;
}

}  // namespace

SolveResult solve(z3::context& c, const z3::expr& formula, const SolveOptions& opt) {
    SolveResult res;
    const double t0 = now_s();
    const double deadline = t0 + std::max(0.01, opt.timeout_s);
    std::vector<std::string> notes;
    auto finish = [&](SolveResult& r) -> SolveResult {
        if (!r.ran.empty()) notes.push_back("ran: " + join(r.ran));
        if (!r.missing.empty()) notes.push_back("missing: " + join(r.missing));
        r.note = join(notes, "; ");
        r.wall_s = now_s() - t0;
        return r;
    };
    if (!formula.is_bool()) {
        res.kind = Kind::Error;
        notes.push_back("formula is not Boolean");
        return finish(res);
    }

    const Features ft = features(formula);
    res.bucket = ft.bucket();
    std::vector<std::string> canonical;
    std::string norm;
    try {
        norm = normalized_query(c, formula, &canonical);
    } catch (const z3::exception& e) {
        res.kind = Kind::Error;
        notes.push_back(std::string("normalisation failed: ") + e.msg());
        return finish(res);
    }
    res.query_hash = sha256_hex(norm);
    const std::string cert_reason = not_certifiable_reason(formula);
    const bool blastable = cert_reason.empty();
    const bool want_cert = opt.certified && blastable;
    if (opt.certified && !blastable) notes.push_back("not certifiable: " + cert_reason);
    const fs::path root = cache_root(opt);

    // ---------------------------------------------------------------- cache
    if (opt.use_cache) {
        if (auto j = cache_load(root, res.query_hash)) {
            const std::string kind = j->value("kind", "");
            if (kind == "sat") {
                std::map<std::string, std::string> m;
                const auto& jm = (*j)["model"];
                for (std::size_t i = 0; i < canonical.size(); ++i) {
                    auto key = "prism!v" + std::to_string(i);
                    if (jm.contains(key)) m[canonical[i]] = jm[key].get<std::string>();
                }
                std::string why;
                if (validate_model(c, formula, m, &why)) {
                    res.kind = Kind::Sat;
                    res.model = std::move(m);
                    res.winner = j->value("winner", "");
                    res.cache_hit = true;
                    notes.push_back("cache hit: counterexample re-validated in Z3");
                    return finish(res);
                }
                notes.push_back("cache entry ignored: " + why);
            } else if (kind == "unsat" && !want_cert) {
                res.kind = Kind::Unsat;
                res.winner = j->value("winner", "");
                res.cache_hit = true;
                notes.push_back(std::string("cache hit: unsat") +
                                (j->value("certified", false) ? " (cached certified; not re-checked for a plain request)"
                                                              : ""));
                if (opt.certified) notes.push_back("not certified: " + cert_reason);
                return finish(res);
            } else if (kind == "unsat" && want_cert) {
                if (!j->value("certified", false)) {
                    notes.push_back("cached unsat is uncertified: solving again for a certificate");
                } else {
                    // Re-derive the CNF and re-check the stored proof: a cache
                    // file is not a certificate by itself.
                    std::string why;
                    auto cnf = bitblast(c, formula, &why);
                    const auto cnf_txt = cnf ? to_dimacs(*cnf) : std::string();
                    const auto cnf_sha = sha256_hex(cnf_txt);
                    const fs::path cp = root / "certs" / (res.query_hash + ".cnf");
                    const fs::path lp = root / "certs" / (res.query_hash + ".lrat");
                    if (!cnf) {
                        notes.push_back("cached certificate not re-checked: " + why);
                    } else if (cnf_sha != j->value("cnf_sha256", "") || sha256_file(cp) != cnf_sha) {
                        notes.push_back("cached certificate is for a different CNF: solving again");
                    } else {
                        auto ck = run_checkers(opt, cp, cnf_sha, lp, j->value("cert_solver", "cadical"),
                                               std::max(10.0, opt.timeout_s), has_empty_clause(*cnf));
                        if (ck.certified) {
                            res.kind = Kind::Unsat;
                            res.certified = true;
                            res.certificate_info = ck.info;
                            res.winner = j->value("winner", "");
                            res.cache_hit = true;
                            notes.push_back("cache hit: certificate re-checked by cake_lpr");
                            return finish(res);
                        }
                        notes.push_back("cached certificate failed re-check (" + ck.note + "): solving again");
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------- members
    json hist;
    {
        std::lock_guard<std::mutex> g(g_history_mu);
        hist = history_load(root);
    }
    std::vector<Member> members;
    auto add = [&](std::string name, MemberKind k, std::vector<std::string> argv, std::string ver) {
        Member m;
        m.name = std::move(name);
        m.kind = k;
        m.argv = std::move(argv);
        m.version = std::move(ver);
        members.push_back(std::move(m));
    };
    const bool smt_ok = !ft.quant && !ft.arith && !ft.other_sort;
    if (opt.z3_in_process) add("z3", MemberKind::Z3, {}, Z3_get_full_version());
    if (opt.portfolio) {
        if (smt_ok) {
            if (auto t = find_tool("bitwuzla", opt))
                add("bitwuzla", MemberKind::Smt2, {t->path.string(), "{input}"}, t->version);
            else
                res.missing.push_back("bitwuzla");
        }
        if (blastable) {
            for (const char* s : {"cadical", "kissat"}) {
                if (auto t = find_tool(s, opt)) {
                    std::vector<std::string> argv{t->path.string()};
                    if (std::string_view(s) == "kissat") argv.push_back("--quiet");
                    argv.push_back("{input}");
                    add(s, MemberKind::Dimacs, std::move(argv), t->version);
                } else {
                    res.missing.push_back(s);
                }
            }
            if (opt.sls) add("sls", MemberKind::Sls, {}, "probsat-cpu");
        }
        for (const auto& x : opt.extra_solvers) {
            if (x.input == ExternalSolver::Input::Dimacs && !blastable) continue;
            if (x.input == ExternalSolver::Input::Smt2 && !smt_ok) continue;
            add(x.name, x.input == ExternalSolver::Input::Dimacs ? MemberKind::Dimacs : MemberKind::Smt2,
                x.argv, "external");
        }
    }
    if (want_cert) {
        // The certificate chain needs CaDiCaL with an LRAT proof even when
        // the portfolio is off.
        auto it = std::find_if(members.begin(), members.end(), [](const Member& m) { return m.name == "cadical"; });
        if (it == members.end()) {
            if (auto t = find_tool("cadical", opt)) {
                add("cadical", MemberKind::Dimacs, {t->path.string(), "{input}"}, t->version);
                it = members.end() - 1;
            } else if (std::find(res.missing.begin(), res.missing.end(), "cadical") == res.missing.end()) {
                res.missing.push_back("cadical");
            }
        }
        if (it != members.end()) {
            it->lrat = true;
            it->argv = {it->argv[0], "--lrat=true", "--binary=false", "{input}", "{proof}"};
        } else {
            notes.push_back("not certified: cadical not found (NOTRUN)");
        }
    }
    // Scheduler. Expected time = the bucket's recorded mean for that member
    // when there is one, else the feature rules. The member with the lowest
    // recorded mean that has also won in this bucket before gets a head
    // start: the others wait min(3 x its mean + 0.2 s, 30% of the timeout)
    // so they do not compete with it for cores. The certificate member is
    // never delayed.
    const auto& hb = hist["buckets"].contains(res.bucket) ? hist["buckets"][res.bucket] : json::object();
    std::optional<std::size_t> lead;
    for (std::size_t i = 0; i < members.size(); ++i) {
        auto& m = members[i];
        m.est = rule_estimate(m.name, ft);
        if (hb.contains(m.name) && hb[m.name].is_object()) {
            const auto n = hb[m.name].value("n", 0);
            if (n >= 1) {
                m.est = hb[m.name].value("total", 0.0) / n;
                if (hb[m.name].value("wins", 0) >= 1 && (!lead || m.est < members[*lead].est)) lead = i;
            }
        }
    }
    if (lead && members.size() > 1) {
        const double delay = std::min(3.0 * members[*lead].est + 0.2, 0.3 * opt.timeout_s);
        for (std::size_t i = 0; i < members.size(); ++i)
            if (i != *lead && !members[i].lrat) members[i].not_before = t0 + delay;
        char buf[160];
        std::snprintf(buf, sizeof buf, "scheduler: %s leads by %.2fs (history, bucket %s)",
                      members[*lead].name.c_str(), delay, res.bucket.c_str());
        notes.push_back(buf);
    }
    for (auto& m : members)
        if (m.lrat) m.est = -1;  // the certificate path always gets a slot
    std::stable_sort(members.begin(), members.end(), [](const Member& a, const Member& b) { return a.est < b.est; });
    if (members.empty()) {
        res.kind = Kind::Unknown;
        notes.push_back("no solver available (NOTRUN)");
        return finish(res);
    }

    // ---------------------------------------------------------------- run
    const fs::path work = make_work_dir(opt, res.query_hash);
    const fs::path smt_path = work / "query.smt2";
    const fs::path cnf_path = work / "query.cnf";
    const fs::path lrat_path = work / "proof.lrat";
    const bool need_smt = std::any_of(members.begin(), members.end(), [](const Member& m) { return m.kind == MemberKind::Smt2; });
    if (need_smt) {
        std::string txt = "(set-option :produce-models true)\n";
        txt += Z3_benchmark_to_smtlib_string(c, "prism", ft.logic().c_str(), "unknown", "", 0, nullptr, formula);
        std::vector<std::string> names;
        for (const auto& k : collect_consts(formula))
            if (plain_symbol(const_name(k))) names.push_back("|" + const_name(k) + "|");
        if (!names.empty()) txt += "(get-value (" + join(names, " ") + "))\n";
        txt += "(exit)\n";
        detail::write_file(smt_path, txt);
    }

    Board board;
    std::vector<std::thread> threads;
    std::deque<std::atomic<bool>> done;                 // one per thread
    std::vector<std::unique_ptr<z3::context>> ctxs;     // private contexts, outlive the threads
    std::shared_ptr<const Cnf> cnf;
    std::string cnf_sha;
    enum class CnfState { None, Running, Ready, Failed } cnf_state = CnfState::None;
    z3::context* blast_ctx = nullptr;
    const unsigned slots = opt.max_parallel ? opt.max_parallel : std::max(2u, std::thread::hardware_concurrency());
    unsigned running = 0;

    auto new_ctx = [&]() -> z3::context& {
        ctxs.push_back(std::make_unique<z3::context>());
        return *ctxs.back();
    };
    // Every job thread raises its own done flag last, so the join below can
    // keep interrupting Z3 until each thread has really left.
    auto spawn = [&](auto&& fn) {
        auto* d = &done.emplace_back(false);
        threads.emplace_back([fn = std::forward<decltype(fn)>(fn), d]() mutable {
            fn();
            d->store(true);
        });
    };
    auto start_blast = [&] {
        cnf_state = CnfState::Running;
        ++running;
        z3::context& bc = new_ctx();
        blast_ctx = &bc;
        z3::expr f2(bc, Z3_translate(c, formula, bc));
        spawn([&board, &bc, f2, cnf_path, &cnf, &cnf_sha] {
            Msg m;
            m.type = Msg::CnfFailed;
            try {
                std::string why;
                auto r = detail::bitblast_fresh(bc, f2, &why);
                if (r) {
                    auto txt = to_dimacs(*r);
                    if (detail::write_file(cnf_path, txt)) {
                        cnf_sha = sha256_hex(txt);
                        cnf = std::make_shared<const Cnf>(std::move(*r));
                        m.type = Msg::CnfReady;
                    } else {
                        m.detail = "cannot write " + cnf_path.string();
                    }
                } else {
                    m.detail = why;
                }
            } catch (const z3::exception& e) {
                m.detail = std::string("bit-blast: ") + e.msg();
            }
            board.post(std::move(m));
        });
    };
    auto start = [&](std::size_t i) {
        Member& m = members[i];
        m.started = true;
        ++running;
        res.ran.push_back(m.name);
        auto* stop = m.stop.get();
        const double left = std::max(0.01, deadline - now_s());
        switch (m.kind) {
            case MemberKind::Z3: {
                z3::context& zc = new_ctx();
                m.zctx = &zc;
                z3::expr f2(zc, Z3_translate(c, formula, zc));
                spawn([&board, &zc, f2, i, left, stop] {
                    Msg msg;
                    msg.member = i;
                    const double s0 = now_s();
                    try {
                        z3::solver s(zc);
                        z3::params p(zc);
                        p.set("timeout", static_cast<unsigned>(std::min(left * 1000.0, 4.0e9)));
                        s.set(p);
                        s.add(f2);
                        auto r = stop->load() ? z3::unknown : s.check();
                        if (r == z3::sat) {
                            msg.kind = Kind::Sat;
                            z3::model md = s.get_model();
                            for (const auto& k : collect_consts(f2))
                                msg.model[const_name(k)] = md.eval(k, true).to_string();
                        } else if (r == z3::unsat) {
                            msg.kind = Kind::Unsat;
                        } else {
                            msg.kind = Kind::Unknown;
                            msg.detail = s.reason_unknown();
                            if (msg.detail.find("timeout") != std::string::npos ||
                                msg.detail.find("canceled") != std::string::npos)
                                msg.kind = Kind::Timeout;
                        }
                    } catch (const z3::exception& e) {
                        msg.kind = Kind::Error;
                        msg.detail = e.msg();
                    }
                    msg.secs = now_s() - s0;
                    board.post(std::move(msg));
                });
                break;
            }
            case MemberKind::Smt2: {
                std::vector<std::string> argv;
                for (const auto& a : m.argv) argv.push_back(subst(a, smt_path, lrat_path));
                spawn([&board, argv, i, left, stop] {
                    board.post(smt_result(i, detail::run(argv, left, stop)));
                });
                break;
            }
            case MemberKind::Dimacs: {
                std::vector<std::string> argv;
                for (const auto& a : m.argv) argv.push_back(subst(a, cnf_path, lrat_path));
                auto cp = cnf;
                spawn([&board, argv, i, left, stop, cp] {
                    board.post(dimacs_result(i, detail::run(argv, left, stop), *cp));
                });
                break;
            }
            case MemberKind::Sls: {
                auto cp = cnf;
                const auto seed = opt.seed;
                // Local search either finds a model early or not at all on
                // structured (Tseitin) CNF: cap it so it gives its core back.
                const double budget = std::min(
                    left, opt.sls_budget_s > 0 ? opt.sls_budget_s : std::max(1.0, 0.1 * opt.timeout_s));
                spawn([&board, i, budget, stop, cp, seed] {
                    Msg msg;
                    msg.member = i;
                    const double s0 = now_s();
                    SlsOptions so;
                    so.seed = seed;
                    so.timeout_s = budget;
                    so.stop = stop;
                    auto r = probsat(*cp, so);
                    msg.secs = now_s() - s0;
                    if (r.found && assignment_satisfies(*cp, r.assignment)) {
                        msg.kind = Kind::Sat;
                        msg.model = model_from_assignment(*cp, r.assignment);
                    } else {
                        msg.kind = Kind::Unknown;  // never evidence of unsat
                        msg.detail = "no assignment after " + std::to_string(r.flips) + " flips";
                    }
                    board.post(std::move(msg));
                });
                break;
            }
        }
    };
    auto fill_slots = [&] {
        for (std::size_t i = 0; i < members.size(); ++i) {
            Member& m = members[i];
            if (m.started || m.finished) continue;
            if (m.not_before > now_s()) continue;
            const bool on_cnf = m.kind == MemberKind::Dimacs || m.kind == MemberKind::Sls;
            if (running >= slots && !m.lrat) break;
            if (on_cnf) {
                if (cnf_state == CnfState::None) { start_blast(); continue; }
                if (cnf_state != CnfState::Ready) continue;
            }
            start(i);
        }
    };
    // Stop every member except `keep` (the certificate member, which may
    // still need the CNF: the bit-blast is then left running too).
    auto stop_all = [&](std::optional<std::size_t> keep = std::nullopt) {
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (keep && *keep == i) continue;
            members[i].stop->store(true);
            if (!members[i].started) members[i].finished = true;
            if (members[i].zctx) Z3_interrupt(*members[i].zctx);
        }
        if (!keep && blast_ctx) Z3_interrupt(*blast_ctx);
    };

    std::optional<Msg> accepted;
    std::string accepted_by;
    std::optional<std::size_t> cert_member;
    for (std::size_t i = 0; i < members.size(); ++i)
        if (members[i].lrat) cert_member = i;
    std::optional<Msg> cert_msg;
    bool timed_out = false;
    std::vector<std::string> disagreements;

    fill_slots();
    for (;;) {
        const bool cert_pending = want_cert && cert_member && !members[*cert_member].finished &&
                                  cnf_state != CnfState::Failed;
        if (accepted && !(accepted->kind == Kind::Unsat && cert_pending)) break;
        const bool any_live = running > 0 || std::any_of(members.begin(), members.end(), [](const Member& m) {
                                  return !m.started && !m.finished;
                              });
        if (!any_live) break;
        double wake = deadline;
        for (const auto& m : members)
            if (!m.started && !m.finished && m.not_before > 0) wake = std::min(wake, m.not_before);
        Msg msg;
        if (!board.wait(msg, wake)) {
            if (now_s() >= deadline) { timed_out = true; break; }
            if (!accepted) fill_slots();
            for (auto& m : members)  // due now: never wait on them again
                if (m.not_before <= now_s()) m.not_before = 0;
            continue;
        }
        if (msg.type == Msg::CnfReady || msg.type == Msg::CnfFailed) {
            --running;
            if (msg.type == Msg::CnfReady) {
                cnf_state = CnfState::Ready;
                if (accepted && cert_member && !members[*cert_member].started) start(*cert_member);
            } else {
                cnf_state = CnfState::Failed;
                notes.push_back("bit-blast failed: " + msg.detail);
                for (auto& m : members)
                    if (!m.started && (m.kind == MemberKind::Dimacs || m.kind == MemberKind::Sls)) m.finished = true;
            }
            if (!accepted) fill_slots();
            continue;
        }
        --running;
        Member& m = members[msg.member];
        m.finished = true;
        const bool definitive = msg.kind == Kind::Sat || msg.kind == Kind::Unsat;
        if (definitive) res.times[m.name] = msg.secs;
        if (msg.member == cert_member) cert_msg = msg;
        if (msg.kind == Kind::Sat) {
            std::string why;
            if (validate_model(c, formula, msg.model, &why)) {
                if (accepted && accepted->kind == Kind::Unsat) {
                    // A validated counterexample outranks any solver's unsat.
                    disagreements.push_back(accepted_by + " said unsat but " + m.name + "'s model validates");
                    accepted = msg;
                    accepted_by = m.name;
                    stop_all();
                } else if (!accepted) {
                    accepted = msg;
                    accepted_by = m.name;
                    stop_all();
                }
            } else {
                notes.push_back(m.name + ": SAT model rejected by Z3 model check (" + why + ")");
            }
        } else if (msg.kind == Kind::Unsat) {
            if (!accepted) {
                accepted = msg;
                accepted_by = m.name;
                stop_all(cert_member);
            }
        } else if (!msg.detail.empty() && msg.detail != "cancelled") {
            notes.push_back(m.name + ": " + std::string(kind_name(msg.kind)) + " (" + msg.detail + ")");
        }
        if (!accepted) fill_slots();
    }
    stop_all();
    // A Z3 interrupt that lands before check() starts can be lost: repeat it
    // until every job thread has left.
    for (;;) {
        bool all = true;
        for (auto& d : done) all = all && d.load();
        if (all) break;
        for (auto& m : members)
            if (m.zctx) Z3_interrupt(*m.zctx);
        if (blast_ctx) Z3_interrupt(*blast_ctx);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (auto& t : threads) t.join();
    // Drain what arrived while stopping (a late certificate-member answer).
    for (Msg msg; board.wait(msg, 0.0);) {
        if (msg.type != Msg::Result) continue;
        if (msg.member == cert_member && !cert_msg) cert_msg = msg;
    }

    // ---------------------------------------------------------------- verdict
    if (accepted) {
        res.kind = accepted->kind;
        res.winner = accepted_by;
        if (res.kind == Kind::Sat) {
            res.model = accepted->model;
            if (opt.certified) notes.push_back("counterexample (model validated in Z3); certification applies to unsat only");
        }
    } else {
        res.kind = timed_out || now_s() >= deadline ? Kind::Timeout : Kind::Unknown;
    }
    for (const auto& d : disagreements) notes.push_back("DISAGREEMENT: " + d);

    if (want_cert && res.kind == Kind::Unsat && cert_member) {
        const Member& cm = members[*cert_member];
        if (!cert_msg) {
            notes.push_back(cnf_state == CnfState::Failed ? "not certified: no CNF"
                                                          : "not certified: cadical did not finish in time");
        } else if (cert_msg->kind == Kind::Sat) {
            // Validated models were handled above; an unvalidated one means the
            // bit-blast and the formula disagree.
            notes.push_back("not certified: cadical found the CNF satisfiable");
        } else if (cert_msg->kind != Kind::Unsat) {
            notes.push_back("not certified: cadical " + std::string(kind_name(cert_msg->kind)) +
                            (cert_msg->detail.empty() ? "" : " (" + cert_msg->detail + ")"));
        } else {
            auto ck = run_checkers(opt, cnf_path, cnf_sha, lrat_path, "cadical " + cm.version,
                                   std::max(10.0, opt.timeout_s), cnf && has_empty_clause(*cnf));
            if (ck.certified) {
                res.certified = true;
                res.certificate_info = ck.info;
                notes.push_back("certified: LRAT proof accepted by cake_lpr");
            } else {
                notes.push_back(ck.note);
            }
        }
    } else if (want_cert && res.kind == Kind::Unsat && !cert_member) {
        // already noted: cadical not found
    }

    // ---------------------------------------------------------------- record
    if (opt.use_cache && (res.kind == Kind::Sat || res.kind == Kind::Unsat)) {
        json e{{"schema", 1}, {"hash", res.query_hash}, {"kind", std::string(kind_name(res.kind))},
               {"winner", res.winner}, {"certified", res.certified}, {"z3", Z3_get_full_version()}};
        if (res.kind == Kind::Sat) {
            json jm = json::object();
            std::map<std::string, std::string> back;
            for (std::size_t i = 0; i < canonical.size(); ++i)
                if (auto it = res.model.find(canonical[i]); it != res.model.end())
                    jm["prism!v" + std::to_string(i)] = it->second;
            e["model"] = jm;
        }
        bool write = true;
        if (res.certified) {
            e["certificate_info"] = res.certificate_info;
            e["cnf_sha256"] = cnf_sha;
            e["cert_solver"] = cert_member ? "cadical " + members[*cert_member].version : "cadical";
            if (opt.cache_certificates) {
                std::error_code ec;
                fs::create_directories(root / "certs", ec);
                fs::copy_file(cnf_path, root / "certs" / (res.query_hash + ".cnf"),
                              fs::copy_options::overwrite_existing, ec);
                if (!ec)
                    fs::copy_file(lrat_path, root / "certs" / (res.query_hash + ".lrat"),
                                  fs::copy_options::overwrite_existing, ec);
                if (ec) e["certified"] = false;  // cannot be re-checked later
            } else {
                e["certified"] = false;
            }
        } else if (res.kind == Kind::Unsat) {
            // Never overwrite a certified entry with a plain one.
            if (auto old = cache_load(root, res.query_hash); old && old->value("certified", false)) write = false;
        }
        if (write) detail::write_file(entry_path(root, res.query_hash), e.dump(1));
    }
    if (!res.times.empty()) {
        std::lock_guard<std::mutex> g(g_history_mu);
        json h = history_load(root);
        auto& b = h["buckets"][res.bucket];
        for (const auto& [name, secs] : res.times) {
            auto& s = b[name];
            if (!s.is_object()) s = json::object();
            s["n"] = s.value("n", 0) + 1;
            s["total"] = s.value("total", 0.0) + secs;
            if (name == res.winner) s["wins"] = s.value("wins", 0) + 1;
        }
        detail::write_file(root / "solve_times.json", h.dump(1));
    }
    if (!opt.keep_artifacts && opt.work_dir.empty()) {
        std::error_code ec;
        fs::remove_all(work, ec);
    }
    return finish(res);
}

}  // namespace prism::solver

#endif  // PRISM_HAS_Z3
