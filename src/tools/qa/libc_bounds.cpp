// libc-bounds: size-bound sweep for the libc model contract harnesses
// (roadmap 8.2).
//
// tests/conformance/libc-models/*_contracts.c prove each model's contract for
// objects of up to N bytes (harness.h, default N = 4). This re-runs every
// `_true` harness alone with N = 4, 8, 16, ... (`#define N` before harness.h,
// `--unwind 2N + 2` so every model loop can close) and reports per function
// the largest N that is still PROVED within the per-run timeout (180 s, as
// the conformance gate).
//
// A PROVED here is a proof for objects up to that N only (a size-bounded
// result, docs/PIR.md "Library models verified by PRISM"), never a proof for
// all sizes (Law 2), and it is reported as such.

#include "qa.hpp"

#include "../../prism/proc.hpp"
#include "prism/regex.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace prism::qa {

namespace {
std::vector<std::string> lines_keepends(const std::string& text) {
    std::vector<std::string> out;
    std::size_t a = 0;
    while (a < text.size()) {
        auto b = text.find('\n', a);
        if (b == std::string::npos) b = text.size() - 1;
        out.push_back(text.substr(a, b - a + 1));
        a = b + 1;
    }
    return out;
}

std::string rstrip(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string strip(const std::string& s) {
    auto r = rstrip(s);
    std::size_t a = 0;
    while (a < r.size() && std::isspace(static_cast<unsigned char>(r[a]))) ++a;
    return r.substr(a);
}

std::string one_decimal(double s) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", s);
    return buf;
}

struct Run {
    std::string status;
    double seconds = 0;
    std::string message;
};

Run run_one(const fs::path& prism, const fs::path& src, const std::string& fn, int n, double timeout,
            const fs::path& repo) {
    const auto out = src.parent_path() / ("out_" + fn + "_" + std::to_string(n));
    detail::SessionOptions so;
    so.cwd = repo;
    so.timeout_s = timeout;
    auto r = detail::run_session({prism.string(), src.string(), "--no-llm", "--stage", "inventory,classify,pir",
                                  "--out", out.string(), "--unwind", std::to_string(2 * n + 2), "--solver-cache",
                                  (src.parent_path() / ("cache_" + fn + "_" + std::to_string(n))).string()},
                                 so);
    if (r.timed_out) return {"TIMEOUT", timeout, ""};
    const double secs = std::round(r.seconds * 10) / 10;
    auto rep = load_json(out / "report.json");
    if (!rep) return {"ERROR", secs, ""};
    static const std::vector<std::string> order = {"FAILED", "ERROR", "NEEDS-HARNESS", "UNENCODED", "BOUNDED",
                                                   "PROVED"};
    for (const auto& st : rep->value("stages", nlohmann::json::array())) {
        if (st.value("name", "") != "pir") continue;
        std::vector<nlohmann::json> rows;
        for (const auto& f : st.value("findings", nlohmann::json::array()))
            if (f.value("function", nlohmann::json()).is_string() && f["function"].get<std::string>() == fn)
                rows.push_back(f);
        if (rows.empty()) continue;
        auto rank = [&](const nlohmann::json& f) {
            auto s = f.value("status", "");
            auto it = std::find(order.begin(), order.end(), s);
            return it == order.end() ? 0 : static_cast<int>(it - order.begin());
        };
        std::stable_sort(rows.begin(), rows.end(), [&](auto& a, auto& b) { return rank(a) < rank(b); });
        auto msg = rows[0].value("message", nlohmann::json(""));
        auto m = msg.is_string() ? msg.get<std::string>() : msg.dump();
        return {rows[0].value("status", ""), secs, m.substr(0, 160)};
    }
    return {"MISSING", secs, ""};
}
}  // namespace

HarnessSplit split_harness(const std::string& text) {
    static const Regex func(R"(^int (\w+)\()");
    HarnessSplit out;
    const auto lines = lines_keepends(text);
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (func.match_prefix(lines[i])) starts.push_back(i);
    if (starts.empty()) {
        out.prelude = text;
        return out;
    }
    auto comment_start = [&](std::size_t i) {
        std::size_t j = i;
        while (j > 0 && (lines[j - 1].starts_with("/*") || lines[j - 1].starts_with(" *") ||
                         lines[j - 1].starts_with("//") || strip(lines[j - 1]) == "*/"))
            --j;
        return j;
    };
    for (std::size_t k = 0; k < comment_start(starts[0]); ++k) out.prelude += lines[k];
    // one-line static helpers between harnesses (string_contracts.c `sign`)
    for (std::size_t k = starts[0]; k < lines.size(); ++k)
        if (lines[k].starts_with("static ") && rstrip(lines[k]).ends_with("}")) out.prelude += lines[k];
    for (auto i : starts) {
        const auto name = func.match_prefix(lines[i])->group(1);
        const auto opens = std::count(lines[i].begin(), lines[i].end(), '{');
        const auto closes = std::count(lines[i].begin(), lines[i].end(), '}');
        const bool one_liner = opens > 0 && opens == closes;
        std::size_t j = i;
        while (!one_liner && j + 1 < lines.size() && !lines[j].starts_with("}")) ++j;
        std::string body;
        for (std::size_t k = comment_start(i); k <= j; ++k) body += lines[k];
        auto it = std::find_if(out.funcs.begin(), out.funcs.end(), [&](auto& p) { return p.first == name; });
        if (it == out.funcs.end()) out.funcs.emplace_back(name, body);
        else it->second = body;
    }
    return out;
}

std::string absolutize_includes(const std::string& prelude, const fs::path& suite) {
    static const Regex inc(R"re(#include "([^"]+)")re");
    std::string out;
    std::size_t at = 0;
    std::error_code ec;
    for (const auto& m : inc.finditer(prelude)) {
        const auto [s, e] = m.spans[0];
        out.append(prelude, at, static_cast<std::size_t>(s) - at);
        out += "#include \"" + fs::weakly_canonical(suite / m.group(1), ec).string() + "\"";
        at = static_cast<std::size_t>(e);
    }
    out.append(prelude, at, std::string::npos);
    return out;
}

int libc_bounds_main(const Args& args) {
    const auto repo = repo_root();
    const auto suite = repo / "tests" / "conformance" / "libc-models";
    std::string bin, sizes_arg = "4,8,16,32,64", filter;
    double timeout = 180.0;
    int jobs = 2;
    fs::path json_out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--prism" || a == "--bin") bin = next();
        else if (a == "--sizes") sizes_arg = next();
        else if (a == "--timeout") timeout = std::atof(next().c_str());
        else if (a == "--jobs" || a == "-j") jobs = std::max(1, std::atoi(next().c_str()));
        else if (a == "--filter") filter = next();
        else if (a == "--json") json_out = next();
        else if (a == "-h" || a == "--help") {
            std::cout << "prism-qa libc-bounds [--prism PRISM] [--sizes 4,8,16,32,64] [--timeout S] [-j N]\n"
                         "                     [--filter RE] [--json OUT.json]\n"
                         "Largest object size N for which each libc model contract harness is still\n"
                         "PROVED (a proof for objects up to N bytes only, never for all sizes).\n";
            return 0;
        } else {
            std::cerr << "libc-bounds: unknown option " << a << "\n";
            return 2;
        }
    }
    std::vector<int> sizes;
    {
        std::istringstream in(sizes_arg);
        for (std::string s; std::getline(in, s, ',');)
            if (!s.empty()) sizes.push_back(std::atoi(s.c_str()));
    }
    auto prism = find_prism(bin, repo);
    if (!prism) {
        std::cout << "libc-bounds: NOTRUN: no PRISM binary (pass --prism or set PRISM_BIN)\n";
        return 2;
    }
    std::optional<Regex> rx;
    if (!filter.empty()) rx.emplace(filter);
    static const Regex sized(R"(\bN\b|MAKE_STR|MAKE_BYTES)");
    struct Job {
        std::string file, fn, text;
    };
    std::vector<Job> todo;
    nlohmann::json results = nlohmann::json::object();
    std::vector<fs::path> harnesses;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(suite, ec))
        if (e.is_regular_file() && e.path().filename().string().ends_with("_contracts.c")) harnesses.push_back(e.path());
    std::sort(harnesses.begin(), harnesses.end());
    auto pad = [](const std::string& s, std::size_t w) { return s.size() >= w ? s : s + std::string(w - s.size(), ' '); };
    for (const auto& f : harnesses) {
        auto split = split_harness(read_text(f));
        for (const auto& [fn, body] : split.funcs) {
            if (!fn.ends_with("_true") || (rx && !rx->search(fn))) continue;
            if (!sized.search(body)) {
                // no object of size N: the harness has no size bound to raise
                std::cout << pad(f.filename().string(), 22) << " " << pad(fn, 24)
                          << " no N in the harness (fixed or symbolic object sizes)" << std::endl;
                results[fn] = {{"file", f.filename().string()}, {"largest_proved_N", nullptr},
                               {"runs", nlohmann::json::object()}};
                continue;
            }
            todo.push_back({f.filename().string(), fn, absolutize_includes(split.prelude, suite) + "\n" + body});
        }
    }
    TempDir tmp("prism-libc-bounds-");
    std::vector<std::optional<std::vector<std::pair<int, Run>>>> done(todo.size());
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<std::size_t> next_job{0};
    auto worker = [&] {
        for (;;) {
            const auto k = next_job.fetch_add(1);
            if (k >= todo.size()) return;
            const auto& job = todo[k];
            std::vector<std::pair<int, Run>> per;
            for (int n : sizes) {
                const auto d = tmp.path / (job.fn + "_" + std::to_string(n));
                std::error_code e2;
                fs::create_directories(d, e2);
                const auto src = d / (job.fn + ".c");
                std::ofstream(src, std::ios::binary) << "#define N " << n << "\n" << job.text;
                per.emplace_back(n, run_one(*prism, src, job.fn, n, timeout, repo));
                if (per.back().second.status != "PROVED") break;
            }
            std::lock_guard lk(mu);
            done[k] = std::move(per);
            cv.notify_all();
        }
    };
    std::vector<std::jthread> pool;
    for (int i = 0; i < std::max(1, jobs); ++i) pool.emplace_back(worker);
    for (std::size_t k = 0; k < todo.size(); ++k) {
        std::vector<std::pair<int, Run>> per;
        {
            std::unique_lock lk(mu);
            cv.wait(lk, [&] { return done[k].has_value(); });
            per = *done[k];
        }
        int best = 0;
        nlohmann::json runs = nlohmann::json::object();
        std::string trail;
        for (const auto& [n, r] : per) {
            if (r.status == "PROVED") best = std::max(best, n);
            nlohmann::json rj{{"status", r.status}, {"seconds", r.seconds}};
            if (!r.message.empty()) rj["message"] = r.message;
            runs[std::to_string(n)] = rj;
            trail += (trail.empty() ? "" : ", ") + ("N=" + std::to_string(n) + ": " + r.status + " " +
                                                    one_decimal(r.seconds) + "s");
        }
        results[todo[k].fn] = {{"file", todo[k].file}, {"largest_proved_N", best}, {"runs", runs}};
        const std::string b = best ? std::to_string(best) : "-";
        std::cout << pad(todo[k].file, 22) << " " << pad(todo[k].fn, 24) << " largest N proved: "
                  << std::string(b.size() < 3 ? 3 - b.size() : 0, ' ') << b << "   (" << trail << ")" << std::endl;
    }
    pool.clear();
    if (!json_out.empty()) std::ofstream(json_out, std::ios::binary) << results.dump(1) << "\n";
    return 0;
}

}  // namespace prism::qa
