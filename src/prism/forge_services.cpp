// PRISM's runtime services on FORGE: exec.hpp -> forge::os::spawn_wait,
// pool.hpp -> forge::task::pool, cache.hpp -> forge::cache::store.
// FORGE is a required dependency; there are no other backends.
#include "prism/cache.hpp"
#include "prism/exec.hpp"
#include "prism/pool.hpp"
#include "prism/sandbox.hpp"
#include "prism/threads.hpp"

#include <forge/cache/cache.hpp>
#include <forge/os/process.hpp>
#include <forge/task/pool.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <stop_token>
#include <thread>

namespace fs = std::filesystem;

namespace prism::exec {

namespace {
std::atomic<int> g_signal{0};
std::atomic<int> g_active{0};
std::stop_source& interrupt_source() {
    static std::stop_source s;
    return s;
}
void on_interrupt_signal(int sig) { g_signal.store(sig); }
struct ActiveRun {
    ActiveRun() { ++g_active; }
    ~ActiveRun() { --g_active; }
};
}  // namespace

void refuse_disabled_checks(const std::vector<std::string>& argv) {
    for (const auto& flag : argv)
        if (flag.starts_with("--no-") && flag.ends_with("-check"))
            throw std::runtime_error("refusing to disable a check: " + flag);
}

void apply_limits(Spec& s, bool enabled, double cpu_seconds, bool limit_as) {
    if (!enabled) return;
    auto cpu = static_cast<std::uint64_t>(cpu_seconds + 0.999);
    if (cpu < 1) cpu = 1;
    s.cpu_seconds = cpu + 1;  // the parent's timeout fires first
    s.open_files = sandbox::LIMIT_NOFILE;
    s.file_size_bytes = sandbox::LIMIT_FSIZE_BYTES;
    s.no_core = true;
    if (limit_as) s.memory_bytes = sandbox::LIMIT_AS_BYTES;
}

Result run(const Spec& s) {
    refuse_disabled_checks(s.argv);
    forge::os::spawn_spec sp;
    sp.merge_stderr = s.merge_stderr;
    sp.limits.file_size_bytes = s.file_size_bytes;
    sp.limits.no_core = s.no_core;
    sp.stdin_file = s.stdin_file;
    sp.stdout_file = s.stdout_file;
    sp.stderr_file = s.stderr_file;
    sp.append_output = s.append_output;
    sp.new_session = s.new_session;
    sp.watch_memory_bytes = s.watch_memory_bytes;
    // One stop token: PRISM's interrupt, or the caller's flag (polled every 20 ms).
    std::stop_source local;
    std::stop_callback on_interrupt(interrupt_source().get_token(), [&local] { local.request_stop(); });
    std::jthread poll;
    if (s.stop)
        poll = std::jthread([&local, flag = s.stop](std::stop_token st) {
            while (!st.stop_requested() && !local.stop_requested()) {
                if (flag->load(std::memory_order_relaxed)) {
                    local.request_stop();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    sp.stop = local.get_token();
    ActiveRun active;
    sp.argv = s.argv;
    sp.cwd = s.cwd;
    sp.env_set = s.env_set;
    sp.env_unset = s.env_unset;
    sp.stdin_data = s.stdin_data;
    sp.timeout_ms = s.timeout_ms;
    sp.max_output = s.max_output;
    sp.limits.memory_bytes = s.memory_bytes;
    sp.limits.cpu_seconds = s.cpu_seconds;
    sp.limits.open_files = s.open_files;
    auto r = forge::os::spawn_wait(sp);
    Result res;
    if (r.err) {
        res.failed = true;
        res.err = r.err.what;
        return res;
    }
    res.exit_code = r.value.exit_code;
    res.term_signal = r.value.term_signal;
    res.timed_out = r.value.timed_out;
    res.truncated = r.value.truncated;
    res.out = std::move(r.value.out);
    res.err = std::move(r.value.err);
    res.wall_ms = r.value.wall_ms;
    res.cancelled = r.value.cancelled;
    res.memory_killed = r.value.memory_killed;
    res.unreaped = r.value.unreaped;
    res.peak_rss_bytes = r.value.peak_rss_bytes;
    return res;
}

ProcOut run_process(const std::vector<std::string>& args, double timeout_s, const fs::path& cwd) {
    Spec s;
    s.argv = args;
    s.cwd = cwd.string();
    s.timeout_ms = timeout_s > 0 ? static_cast<std::uint64_t>(timeout_s * 1000) : 0;
    s.merge_stderr = true;
    s.max_output = ~0ull;
    auto r = run(s);
    ProcOut o;
    o.text = r.failed ? r.err : std::move(r.out);
    o.rc = r.rc();
    o.timed_out = r.timed_out;
    o.failed = r.failed;
    return o;
}

SessionOut run_session(const std::vector<std::string>& args, const SessionOptions& opt) {
    Spec s;
    s.argv = args;
    s.cwd = opt.cwd.string();
    for (const auto& [k, v] : opt.env) s.env_set[k] = v;
    s.stdin_data = opt.input;
    s.timeout_ms = opt.timeout_s > 0 ? static_cast<std::uint64_t>(opt.timeout_s * 1000) : 0;
    s.memory_bytes = opt.rlimit_as_mb > 0 ? static_cast<std::uint64_t>(opt.rlimit_as_mb) << 20 : 0;
    s.new_session = true;
    s.max_output = ~0ull;
    auto r = run(s);
    SessionOut o;
    o.out = std::move(r.out);
    o.err = std::move(r.err);
    o.rc = r.failed ? -1 : r.rc();
    o.timed_out = r.timed_out;
    o.failed = r.failed;
    o.seconds = static_cast<double>(r.wall_ms) / 1000.0;
    return o;
}

SessionOut run_session(const std::vector<std::string>& args, double timeout_s, const fs::path& cwd) {
    SessionOptions o;
    o.timeout_s = timeout_s;
    o.cwd = cwd;
    return run_session(args, o);
}

void install_interrupt_cleanup() noexcept {
    static std::once_flag once;
    std::call_once(once, [] {
        auto hook = [](int sig) {
            auto prev = std::signal(sig, on_interrupt_signal);
            if (prev == SIG_IGN) std::signal(sig, SIG_IGN);  // ignored stays ignored
        };
        hook(SIGINT);
        hook(SIGTERM);
#ifdef SIGHUP
        hook(SIGHUP);
#endif
        // Not in the handler (not async-signal-safe): cancel the runs, wait up to
        // 2 s for them to reap their trees, then die of the same signal.
        std::thread([] {
            for (;;) {
                const int sig = g_signal.load();
                if (sig != 0) {
                    interrupt_source().request_stop();
                    for (int i = 0; i < 100 && g_active.load() > 0; ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    std::signal(sig, SIG_DFL);
                    std::raise(sig);
                    std::_Exit(128 + sig);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }).detach();
    });
}

}  // namespace prism::exec

namespace prism::pool {
namespace {

std::mutex g_m;
std::map<unsigned, std::unique_ptr<forge::task::pool>> g_pools;  // one per worker count

forge::task::pool& pool_for(int jobs) {
    const auto n = static_cast<unsigned>(clamp_jobs(jobs));
    std::lock_guard<std::mutex> lk(g_m);
    auto& p = g_pools[n];
    if (!p) p = std::make_unique<forge::task::pool>(n);
    return *p;
}

}  // namespace

void parallel_for(int jobs, std::size_t n, std::size_t grain, const RangeFn& fn) {
    pool_for(jobs).parallel_for(n, grain, fn);
}
unsigned size() { return pool_for(0).size(); }

}  // namespace prism::pool

namespace prism::cache {
namespace {

std::mutex g_m;
std::map<fs::path, std::unique_ptr<forge::cache::store>> g_stores;

forge::cache::store& store_for(const fs::path& dir) {
    std::lock_guard<std::mutex> lk(g_m);
    auto& s = g_stores[fs::absolute(dir).lexically_normal()];
    if (!s) s = std::make_unique<forge::cache::store>(dir);
    return *s;
}

}  // namespace

std::string key_of(std::string_view data) { return forge::cache::to_hex(forge::cache::key_of(data)); }

bool put(const fs::path& dir, const std::string& key, std::string_view value) {
    auto k = forge::cache::from_hex(key);
    return k && store_for(dir).put(*k, value);
}
std::optional<std::string> get(const fs::path& dir, const std::string& key) {
    auto k = forge::cache::from_hex(key);
    if (!k) return std::nullopt;
    return store_for(dir).get(*k);
}
bool contains(const fs::path& dir, const std::string& key) {
    auto k = forge::cache::from_hex(key);
    return k && store_for(dir).contains(*k);
}
bool erase(const fs::path& dir, const std::string& key) {
    auto k = forge::cache::from_hex(key);
    return k && store_for(dir).erase(*k);
}
std::uint64_t prune(const fs::path& dir, std::uint64_t max_bytes) { return store_for(dir).prune(max_bytes); }
Stats stats(const fs::path& dir) {
    auto s = store_for(dir).stats();
    return {s.entries, s.bytes, s.hits, s.misses, s.evictions, s.corrupt};
}

}  // namespace prism::cache
