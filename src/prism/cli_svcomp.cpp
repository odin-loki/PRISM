// `prism svcomp`: PRISM as an SV-COMP verifier (include/prism/svcomp.hpp,
// docs/SVCOMP.md). One task, the subset scorer (`svcomp score`) and the
// tool archive (`svcomp pack`).
#include "prism/svcomp.hpp"

#include "prism/config.hpp"
#include "prism/pipeline.hpp"
#include "prism/taskdef.hpp"
#include "proc.hpp"
#include "svcomp/internal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#ifndef _WIN32
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace prism::svcomp {

using namespace detail;
namespace fs = std::filesystem;

namespace {

// stdout and stderr to /dev/null while the pipeline runs: the only lines
// this subcommand prints are its result lines.
class Silence {
public:
    Silence() {
#ifndef _WIN32
        std::cout.flush();
        std::cerr.flush();
        std::fflush(nullptr);
        int null = ::open("/dev/null", O_WRONLY);
        if (null < 0) return;
        saved_out_ = ::dup(STDOUT_FILENO);
        saved_err_ = ::dup(STDERR_FILENO);
        ::dup2(null, STDOUT_FILENO);
        ::dup2(null, STDERR_FILENO);
        ::close(null);
#endif
    }
    ~Silence() {
#ifndef _WIN32
        std::cout.flush();
        std::cerr.flush();
        std::fflush(nullptr);
        if (saved_out_ >= 0) {
            ::dup2(saved_out_, STDOUT_FILENO);
            ::close(saved_out_);
        }
        if (saved_err_ >= 0) {
            ::dup2(saved_err_, STDERR_FILENO);
            ::close(saved_err_);
        }
#endif
    }
    Silence(const Silence&) = delete;
    Silence& operator=(const Silence&) = delete;

private:
    int saved_out_ = -1, saved_err_ = -1;
};

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// A fresh directory below parent ("prism-replay-XXXXXX").
fs::path make_temp_dir(const fs::path& parent, const std::string& prefix) {
#ifndef _WIN32
    std::string tmpl = (parent / (prefix + "XXXXXX")).string();
    if (::mkdtemp(tmpl.data())) return tmpl;
#endif
    for (int k = 0;; ++k) {
        fs::path p = parent / (prefix + std::to_string(std::time(nullptr)) + "-" + std::to_string(k));
        std::error_code ec;
        if (fs::create_directory(p, ec)) return p;
        if (k > 1000) return p;
    }
}

// Run the pipeline in this process with exactly the SV-COMP stages, no LLM
// and no execution of task code (allow_exec stays off: replay is the only
// step that runs the task, and it is gated separately).
std::optional<json> run_prism(const fs::path& task_c, const fs::path& out, std::string& error) {
    Config cfg = default_config();
    cfg.llm = false;
    cfg.allow_exec = false;
    std::vector<std::string> stages;
    std::stringstream ss(STAGES);
    for (std::string s; std::getline(ss, s, ',');) stages.push_back(s);
    cfg.stages = stages;
    cfg.root = fs::absolute(task_c);
    cfg.out = fs::absolute(out);
    try {
        Silence quiet;
        (void)run_pipeline(cfg);
    } catch (const std::exception& e) {
        error = std::string("prism pipeline failed: ") + e.what();
        return std::nullopt;
    }
    const fs::path rep = cfg.out / "report.json";
    std::ifstream in(rep, std::ios::binary);
    if (!in) {
        error = "prism wrote no report.json (" + cfg.root.string() + " --no-llm --stage " + STAGES + " --out " +
                cfg.out.string() + ")";
        return std::nullopt;
    }
    try {
        return json::parse(in);
    } catch (const std::exception& e) {
        error = "unreadable report.json: " + std::string(e.what());
        return std::nullopt;
    }
}

}  // namespace

std::string version_string() { return PRISM_VERSION; }

Outcome solve(const fs::path& task, const fs::path& prop_file, const SolveOptions& opt) {
    Outcome oc;
    std::ifstream pin(prop_file, std::ios::binary);
    if (!pin) {
        oc.decision = Decision{"error", "cannot read property file " + prop_file.string(), std::nullopt, json::object()};
        return oc;
    }
    std::string spec((std::istreambuf_iterator<char>(pin)), std::istreambuf_iterator<char>());
    const std::string prop = parse_property(spec);
    if (!supported_property(prop)) {
        oc.decision = Decision{"unknown", "unsupported property file " + prop_file.filename().string(), std::nullopt,
                               json::object()};
        return oc;
    }
    const std::string src_text = read_text(task);
    const std::string dm = upper(opt.data_model);
    if (dm == "ILP32" && width_dependent_code(src_text)) {
        oc.decision = Decision{"unknown", "ILP32 task uses width-dependent types; PRISM's encoders are LP64",
                               std::nullopt, json::object()};
        return oc;
    }
    std::error_code ec;
    fs::create_directories(opt.out, ec);
    // Preprocessed .i tasks are C; copy to .c so clang names the unit consistently.
    const fs::path task_c = opt.out / (task.stem().string() + ".c");
    fs::copy_file(task, task_c, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        oc.decision = Decision{"error", "cannot copy " + task.string() + ": " + ec.message(), std::nullopt, json::object()};
        return oc;
    }
    std::string error;
    auto report = run_prism(task_c, opt.out / "prism-out", error);
    if (!report) {
        oc.decision = Decision{"error", error, std::nullopt, json::object()};
        return oc;
    }
    auto rp = [&](const json& f) -> json {
        auto trace = nondet_trace(f);
        std::optional<std::vector<Num>> values;
        if (trace) {
            values.emplace();
            for (const auto& [_, v] : *trace) values->push_back(v);
        }
        fs::path d = make_temp_dir(opt.out, "prism-replay-");
        json r = replay(task, prop, opt.allow_exec, values, d);
        std::error_code rm;
        fs::remove_all(d, rm);
        return r;
    };
    Decision dec = decide(*report, prop, rp);
    oc.decision = dec;
    Decision& d = oc.decision;
    WitnessMeta meta;
    meta.input_file = task;
    meta.input_file_name = task.filename().string();
    meta.specification = spec;
    meta.data_model = dm;
    meta.producer_version = version_string();
    if (d.answer == "true" && opt.witness && d.finding) {
        auto [invariants, note] = correctness_invariants(task, *d.finding);
        d.reason += "; correctness witness: " + note;
        if (write_witness(build_correctness_witness(invariants, meta), *opt.witness)) oc.witness_path = *opt.witness;
        else d.reason += "; witness not written: cannot write " + opt.witness->string();
        return oc;
    }
    if (d.answer.starts_with("false") && opt.witness && d.finding) {
        const json& f = *d.finding;
        long line = 1;
        if (has(d.replay, "line") && truthy(d.replay.at("line"))) line = static_cast<long>(to_int(d.replay.at("line")).value_or(1));
        else if (has(f, "line") && truthy(f.at("line"))) line = static_cast<long>(to_int(f.at("line")).value_or(1));
        std::optional<long> col;
        if (has(d.replay, "column") && truthy(d.replay.at("column")))
            col = target_column(task, line, static_cast<long>(to_int(d.replay.at("column")).value_or(1)));
        else
            col = first_code_column(task, line);
        if (prop == "unreach-call") {
            std::optional<long> hint;
            if (has(f, "line") && f.at("line").is_number()) hint = static_cast<long>(to_int(f.at("line")).value_or(0));
            auto site = reach_error_call_site(task, hint);
            if (!site) {
                // No witness rather than a witness pointing at the wrong call.
                d.reason += "; no witness: the reach_error() call site is ambiguous";
                return oc;
            }
            line = site->first;
            col = site->second;
        }
        // No "function" on the target: the location (UBSan report, reach_error()
        // call) need not be in main, and the field is optional in format 2.0.
        Counterexample cex{"main", Location{task.filename().string(), line, col, std::nullopt}, {}};
        auto trace = nondet_trace(f).value_or(std::vector<std::pair<std::string, Num>>{});
        const bool physical = get_str(extra_of(f), "nondet_loc_kind") == "physical";
        cex.nondet = nondet_waypoints(task, trace, nondet_locations(f, trace.size()), physical);
        if (write_witness(build_violation_witness(cex, meta), *opt.witness)) oc.witness_path = *opt.witness;
        else d.reason += "; witness not written: cannot write " + opt.witness->string();
    }
    return oc;
}

// ---------------------------------------------------------------- score

std::vector<SubsetTask> subset_tasks(const fs::path& suite, const std::string& prop_name) {
    std::vector<fs::path> ymls;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(suite, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_regular_file() && it->path().extension() == ".yml") ymls.push_back(it->path());
    // sorted like pathlib paths: component by component
    std::sort(ymls.begin(), ymls.end(), [](const fs::path& a, const fs::path& b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                            [](const fs::path& x, const fs::path& y) { return x.string() < y.string(); });
    });
    std::vector<SubsetTask> out;
    for (const auto& yml : ymls) {
        auto y = taskdef::load_yaml(yml);
        if (!y.value) {
            std::cerr << "WARNING svcomp score: " << y.error << " (task skipped)\n";
            continue;
        }
        auto t = taskdef::parse_taskdef(*y.value);
        if (!t) continue;
        for (const auto& p : t->properties) {
            if (fs::path(p.property_file).filename().string() != prop_name || !p.expected_verdict) continue;
            if (t->input_files.empty()) continue;
            SubsetTask st;
            st.id = fs::relative(yml, suite, ec).generic_string();
            st.yml = yml;
            st.input = fs::weakly_canonical(yml.parent_path() / t->input_files.front(), ec);
            st.property_file = fs::weakly_canonical(yml.parent_path() / p.property_file, ec);
            st.expected = *p.expected_verdict;
            st.data_model = t->data_model;
            out.push_back(std::move(st));
        }
    }
    return out;
}

std::pair<std::string, int> score(bool expected, const std::string& answer) {
    std::string a;
    for (char c : answer) a += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool is_true = a == "true";
    if (!is_true && !a.starts_with("false")) return {"unknown", 0};
    int pts = is_true ? (expected ? 2 : -32) : (expected ? -16 : 1);
    return {pts > 0 ? "correct" : "wrong", pts};
}

std::string score_markdown(const ojson& rows, const ojson& meta) {
    long total = 0, n_true = 0, ct = 0, cf = 0, wt = 0, wf = 0, unk = 0;
    for (const auto& r : rows) {
        const bool exp = r.at("expected").get<bool>();
        std::string ans = r.at("answer").get<std::string>();
        std::string low;
        for (char c : ans) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::string outcome = r.at("outcome").get<std::string>();
        total += r.at("points").get<long>();
        if (exp) ++n_true;
        if (exp && outcome == "correct") ++ct;
        if (!exp && outcome == "correct") ++cf;
        if (!exp && low == "true") ++wt;
        if (exp && low.starts_with("false")) ++wf;
        if (outcome == "unknown") ++unk;
    }
    const long n_false = static_cast<long>(rows.size()) - n_true;
    const long best = 2 * n_true + n_false;
    std::ostringstream o;
    o << "- property: " << meta.at("property").get<std::string>() << "\n"
      << "- tasks: " << rows.size() << " (" << n_true << " expected true, " << n_false << " expected false)\n"
      << "- score: **" << total << "** of a possible " << best << "\n"
      << "- correct true: " << ct << ", correct false: " << cf << ", incorrect true: " << wt
      << ", incorrect false: " << wf << ", unknown/error: " << unk << "\n"
      << "- result mapping via: " << meta.at("via").get<std::string>() << "\n"
      << "- engine: " << meta.at("prism").get<std::string>() << "\n\n"
      << "| task | expected | answer | points | reason |\n|---|---|---|---|---|\n";
    for (const auto& r : rows) {
        std::string reason = r.at("reason").get<std::string>().substr(0, 160);
        std::replace(reason.begin(), reason.end(), '|', '/');
        o << "| " << r.at("task").get<std::string>() << " | " << (r.at("expected").get<bool>() ? "true" : "false")
          << " | " << r.at("answer").get<std::string>() << " | " << r.at("points").get<long>() << " | " << reason
          << " |\n";
    }
    return o.str();
}

namespace {

fs::path self_exe(const char* fallback) {
#ifndef _WIN32
    char buf[4096]{};
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) return fs::path(std::string(buf, static_cast<std::size_t>(n)));
#endif
    std::error_code ec;
    return fs::absolute(fallback ? fallback : "prism", ec);
}

// The repository (or unpacked source tree) that holds tools/svcomp/: above
// start, else nullopt.
std::optional<fs::path> find_repo(fs::path start) {
    std::error_code ec;
    start = fs::weakly_canonical(fs::absolute(start, ec), ec);
    for (fs::path d = start; !d.empty(); d = d.parent_path()) {
        if (fs::is_regular_file(d / "tools" / "svcomp" / "prism.py", ec)) return d;
        if (d == d.parent_path()) break;
    }
    return std::nullopt;
}

double round1(double s) { return std::round(s * 10.0) / 10.0; }

int score_main(int argc, char** argv) {
    std::string property = "no-overflow";
    fs::path out = "svcomp-out";
    fs::path suite;
    fs::path prism = self_exe(nullptr);
    int jobs = 2;
    double timeout = 900.0;
    std::string only;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--property") property = next();
        else if (a == "--out") out = next();
        else if (a == "--suite") suite = next();
        else if (a == "--prism") prism = fs::absolute(next());
        else if (a == "--jobs" || a == "-j") jobs = std::max(1, std::atoi(next().c_str()));
        else if (a == "--timeout") timeout = std::atof(next().c_str());
        else if (a == "--only") only = next();
        else if (a == "-h" || a == "--help") {
            std::cout << "prism svcomp score [--property no-overflow|unreach-call] [--jobs N] [--out DIR]\n"
                         "                   [--timeout S] [--only SUBSTRING] [--suite DIR] [--prism BIN]\n"
                         "Runs `prism svcomp` on every task of the pinned SV-COMP subset\n"
                         "(tests/conformance/sv-comp) with a verdict for the property, one task per\n"
                         "process session (a timeout kills the whole tree), and scores it with SV-COMP\n"
                         "points: correct true +2, correct false +1, incorrect true -32, incorrect false\n"
                         "-16, unknown/error/timeout 0. No validator runs, so the score is an unvalidated\n"
                         "upper bound. Replay executes the benchmark programs: the tasks run with\n"
                         "--allow-exec because the pinned suite in this repository is trusted (Law 9).\n"
                         "Writes DIR/score.md and DIR/results.json; exit 1 on any incorrect answer.\n";
            return 0;
        } else {
            std::cerr << "prism svcomp score: unknown argument " << a << "\n";
            return 2;
        }
    }
    if (property != "no-overflow" && property != "unreach-call") {
        std::cerr << "prism svcomp score: --property expects no-overflow or unreach-call\n";
        return 2;
    }
    if (suite.empty()) {
        auto repo = find_repo(fs::current_path());
        if (!repo) repo = find_repo(self_exe(nullptr).parent_path());
        if (!repo) {
            std::cerr << "prism svcomp score: no tests/conformance/sv-comp here; pass --suite DIR\n";
            return 2;
        }
        suite = *repo / "tests" / "conformance" / "sv-comp";
    }
    std::error_code ec;
    const fs::path work = fs::weakly_canonical(fs::absolute(out), ec);
    fs::create_directories(work, ec);
    std::vector<SubsetTask> ts;
    for (auto& t : subset_tasks(suite, property + ".prp"))
        if (only.empty() || t.id.find(only) != std::string::npos) ts.push_back(std::move(t));
    std::vector<ojson> rows(ts.size());
    std::atomic<std::size_t> next_task{0};
    auto worker = [&] {
        for (;;) {
            std::size_t k = next_task++;
            if (k >= ts.size()) return;
            const auto& t = ts[k];
            // "a/b.yml" -> "a__b"
            std::string stem;
            for (char c : t.id) stem += c == '/' ? std::string("__") : std::string(1, c);
            if (stem.ends_with(".yml")) stem.resize(stem.size() - 4);
            const fs::path tout = work / stem;
            std::vector<std::string> cmd = {prism.string(), "svcomp", "--allow-exec", "--out", tout.string(),
                                            "--witness", (tout / "witness.yml").string(), "--prop",
                                            t.property_file.string()};
            if (!t.data_model.empty()) {
                cmd.emplace_back("--data-model");
                cmd.push_back(t.data_model);
            }
            cmd.push_back(t.input.string());
            const auto t0 = std::chrono::steady_clock::now();
            auto r = prism::detail::run_session(cmd, timeout);
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::vector<std::string> lines = splitlines(r.out);
            std::string answer;
            for (const auto& ln : lines)
                if (ln.starts_with(RESULT_PREFIX)) {
                    answer = strip(ln.substr(std::string(RESULT_PREFIX).size()));
                    break;
                }
            if (answer.empty()) answer = r.timed_out ? "TIMEOUT" : "ERROR";
            std::string reason;
            for (const auto& ln : lines)
                if (ln.starts_with(REASON_PREFIX)) {
                    reason = ln.substr(std::string(REASON_PREFIX).size());
                    break;
                }
            auto [outcome, pts] = score(t.expected, answer);
            std::error_code wec;
            const fs::path wit = tout / "witness.yml";
            ojson row = ojson::object();
            row["task"] = t.id;
            row["expected"] = t.expected;
            row["answer"] = answer;
            row["outcome"] = outcome;
            row["points"] = pts;
            row["reason"] = reason;
            row["witness"] = fs::exists(wit, wec) ? ojson(wit.string()) : ojson(nullptr);
            row["seconds"] = round1(secs);
            row["data_model"] = t.data_model;
            rows[k] = std::move(row);
        }
    };
    {
        std::vector<std::jthread> pool;
        for (int j = 0; j < std::min<int>(jobs, static_cast<int>(std::max<std::size_t>(1, ts.size()))); ++j)
            pool.emplace_back(worker);
    }
    ojson meta = ojson::object();
    meta["via"] = "prism svcomp result line";
    meta["prism"] = prism.string();
    meta["property"] = property;
    ojson all_rows = ojson::array();
    for (auto& r : rows) all_rows.push_back(r);
    ojson results = ojson::object();
    results["meta"] = meta;
    results["rows"] = all_rows;
    write_text(work / "results.json", results.dump(2) + "\n");
    const std::string md = score_markdown(all_rows, meta);
    write_text(work / "score.md", md);
    std::cout << md << "\n";
    for (const auto& r : all_rows)
        if (r.at("outcome").get<std::string>() == "wrong") return 1;
    return 0;
}

// ---------------------------------------------------------------- pack

std::optional<std::string> first_line(const std::vector<std::string>& cmd) {
    auto r = prism::detail::run_process(cmd, 15.0);
    if (r.failed || r.timed_out || r.rc != 0) return std::nullopt;
    std::string t = strip(r.text);
    if (t.empty()) return std::nullopt;
    auto nl = t.find('\n');
    return nl == std::string::npos ? t : t.substr(0, nl);
}

int pack_main(int argc, char** argv) {
    fs::path out = "prism-svcomp.tar.gz";
    fs::path prism = self_exe(nullptr);
    std::optional<fs::path> repo;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--out") out = next();
        else if (a == "--prism") prism = next();
        else if (a == "--repo") repo = next();
        else if (a == "-h" || a == "--help") {
            std::cout << "prism svcomp pack [--out prism-svcomp.tar.gz] [--prism BIN] [--repo DIR]\n"
                         "Builds the SV-COMP tool archive. It unpacks to prism/ with the prism binary\n"
                         "(BIN, default this executable), the BenchExec tool-info module prism.py,\n"
                         "README.md, fm-tools.yml (from DIR/tools/svcomp), LICENSE, MANIFEST.json and\n"
                         "dependencies.json (the clang/opt seen here, which are not bundled).\n";
            return 0;
        } else {
            std::cerr << "prism svcomp pack: unknown argument " << a << "\n";
            return 2;
        }
    }
    if (!repo) repo = find_repo(fs::current_path());
    if (!repo) repo = find_repo(self_exe(nullptr).parent_path());
    if (!repo) {
        std::cerr << "prism svcomp pack: tools/svcomp/prism.py not found above the working directory; pass --repo DIR\n";
        return 2;
    }
    auto res = pack(prism, *repo, fs::absolute(out));
    if (!res.ok) {
        std::cerr << "ERROR svcomp pack: " << res.error << "\n";
        return 2;
    }
    std::cout << "wrote " << out.string() << " (" << res.manifest.at("files").size() << " files, prism "
              << res.manifest.at("prism_version").dump() << ")\n";
    return 0;
}

}  // namespace

PackResult pack(const fs::path& prism_bin, const fs::path& repo, const fs::path& out) {
    PackResult res;
    std::error_code ec;
    if (!fs::is_regular_file(prism_bin, ec)) {
        res.error = "prism binary not found: " + prism_bin.string();
        return res;
    }
#ifndef _WIN32
    if (::access(prism_bin.c_str(), X_OK) != 0) {
        res.error = "prism binary is not executable: " + prism_bin.string();
        return res;
    }
#endif
    const fs::path here = repo / "tools" / "svcomp";
    const std::vector<std::pair<fs::path, std::string>> statics = {
        {here / "prism.py", "prism.py"},
        {here / "README.md", "README.md"},
        {here / "fm-tools.yml", "fm-tools.yml"},
        {repo / "LICENSE", "LICENSE"},
    };
    for (const auto& [src, _] : statics)
        if (!fs::is_regular_file(src, ec)) {
            res.error = "missing " + src.string();
            return res;
        }
    Config cfg;
    auto tar_tool = cfg.which({"cmake"});
    std::vector<std::string> tar_cmd;
    if (tar_tool) tar_cmd = {tar_tool->string(), "-E", "tar", "czf"};
    else if (auto t = cfg.which({"tar"})) tar_cmd = {t->string(), "czf"};
    else {
        res.error = "neither cmake nor tar on PATH (NOTRUN: the archive needs one of them)";
        return res;
    }
    ojson manifest = ojson::object();
    manifest["format"] = "prism-svcomp-archive";
    manifest["format_version"] = 2;
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        ::gmtime_r(&t, &tm);
        char buf[32];
        std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
        manifest["created"] = buf;
    }
    std::optional<std::string> version;
    if (auto line = first_line({prism_bin.string(), "--version"}); line && line->starts_with("prism ")) {
        std::istringstream ls(*line);
        std::string word;
        ls >> word >> word;
        version = word;
    }
    manifest["prism_version"] = version ? ojson(*version) : ojson(nullptr);
    std::optional<std::string> head;
    if (auto g = cfg.which({"git"})) head = first_line({g->string(), "-C", repo.string(), "rev-parse", "HEAD"});
    manifest["git_commit"] = head ? ojson(*head) : ojson(nullptr);
    std::vector<std::string> files = {"prism", "MANIFEST.json", "dependencies.json"};
    for (const auto& [_, name] : statics) files.push_back(name);
    std::sort(files.begin(), files.end());
    manifest["files"] = files;
    ojson deps = ojson::object();
    auto tool_line = [&](std::initializer_list<std::string_view> names) -> ojson {
        auto exe = cfg.which(names);
        if (!exe) return nullptr;
        auto l = first_line({exe->string(), "--version"});
        return l ? ojson(*l) : ojson(nullptr);
    };
    deps["clang"] = tool_line({"clang"});
    deps["opt"] = tool_line({"opt"});
    deps["bubblewrap"] = cfg.which({"bwrap"}).has_value();
    ojson env = ojson::object();
    env["PRISM_FUNCTION_BUDGET"] = "optional per-function pir wall seconds (0: none)";
    env["PRISM_HOUDINI_BUDGET"] = "optional run-wide Houdini loop-invariant seconds";
    env["PRISM_CERTIFY_BUDGET"] = "optional per-function certification seconds (certified mode)";
    deps["prism_env"] = env;
    manifest["dependencies"] = deps;

    const fs::path stage = make_temp_dir(fs::temp_directory_path(), "prism-svcomp-pack-");
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(p, e);
        }
    } cleanup{stage};
    const fs::path root = stage / "prism";
    fs::create_directories(root, ec);
    fs::copy_file(prism_bin, root / "prism", fs::copy_options::overwrite_existing, ec);
    if (ec) {
        res.error = "cannot copy " + prism_bin.string() + ": " + ec.message();
        return res;
    }
    fs::permissions(root / "prism", fs::perms(0755), fs::perm_options::replace, ec);
    for (const auto& [src, name] : statics) {
        fs::copy_file(src, root / name, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            res.error = "cannot copy " + src.string() + ": " + ec.message();
            return res;
        }
    }
    write_text(root / "dependencies.json", deps.dump(2) + "\n");
    write_text(root / "MANIFEST.json", manifest.dump(2) + "\n");
    fs::create_directories(out.parent_path(), ec);
    std::vector<std::string> cmd = tar_cmd;
    cmd.push_back(out.string());
    cmd.emplace_back("prism");
    auto r = prism::detail::run_process(cmd, 300.0, stage);
    if (r.failed || r.timed_out || r.rc != 0) {
        res.error = "archiver failed: " + strip(r.text);
        return res;
    }
    res.ok = true;
    res.manifest = manifest;
    return res;
}

int svcomp_main(int argc, char** argv) {
    if (argc > 0 && std::string(argv[0]) == "score") return score_main(argc - 1, argv + 1);
    if (argc > 0 && std::string(argv[0]) == "pack") return pack_main(argc - 1, argv + 1);
    std::optional<std::string> task, prop;
    SolveOptions opt;
    bool version = false;
    auto usage = [] {
        std::cerr << "usage: prism svcomp [--allow-exec] [--data-model ILP32|LP64] [--out DIR] [--witness FILE]\n"
                     "                    --prop PROPERTY.prp TASK.c\n"
                     "       prism svcomp score ... | prism svcomp pack ...   (--help for each)\n";
    };
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        std::optional<std::string> inline_val;
        if (a.starts_with("--") && a.find('=') != std::string::npos) {
            inline_val = a.substr(a.find('=') + 1);
            a = a.substr(0, a.find('='));
        }
        auto next = [&]() -> std::optional<std::string> {
            if (inline_val) return inline_val;
            if (i + 1 < argc) return std::string(argv[++i]);
            return std::nullopt;
        };
        if (a == "--allow-exec") opt.allow_exec = true;
        else if (a == "--version") version = true;
        else if (a == "--prop" || a == "--data-model" || a == "--out" || a == "--witness" || a == "--prism") {
            auto v = next();
            if (!v) {
                usage();
                std::cerr << "prism svcomp: " << a << " expects a value\n";
                return 2;
            }
            if (a == "--prop") prop = *v;
            else if (a == "--out") opt.out = *v;
            else if (a == "--witness") opt.witness = fs::path(*v);
            else if (a == "--data-model") {
                if (upper(*v) != "LP64" && upper(*v) != "ILP32") {
                    usage();
                    std::cerr << "prism svcomp: --data-model expects LP64 or ILP32\n";
                    return 2;
                }
                opt.data_model = *v;
            }
            // --prism: accepted for command lines written for the former
            // wrapper script; the engine now runs in this process.
        } else if (a == "-h" || a == "--help") {
            std::cout << "prism svcomp [--allow-exec] [--data-model ILP32|LP64] [--out DIR] [--witness FILE]\n"
                         "             --prop PROPERTY.prp TASK.c\n"
                         "PRISM as an SV-COMP verifier: runs the stages " << STAGES << " on the task\n"
                         "(no LLM), maps the report to an SV-COMP answer and writes a witness (format 2.0).\n"
                         "Prints PRISM-SVCOMP-RESULT: true | false(<property>) | unknown | error, then\n"
                         "PRISM-SVCOMP-REASON: ... (and PRISM-SVCOMP-WITNESS: FILE when one was written).\n"
                         "true only from a proof of main that covers the property (never\n"
                         "PROVED-ASSUMING or BOUNDED); false only from a refutation whose counterexample\n"
                         "replays. Replay compiles and runs the task: --allow-exec (Law 9); without it\n"
                         "every refutation is unknown. docs/SVCOMP.md has the rules.\n"
                         "  prism svcomp score --help   score the pinned subset (tests/conformance/sv-comp)\n"
                         "  prism svcomp pack --help    build the SV-COMP tool archive\n";
            return 0;
        } else if (!a.starts_with("-") && !task) {
            task = a;
        } else {
            usage();
            std::cerr << "prism svcomp: unrecognized argument " << a << "\n";
            return 2;
        }
    }
    if (version) {
        std::cout << version_string() << "\n";
        return 0;
    }
    if (!task || !prop) {
        usage();
        std::cerr << "prism svcomp: TASK and --prop are required\n";
        return 2;
    }
    Outcome oc;
    try {
        oc = solve(*task, *prop, opt);
    } catch (const std::exception& e) {
        oc.decision = Decision{"error", std::string("prism svcomp failed: ") + e.what(), std::nullopt, json::object()};
        oc.witness_path.reset();
    }
    std::string reason = oc.decision.reason;
    std::replace(reason.begin(), reason.end(), '\n', ' ');
    std::cout << RESULT_PREFIX << oc.decision.answer << "\n" << REASON_PREFIX << reason << "\n";
    if (oc.witness_path) std::cout << WITNESS_PREFIX << oc.witness_path->string() << "\n";
    return 0;
}

}  // namespace prism::svcomp
