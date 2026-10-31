// PRISM's runtime services on FORGE: exec.hpp -> forge::os::spawn_wait,
// pool.hpp -> forge::task::pool, cache.hpp -> forge::cache::store.
// FORGE is a required dependency; there are no other backends.
#include "prism/cache.hpp"
#include "prism/exec.hpp"
#include "prism/pool.hpp"
#include "prism/threads.hpp"

#include <forge/cache/cache.hpp>
#include <forge/os/process.hpp>
#include <forge/task/pool.hpp>

#include <map>
#include <memory>
#include <mutex>

namespace fs = std::filesystem;

namespace prism::exec {

Result run(const Spec& s) {
    forge::os::spawn_spec sp;
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
    return res;
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
