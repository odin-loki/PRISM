#include <filesystem>
#include <cstring>
#include "prism/membudget.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <chrono>
#include <map>
#include <thread>

#ifdef __linux__
#include <malloc.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace prism::membudget {
namespace {

std::atomic<uint64_t> g_budget{0};
std::atomic<uint64_t> g_test{0};
std::atomic<bool> g_low_trip{false};
std::string g_source;
std::once_flag g_once;

uint64_t read_u64_file(const char* path) {
    std::ifstream in(path);
    std::string s;
    if (!(in >> s) || s == "max") return 0;
    try {
        return std::stoull(s);
    } catch (...) {
        return 0;
    }
}

uint64_t meminfo_mb(const char* key) {
    std::ifstream in("/proc/meminfo");
    std::string k;
    uint64_t v = 0;
    std::string unit;
    while (in >> k >> v >> unit)
        if (k == key) return v / 1024;
    return 0;
}

void compute() {
#ifdef __linux__
    // glibc reserves a 64 MB heap arena per thread, up to 8 per core: 4 GB of
    // address space on 8 cores with no memory behind it. Under an RLIMIT_AS
    // (prlimit --as=4G) that ran out at ~28 MB RSS: a failed allocation inside
    // Z3's rewriter, then a null deref in th_rewriter (sstream_contracts.cpp,
    // unordered_map_contracts.cpp). Two arenas: on testdata/conformance/sv-comp (115 units, --jobs 2) peak RSS
    // was 826 MB with 4 and 276 MB with 2, same verdicts, 518 s vs 235 s.
    if (!std::getenv("MALLOC_ARENA_MAX")) mallopt(M_ARENA_MAX, 2);
#endif
    uint64_t mb = 0;
    for (const char* env : {"PRISM_MEM_BUDGET_MB", "PRISM_PIR_MEM_MB"}) {
        if (const char* e = std::getenv(env); e && *e) {
            mb = std::strtoull(e, nullptr, 10);
            g_source = std::string("$") + env;
            g_budget.store(mb);
            return;
        }
    }
#ifdef __linux__
    std::string src;
    auto take = [&](uint64_t v, const char* what) {
        if (v == 0) return;
        if (mb == 0 || v < mb) {
            mb = v;
            src = what;
        }
    };
    // cgroup v2, then v1 (a value near 2^63 means no limit)
    uint64_t cg = read_u64_file("/sys/fs/cgroup/memory.max");
    uint64_t cur = read_u64_file("/sys/fs/cgroup/memory.current");
    if (cg == 0) {
        cg = read_u64_file("/sys/fs/cgroup/memory/memory.limit_in_bytes");
        cur = read_u64_file("/sys/fs/cgroup/memory/memory.usage_in_bytes");
    }
    if (cg > 0 && cg < (uint64_t{1} << 60)) take((cg - std::min(cg, cur)) / (1024 * 1024), "cgroup memory limit");
    take(meminfo_mb("MemAvailable:"), "MemAvailable");
    struct rlimit rl{};
    if (::getrlimit(RLIMIT_AS, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        take(static_cast<uint64_t>(rl.rlim_cur) / (1024 * 1024) / 2, "RLIMIT_AS/2");
    mb = mb * 3 / 4;
    g_source = "75% of " + src;
#endif
    g_budget.store(mb);
}

}  // namespace

uint64_t budget_mb() {
    if (uint64_t t = g_test.load()) return t;
    std::call_once(g_once, compute);
    return g_budget.load();
}

uint64_t base_budget_mb() {
    std::call_once(g_once, compute);
    return g_budget.load();
}

const std::string& budget_source() {
    std::call_once(g_once, compute);
    return g_source;
}

uint64_t per_job_mb(int jobs) {
    const uint64_t b = budget_mb();
    return jobs > 1 ? b / static_cast<uint64_t>(jobs) : b;
}

uint64_t rss_mb() {
#ifdef __linux__
    long pages = 0, rss = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0;
        std::fclose(f);
    }
    return static_cast<uint64_t>(rss) * static_cast<uint64_t>(::sysconf(_SC_PAGE_SIZE)) / (1024 * 1024);
#else
    return 0;
#endif
}

#ifdef __linux__
namespace {
uint64_t proc_rss_kb(const std::string& pid) {
    uint64_t kb = 0;
    if (FILE* f = std::fopen(("/proc/" + pid + "/status").c_str(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f))
            if (std::strncmp(line, "VmRSS:", 6) == 0) { kb = std::strtoull(line + 6, nullptr, 10); break; }
        std::fclose(f);
    }
    return kb;
}
uint64_t children_rss_kb(const std::string& pid, int depth) {
    if (depth > 8) return 0;
    uint64_t kb = 0;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator("/proc/" + pid + "/task", ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::ifstream in(it->path() / "children");
        std::string c;
        while (in >> c) kb += proc_rss_kb(c) + children_rss_kb(c, depth + 1);
    }
    return kb;
}
}  // namespace
#endif

// This process and every process it started (solver children, z3-worker):
// the budget is for the whole tree, as prism-run measures it (pir6 peaked at
// 3439 MB of tree RSS with a 3000 MB budget while the process alone was under).
uint64_t tree_rss_mb() {
#ifdef __linux__
    return rss_mb() + children_rss_kb(std::to_string(::getpid()), 0) / 1024;
#else
    return rss_mb();
#endif
}

bool system_low() {
#ifdef __linux__
    static const uint64_t reserve = [] {
        const char* e = std::getenv("PRISM_MEM_RESERVE_MB");
        return e && *e ? std::strtoull(e, nullptr, 10) : uint64_t{1500};
    }();
    if (reserve == 0) return false;
    const uint64_t avail = meminfo_mb("MemAvailable:");
    return avail > 0 && avail < reserve;
#else
    return false;
#endif
}

bool over_budget() {
    const uint64_t b = budget_mb();
    const uint64_t rss = tree_rss_mb();
    // A low box stops only a function that is itself a big consumer (half
    // its budget): vector_insert.cpp was stopped at 789 MB of 3000 because
    // other programs had filled the box.
    if (system_low() && (b == 0 || rss >= b / 2)) { g_low_trip.store(true); return true; }
    if (b == 0) return false;
    // trip at 80%: Z3 stops at its next interrupt check and the watchdog
    // samples every 50 ms, so the peak lands above the trip point
    // (priority_queue_contracts.cpp: 3.5 GB peak with a 3 GB trip)
    if (rss >= b * 4 / 5) return true;
    // Fast growth (over ~1 GB/s, unordered_map_contracts.cpp was killed at
    // 3379 MB): trip at 70% so the stop latency still lands under budget.
    static std::mutex mu;
    static uint64_t last_rss = 0;
    static std::chrono::steady_clock::time_point last_t{};
    static bool fast = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        const auto now = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_t).count();
        if (ms >= 50) {
            fast = last_rss > 0 && rss > last_rss && (rss - last_rss) * 1000 / static_cast<uint64_t>(ms) >= 1024;
            last_rss = rss;
            last_t = now;
        }
        if (fast && rss >= b * 7 / 10) return true;
    }
    return false;
}

namespace {
struct Reg {
    std::function<void()> stop;
    bool fired = false;
};
std::mutex g_wmu;
std::map<unsigned long, Reg> g_watches;
unsigned long g_next = 1;
std::atomic<uint64_t> g_trips{0};
bool g_thread = false;

std::mutex g_hmu;
std::function<void()> g_stuck_hook;

void watchdog() {
    using clk = std::chrono::steady_clock;
    clk::time_point streak{};
    bool hooked = false;
    const long stuck_ms = [] {
        const char* e = std::getenv("PRISM_MEM_STUCK_MS");
        return e && *e ? std::strtol(e, nullptr, 10) : 1000L;
    }();
    for (;;) {
        // 20 ms near the limit (from 60% of budget): a Z3 query or an encoder
        // burst grows hundreds of MB between 50 ms samples (~30% overshoot).
        {
            const uint64_t b = budget_mb();
            const bool near = b > 0 && rss_mb() >= b * 3 / 5;
            std::this_thread::sleep_for(std::chrono::milliseconds(near ? 20 : 50));
        }
        {
            std::lock_guard<std::mutex> lk(g_wmu);
            if (g_watches.empty()) continue;
        }
        if (!over_budget()) {
            streak = {};
            hooked = false;
            continue;
        }
        if (streak == clk::time_point{}) streak = clk::now();
        if (!hooked && stuck_ms > 0 && clk::now() - streak >= std::chrono::milliseconds(stuck_ms)) {
            hooked = true;
            std::function<void()> h;
            { std::lock_guard<std::mutex> lk(g_hmu); h = g_stuck_hook; }
            if (std::getenv("PRISM_MEM_TRACE"))
                std::fprintf(stderr, "[memtrace] stuck %ld ms rss=%llu phases=%s: hard stop\n", stuck_ms,
                             static_cast<unsigned long long>(rss_mb()), phases().c_str());
            if (h) h();
        }
        g_trips.fetch_add(1);
        if (std::getenv("PRISM_MEM_TRACE")) {
            std::size_t n = 0;
            { std::lock_guard<std::mutex> lk(g_wmu); n = g_watches.size(); }
            std::fprintf(stderr, "[memtrace] %lld trip rss=%llu watches=%zu phases=%s\n",
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()),
                         static_cast<unsigned long long>(rss_mb()), n, phases().c_str());
        }
        std::lock_guard<std::mutex> lk(g_wmu);
        for (auto& [id, r] : g_watches) {
            r.fired = true;
            if (r.stop) r.stop();
        }
    }
}
}  // namespace

Watch::Watch(std::function<void()> stop) {
    std::lock_guard<std::mutex> lk(g_wmu);
    id_ = g_next++;
    g_watches[id_] = Reg{std::move(stop), false};
    if (!g_thread) {
        g_thread = true;
        std::thread(watchdog).detach();
    }
}

Watch::~Watch() {
    std::lock_guard<std::mutex> lk(g_wmu);
    g_watches.erase(id_);
}

uint64_t trips() { return g_trips.load(); }

void set_stuck_hook(std::function<void()> hook) {
    std::lock_guard<std::mutex> lk(g_hmu);
    g_stuck_hook = std::move(hook);
}

namespace {
std::mutex g_pmu;
std::map<std::thread::id, const char*> g_phase;
}  // namespace
Phase::Phase(const char* name) {
    std::lock_guard<std::mutex> lk(g_pmu);
    auto& slot = g_phase[std::this_thread::get_id()];
    prev_ = slot;
    slot = name;
}
Phase::~Phase() {
    std::lock_guard<std::mutex> lk(g_pmu);
    if (prev_) g_phase[std::this_thread::get_id()] = prev_;
    else g_phase.erase(std::this_thread::get_id());
}
std::string phases() {
    std::lock_guard<std::mutex> lk(g_pmu);
    std::string out;
    for (auto& [id, n] : g_phase) out += (out.empty() ? "" : ",") + std::string(n ? n : "?");
    return out;
}

bool Watch::fired() const {
    std::lock_guard<std::mutex> lk(g_wmu);
    auto it = g_watches.find(id_);
    return it != g_watches.end() && it->second.fired;
}

bool over_budget_sampled(unsigned every) {
    thread_local unsigned n = 0;
    thread_local bool last = false;
    if (++n < every && !last) return false;
    n = 0;
    last = over_budget();
    return last;
}

void release_free_memory() {
#ifdef __linux__
    malloc_trim(0);
#endif
}

std::string over_message(const char* where) {
    if (g_low_trip.exchange(false) || system_low())
        return std::string("machine memory low (MemAvailable ") + std::to_string(meminfo_mb("MemAvailable:")) +
               " MB, PRISM_MEM_RESERVE_MB) " + where + "; stopped by the memory budget watchdog; no proof";
    return std::string("memory budget reached ") + where + " (process RSS " + std::to_string(rss_mb()) + " MB, budget " +
           std::to_string(budget_mb()) + " MB from " + (g_test.load() ? std::string("a test") : budget_source()) +
           "; PRISM_MEM_BUDGET_MB); no proof";
}

void set_budget_mb_for_test(uint64_t mb) { g_test.store(mb); }

}  // namespace prism::membudget
