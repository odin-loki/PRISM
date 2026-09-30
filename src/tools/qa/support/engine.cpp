#include "engine.hpp"

#include "proctree.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>

namespace prism::qa {

std::vector<std::string> prism_command(const std::string& prism_arg) {
    std::string binp = prism_arg;
    if (binp.empty())
        if (const char* e = std::getenv("PRISM_BIN")) binp = e;
    std::error_code ec;
    if (binp.empty()) {
        fs::path built = repo_root() / "build" / "prism";
        if (!fs::exists(built, ec)) return {};
        binp = built.string();
    }
    return {fs::weakly_canonical(fs::absolute(binp, ec), ec).string()};
}

std::vector<std::string> list_stages(const std::vector<std::string>& cmd) {
    if (cmd.empty()) return {};
    std::vector<std::string> argv = cmd;
    argv.push_back("--list-stages");
    RunOpts o;
    o.timeout_s = 60;
    o.cwd = repo_root();
    auto r = run_tree(argv, o);
    if (r.start_failed || r.timed_out) return {};
    std::vector<std::string> out;
    std::istringstream in(r.out);
    std::string s;
    while (in >> s) out.push_back(s);
    return out;
}

const ojson& PrismRun::found(const std::string& stage, const std::string& fn) const {
    static const ojson empty = ojson::array();
    auto s = findings.find(stage);
    if (s == findings.end()) return empty;
    auto f = s->second.find(fn);
    return f == s->second.end() ? empty : f->second;
}

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string jstr(const ojson& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_null()) return "None";
    if (j.is_boolean()) return j.get<bool>() ? "True" : "False";
    return j.dump();
}

bool falsy_fn(const ojson& f) {
    auto it = f.find("function");
    if (it == f.end() || it->is_null()) return true;
    if (it->is_string()) return it->get<std::string>().empty();
    return false;
}

ojson get_or_null(const ojson& f, const char* k) {
    auto it = f.find(k);
    return it == f.end() ? ojson(nullptr) : *it;
}

ojson placeholder(const std::string& status, const std::string& message) {
    ojson rec = ojson::object();
    rec["status"] = status;
    rec["cls"] = "";
    rec["message"] = message;
    rec["counterexample"] = "";
    rec["line"] = nullptr;
    return rec;
}
}  // namespace

PrismRun run_prism(const std::vector<std::string>& cmd, const Task& task, const std::vector<std::string>& stages,
                   const fs::path& work, double timeout, int unwind, const std::vector<std::string>& extra_args,
                   const std::map<std::string, std::string>& rename, const std::string& tag, long mem_limit_mb) {
    PrismRun res;
    fs::path out = work / tag / path_key(task.ident);
    std::error_code ec;
    if (fs::exists(out, ec)) fs::remove_all(out, ec);
    std::string st_list;
    for (std::size_t i = 0; i < stages.size(); ++i) st_list += (i ? "," : "") + stages[i];
    std::vector<std::string> argv = cmd;
    argv.insert(argv.end(), {task.source.string(), "--no-llm", "--stage", st_list, "--out", out.string()});
    if (task.unwind || unwind) argv.insert(argv.end(), {"--unwind", std::to_string(task.unwind ? task.unwind : unwind)});
    argv.insert(argv.end(), extra_args.begin(), extra_args.end());
    res.argv = argv;
    timeout = std::max(timeout, task.timeout);
    const auto t0 = std::chrono::steady_clock::now();
    // Own session (so a timeout kills prism's solver/clang/checker children
    // too) and an address-space cap every child inherits: a task that needs
    // more memory than the runner has is "no answer", never a lost runner.
    RunOpts o;
    o.timeout_s = timeout;
    o.cwd = repo_root();
    o.mem_limit_mb = mem_limit_mb;
    auto r = run_tree(argv, o);
    if (r.timed_out) {
        res.error = "TIMEOUT";
        res.seconds = timeout;
        return res;
    }
    if (r.start_failed) {
        res.error = "no report.json (cannot start: " + utf8_clean(r.err) + "): ";
        res.seconds = 0;
        return res;
    }
    const int rc = r.rc;
    res.exit = rc;
    const std::string tail = py_tail(utf8_clean(r.err), 400);
    res.seconds = std::round(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 100.0) / 100.0;
    fs::path rep = out / "report.json";
    if (!fs::exists(rep, ec)) {
        bool mem = mem_limit_mb > 0 && (tail.find("bad_alloc") != std::string::npos ||
                                        lower(tail).find("out of memory") != std::string::npos || rc == -6 ||
                                        rc == -9 || rc == 134 || rc == 137);
        res.error = "no report.json (" + (mem ? std::string("memory limit") : "exit " + std::to_string(rc)) + "): " + tail;
        return res;
    }
    ojson data;
    try {
        data = ojson::parse(read_text(rep));
    } catch (const std::exception& e) {
        // a truncated report (killed run, full disk) is no answer, not a crash of the whole suite
        res.error = "unreadable report.json (exit " + std::to_string(rc) + "): " + e.what();
        return res;
    }
    auto stages_it = data.find("stages");
    if (stages_it == data.end() || !stages_it->is_array()) return res;
    for (const auto& st : *stages_it) {
        std::string name = st.value("name", std::string());
        res.stage_status[name] = st.contains("status") && st["status"].is_string() ? st["status"].get<std::string>() : "";
        if (std::find(VERDICT_STAGES.begin(), VERDICT_STAGES.end(), name) == VERDICT_STAGES.end()) continue;
        if (auto rn = rename.find(name); rn != rename.end()) name = rn->second;
        const ojson empty = ojson::array();
        const ojson& fs_ = st.contains("findings") && st["findings"].is_array() ? st["findings"] : empty;
        for (const auto& f : fs_) {
            if (!f.contains("function") || !f["function"].is_string()) continue;
            std::string fn = f["function"].get<std::string>();
            if (!task.has(fn)) continue;
            ojson rec = ojson::object();
            for (const char* k : {"status", "cls", "message", "counterexample", "line"}) rec[k] = get_or_null(f, k);
            if (f.contains("extra") && f["extra"].is_object()) {
                ojson ex = ojson::object();
                for (const auto& [k, v] : f["extra"].items())
                    if (std::find(KEEP_EXTRA.begin(), KEEP_EXTRA.end(), k) != KEEP_EXTRA.end()) ex[k] = v;
                if (!ex.empty()) rec["extra"] = ex;
            }
            auto& lst = res.findings[name][fn];
            if (lst.is_null()) lst = ojson::array();
            lst.push_back(rec);
        }
        // A unit-level ERROR (e.g. the Clang front end rejects the file) speaks
        // for every function of the unit: recorded as ERROR, not as missing.
        const ojson* unit_err = nullptr;
        for (const auto& f : fs_)
            if (falsy_fn(f) && f.value("status", ojson()).is_string() && f["status"] == "ERROR") {
                unit_err = &f;
                break;
            }
        if (unit_err) {
            for (const auto& [fn, _] : task.expected) {
                auto& lst = res.findings[name][fn];
                if (lst.is_null()) lst = ojson::array();
                if (lst.empty())
                    lst.push_back(placeholder("ERROR", py_head(jstr(unit_err->contains("message") ? (*unit_err)["message"]
                                                                                                  : ojson("")),
                                                               300)));
            }
        }
        if (st.contains("status") && st["status"] == "failed") {
            // A crashed stage loses every function of the file: record why
            // instead of reporting the functions as silently missing.
            for (const auto& [fn, _] : task.expected) {
                auto& lst = res.findings[name][fn];
                if (lst.is_null()) lst = ojson::array();
                if (lst.empty())
                    lst.push_back(placeholder("STAGE-FAILED",
                                              py_head(jstr(st.contains("detail") ? st["detail"] : ojson("")), 300)));
            }
        }
    }
    return res;
}

}  // namespace prism::qa
