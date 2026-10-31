// Default backends of the thin interfaces exec.hpp, pool.hpp and cache.hpp.
// Each is replaceable at run time (set_backend); FORGE's process runner,
// task pool and BLAKE3 cache drop in as backends.
#include "prism/cache.hpp"
#include "prism/deps.hpp"
#include "prism/exec.hpp"
#include "prism/pool.hpp"
#include "prism/threads.hpp"

#include "proc.hpp"

#include <forge/task/pool.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------- exec
namespace prism::exec {
namespace {

Result default_run(const Spec& s) {
    detail::RunSpec r;
    r.argv = s.argv;
    r.cwd = s.cwd;
    for (const auto& [k, v] : s.env_set) r.env.emplace_back(k, v);
    // detail::run overlays only; an unset becomes an empty value.
    for (const auto& k : s.env_unset) r.env.emplace_back(k, std::string{});
    if (s.stdin_data) r.input = *s.stdin_data;
    r.timeout_s = s.timeout_ms ? static_cast<double>(s.timeout_ms) / 1000.0 : 0.0;
    r.max_output_bytes = static_cast<std::size_t>(s.max_output);
    r.mem_limit_mb = static_cast<long>(s.memory_bytes >> 20);
    if (s.cpu_seconds || s.memory_bytes) {
        r.limits.enabled = true;
        r.limits.cpu_seconds = static_cast<double>(s.cpu_seconds);
        r.limits.limit_as = s.memory_bytes != 0;
    }
    r.kill_descendants = true;
    const auto t0 = std::chrono::steady_clock::now();
    auto o = detail::run(r);
    Result res;
    res.wall_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    res.out = std::move(o.out);
    res.err = std::move(o.err);
    res.timed_out = o.timed_out;
    res.failed = o.failed;
    res.truncated = s.max_output && (res.out.size() >= s.max_output || res.err.size() >= s.max_output);
    if (o.rc < 0 && !o.failed) {
        res.term_signal = -o.rc;
        res.exit_code = 128 + res.term_signal;
    } else {
        res.exit_code = o.rc;
    }
    return res;
}

std::atomic<Backend> g_exec{&default_run};

}  // namespace

Backend set_backend(Backend b) { return g_exec.exchange(b ? b : &default_run); }
Result run(const Spec& spec) { return g_exec.load()(spec); }

}  // namespace prism::exec

// ---------------------------------------------------------------- pool
namespace prism::pool {
namespace {

// FORGE's task pool, sized by PRISM's --jobs policy (clamp_jobs); the caller
// is one of the workers.
void default_parallel_for(int jobs, std::size_t n, std::size_t grain, const RangeFn& fn) {
    if (n == 0) return;
    grain = std::max<std::size_t>(grain, 1);
    const std::size_t chunks = (n + grain - 1) / grain;
    const auto threads = static_cast<unsigned>(std::min<std::size_t>(chunks, static_cast<std::size_t>(clamp_jobs(jobs))));
    if (threads <= 1) {
        fn(0, n);
        return;
    }
    forge::task::pool p(threads);
    p.parallel_for(n, grain, fn);
}

unsigned default_size() { return static_cast<unsigned>(clamp_jobs(0)); }

std::mutex g_pool_m;
Backend g_pool{&default_parallel_for, &default_size};

Backend current() {
    std::lock_guard<std::mutex> lk(g_pool_m);
    return g_pool;
}

}  // namespace

Backend set_backend(Backend b) {
    if (!b.parallel_for) b.parallel_for = &default_parallel_for;
    if (!b.size) b.size = &default_size;
    std::lock_guard<std::mutex> lk(g_pool_m);
    return std::exchange(g_pool, b);
}

void parallel_for(int jobs, std::size_t n, std::size_t grain, const RangeFn& fn) {
    current().parallel_for(jobs, n, grain, fn);
}
unsigned size() { return current().size(); }

}  // namespace prism::pool

// ---------------------------------------------------------------- cache
namespace prism::cache {
namespace {

std::mutex g_file_m;  // serialises writers within one process
std::atomic<std::uint64_t> g_hits{0}, g_misses{0}, g_evictions{0}, g_corrupt{0};

bool valid_key(const std::string& k) {
    return k.size() == 64 && std::all_of(k.begin(), k.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

fs::path path_of(const fs::path& dir, const std::string& k) { return dir / k.substr(0, 2) / k.substr(2); }

std::string default_key_of(std::string_view data) { return deps::sha256_hex(data); }

// File layout: 64 hex digits of the value's digest, then the value. A
// mismatch on read is corruption: the entry is removed and reported a miss.
bool default_put(const fs::path& dir, const std::string& k, std::string_view value) {
    if (!valid_key(k)) return false;
    const auto p = path_of(dir, k);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const auto tmp = p.string() + ".tmp" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        const auto d = default_key_of(value);
        f.write(d.data(), static_cast<std::streamsize>(d.size()));
        f.write(value.data(), static_cast<std::streamsize>(value.size()));
        if (!f) return false;
    }
    std::lock_guard<std::mutex> lk(g_file_m);
    fs::rename(tmp, p, ec);
    if (ec) fs::remove(tmp, ec);
    return !ec;
}

std::optional<std::string> default_get(const fs::path& dir, const std::string& k) {
    if (!valid_key(k)) return std::nullopt;
    const auto p = path_of(dir, k);
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        ++g_misses;
        return std::nullopt;
    }
    std::string blob((std::istreambuf_iterator<char>(f)), {});
    f.close();
    if (blob.size() < 64 || blob.compare(0, 64, default_key_of(std::string_view(blob).substr(64))) != 0) {
        ++g_corrupt;
        ++g_misses;
        std::error_code ec;
        fs::remove(p, ec);
        return std::nullopt;
    }
    ++g_hits;
    std::error_code ec;
    fs::last_write_time(p, fs::file_time_type::clock::now(), ec);  // LRU touch
    return blob.substr(64);
}

bool default_erase(const fs::path& dir, const std::string& k) {
    if (!valid_key(k)) return false;
    std::error_code ec;
    return fs::remove(path_of(dir, k), ec);
}

struct Ent {
    fs::path p;
    fs::file_time_type t;
    std::uint64_t n;
};

std::vector<Ent> list(const fs::path& dir) {
    std::vector<Ent> v;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().filename().string().find(".tmp") != std::string::npos) continue;
        std::error_code e2;
        v.push_back({it->path(), it->last_write_time(e2), static_cast<std::uint64_t>(it->file_size(e2))});
    }
    return v;
}

std::uint64_t default_prune(const fs::path& dir, std::uint64_t max_bytes) {
    std::lock_guard<std::mutex> lk(g_file_m);
    auto v = list(dir);
    std::uint64_t total = 0;
    for (const auto& e : v) total += e.n;
    std::sort(v.begin(), v.end(), [](const Ent& a, const Ent& b) { return a.t < b.t; });
    std::uint64_t freed = 0;
    for (const auto& e : v) {
        if (total <= max_bytes) break;
        std::error_code ec;
        if (fs::remove(e.p, ec)) {
            total -= e.n;
            freed += e.n;
            ++g_evictions;
        }
    }
    return freed;
}

Stats default_stats(const fs::path& dir) {
    Stats s;
    for (const auto& e : list(dir)) {
        ++s.entries;
        s.bytes += e.n;
    }
    s.hits = g_hits;
    s.misses = g_misses;
    s.evictions = g_evictions;
    s.corrupt = g_corrupt;
    return s;
}

const Backend kDefault{&default_key_of, &default_put, &default_get, &default_erase, &default_prune, &default_stats};
std::mutex g_cache_m;
Backend g_cache = kDefault;

Backend current() {
    std::lock_guard<std::mutex> lk(g_cache_m);
    return g_cache;
}

}  // namespace

Backend set_backend(Backend b) {
    if (!b.key_of) b.key_of = kDefault.key_of;
    if (!b.put) b.put = kDefault.put;
    if (!b.get) b.get = kDefault.get;
    if (!b.erase) b.erase = kDefault.erase;
    if (!b.prune) b.prune = kDefault.prune;
    if (!b.stats) b.stats = kDefault.stats;
    std::lock_guard<std::mutex> lk(g_cache_m);
    return std::exchange(g_cache, b);
}

std::string key_of(std::string_view data) { return current().key_of(data); }
bool put(const fs::path& dir, const std::string& key, std::string_view value) { return current().put(dir, key, value); }
std::optional<std::string> get(const fs::path& dir, const std::string& key) { return current().get(dir, key); }
bool contains(const fs::path& dir, const std::string& key) {
    if (!valid_key(key)) return false;
    std::error_code ec;
    return current().get == kDefault.get ? fs::is_regular_file(path_of(dir, key), ec) : current().get(dir, key).has_value();
}
bool erase(const fs::path& dir, const std::string& key) { return current().erase(dir, key); }
std::uint64_t prune(const fs::path& dir, std::uint64_t max_bytes) { return current().prune(dir, max_bytes); }
Stats stats(const fs::path& dir) { return current().stats(dir); }

}  // namespace prism::cache
