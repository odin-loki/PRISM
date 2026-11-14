// GPU ProbSAT host side (docs/GPU_PLAN.md G7): CNF upload, walker sizing to
// the FORGE device-memory budget, chunked launches of src/cuda/probsat.cu, and
// the host re-check of every clause. The kernel's answer is never trusted: a
// found assignment is checked here against the caller's CNF, and the portfolio
// validates the model against the original formula in Z3 (docs/TRUSTED_BASE.md).

#include "prism/solver.hpp"
#include "prism/cuda_driver.hpp"

#include "../../cuda/probsat_launch.h"

#include <algorithm>
#include <cstddef>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>

#if __has_include("prism/probsat_ptx.inc")
#include "prism/probsat_ptx.inc"  // kProbsatPtx[], kProbsatPtxSize (CMake, from src/cuda/probsat.cu)
#else
static const char kProbsatPtx[] = {0};
static const std::size_t kProbsatPtxSize = 0;
#endif

namespace prism::solver {

namespace {

constexpr const char* kModule = "probsat";
constexpr const char* kTag = "prism probsat";
constexpr std::uint32_t kMaxClauseLen = 255;  // range of the kernel's u8 true-literal counter
constexpr unsigned kWalkBlock = 128;
constexpr unsigned kWarpWalkersPerSm = 4;  // warp-per-walker default (measured best time-to-solution on planted 3-SAT)
constexpr double kLaunchTarget_s = 0.03;  // per walk launch: well under the display watchdog

struct Kernels {
    cuda::Kernel assign = nullptr, count = nullptr, walk = nullptr, walk_warp = nullptr;
};

bool load_kernels(cuda::Driver& d, Kernels& k, std::string* why) {
    if (!d.available()) {
        if (why) *why = d.why();
        return false;
    }
    if (!d.load_module(kModule, kProbsatPtxSize ? kProbsatPtx : nullptr, kProbsatPtxSize, why)) return false;
    k.assign = d.kernel(kModule, "prism_probsat_init_assign", why);
    k.count = k.assign ? d.kernel(kModule, "prism_probsat_init_count", why) : nullptr;
    k.walk = k.count ? d.kernel(kModule, "prism_probsat_walk", why) : nullptr;
    k.walk_warp = k.walk ? d.kernel(kModule, "prism_probsat_walk_warp", why) : nullptr;
    return k.walk_warp != nullptr;
}

// The CNF the kernel walks: literals deduplicated, tautologies dropped (both
// keep the formula's models), in CSR form with literal -> clause occurrence lists.
struct Csr {
    std::vector<ps_u32> cl_start{0}, occ_start, occ;
    std::vector<ps_i32> cl_lits;
    std::size_t maxlen = 0;
    std::uint32_t nc = 0;
};

std::optional<Csr> build_csr(const Cnf& cnf, std::string& why) {
    Csr g;
    const std::size_t nv = std::size_t(std::max(cnf.num_vars, 0));
    std::vector<int> cl;
    for (const auto& c : cnf.clauses) {
        if (c.empty()) {
            why = "the CNF has an empty clause (unsatisfiable): nothing for local search to find";
            return std::nullopt;
        }
        cl.assign(c.begin(), c.end());
        std::sort(cl.begin(), cl.end(), [](int a, int b) {
            const int x = std::abs(a), y = std::abs(b);
            return x != y ? x < y : a < b;
        });
        cl.erase(std::unique(cl.begin(), cl.end()), cl.end());
        bool taut = false;
        for (std::size_t i = 0; i < cl.size(); ++i) {
            if (cl[i] == 0 || std::size_t(std::abs(cl[i])) > nv) {
                why = "literal " + std::to_string(cl[i]) + " out of range";
                return std::nullopt;
            }
            if (i && cl[i] == -cl[i - 1]) taut = true;
        }
        if (taut) continue;
        if (cl.size() > kMaxClauseLen) {
            why = "a clause has " + std::to_string(cl.size()) + " literals (the GPU walker holds at most " +
                  std::to_string(kMaxClauseLen) + "; the CPU walker has no limit)";
            return std::nullopt;
        }
        g.maxlen = std::max(g.maxlen, cl.size());
        g.cl_lits.insert(g.cl_lits.end(), cl.begin(), cl.end());
        g.cl_start.push_back(ps_u32(g.cl_lits.size()));
    }
    if (g.cl_lits.size() >= std::numeric_limits<ps_u32>::max()) {
        why = "the CNF has more than 2^32 literals";
        return std::nullopt;
    }
    g.nc = ps_u32(g.cl_start.size() - 1);
    std::vector<ps_u32> cnt(2 * nv + 2, 0);
    for (ps_i32 l : g.cl_lits) ++cnt[std::size_t(l < 0 ? 2 * -l + 1 : 2 * l)];
    g.occ_start.assign(2 * nv + 3, 0);
    for (std::size_t i = 0; i < cnt.size(); ++i) g.occ_start[i + 1] = g.occ_start[i] + cnt[i];
    g.occ.resize(g.cl_lits.size());
    std::vector<ps_u32> fill(g.occ_start.begin(), g.occ_start.end() - 1);
    for (ps_u32 c = 0; c < g.nc; ++c)
        for (ps_u32 k = g.cl_start[c]; k < g.cl_start[c + 1]; ++k) {
            const ps_i32 l = g.cl_lits[k];
            g.occ[fill[std::size_t(l < 0 ? 2 * -l + 1 : 2 * l)]++] = c;
        }
    return g;
}

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

bool gpu_sls_available(std::string* why) {
    Kernels k;
    return load_kernels(cuda::Driver::get(), k, why);
}

GpuSlsResult gpu_probsat(const Cnf& cnf, const GpuSlsOptions& opt) {
    GpuSlsResult res;
    const double t_setup = now_s();
    auto& d = cuda::Driver::get();
    res.device = d.describe();
    Kernels K;
    std::string why;
    if (!load_kernels(d, K, &why)) {
        res.note = "GPU local search not run: " + why;
        return res;
    }
    auto csr = build_csr(cnf, why);
    if (!csr) {
        res.note = "GPU local search not run: " + why;
        return res;
    }
    const std::size_t nv = std::size_t(std::max(cnf.num_vars, 0));
    if (csr->nc == 0) {  // every clause was a tautology (or there were none)
        res.assignment.assign(nv + 1, 0);
        res.found = assignment_satisfies(cnf, res.assignment);
        res.note = "no non-tautological clause: every assignment satisfies (GPU not needed)";
        return res;
    }
    const ps_u32 nc = csr->nc;
    const ps_u32 words = ps_u32((nv + 1 + 31) / 32);

    // Sizing: per-walker state against the budget (FORGE pool cuda:0, and by
    // default a quarter of the free device memory, which FORGE inference shares).
    const std::uint64_t per_walker = 9ull * nc + 4ull * words + sizeof(PsWalker);
    const std::uint64_t shared = 4ull * csr->cl_start.size() + 4ull * csr->cl_lits.size() +
                                 4ull * csr->occ_start.size() + 4ull * csr->occ.size() + 4ull * PS_WEIGHTS + 16 +
                                 64 * 8;  // allocation rounding slack
    std::size_t free_b = 0, total_b = 0;
    d.mem_info(&free_b, &total_b);
    std::uint64_t budget = opt.max_device_bytes ? opt.max_device_bytes : std::uint64_t(free_b) / 4;
    budget = std::min<std::uint64_t>(budget, cuda::Driver::pool_available());
    const std::uint64_t fit = budget > shared ? (budget - shared) / per_walker : 0;
    if (fit == 0) {
        res.note = "GPU local search not run: one walker (" + std::to_string(per_walker + shared) +
                   " bytes) does not fit the device-memory budget (" + std::to_string(budget) + " bytes)";
        return res;
    }
    const bool warp = opt.lanes_per_walker != 1;
    const std::uint64_t want =
        opt.walkers ? opt.walkers : std::uint64_t(std::max(1, d.sm_count())) * (warp ? kWarpWalkersPerSm : 128);
    const unsigned W = unsigned(std::min<std::uint64_t>({want, fit, 1u << 24}));
    if (opt.walkers && W < opt.walkers)
        res.note = "walkers reduced from " + std::to_string(opt.walkers) + " to " + std::to_string(W) +
                   " to fit the device-memory budget; ";
    res.walkers = W;
    res.lanes_per_walker = warp ? 32 : 1;

    // ProbSAT defaults (as probsat() in cnf.cpp): cb from the longest clause, eps 1.
    const double cbv = csr->maxlen <= 3 ? 2.38 : csr->maxlen <= 4 ? 3.0 : csr->maxlen <= 5 ? 3.7 : 5.4;
    float weight[PS_WEIGHTS];
    for (int b = 0; b < PS_WEIGHTS; ++b) weight[b] = float(std::pow(1.0 + b, -cbv));

    auto fail = [&](const std::string& m) {
        res.note += "GPU local search failed: " + m;
        res.ran = false;
        return res;
    };
    cuda::Stream s = d.stream(&why);
    if (!s.handle()) return fail(why);
    auto up = [&](const auto& v, cuda::Buffer& b) {
        const std::size_t n = v.size() * sizeof(v[0]);
        b = d.alloc(n, kTag, &why);
        return b && b.upload(v.data(), n, 0, &why);
    };
    cuda::Buffer b_cls, b_lits, b_occs, b_occ, b_w, b_asg, b_nt, b_wh, b_us, b_walk, b_found;
    std::vector<float> wv(weight, weight + PS_WEIGHTS);
    if (!up(csr->cl_start, b_cls) || !up(csr->cl_lits, b_lits) || !up(csr->occ_start, b_occs) ||
        !up(csr->occ, b_occ) || !up(wv, b_w))
        return fail(why);
    auto mk = [&](std::uint64_t n, cuda::Buffer& b) {
        b = d.alloc(std::size_t(n), kTag, &why);
        return bool(b);
    };
    if (!mk(4ull * words * W, b_asg) || !mk(1ull * nc * W, b_nt) || !mk(4ull * nc * W, b_wh) ||
        !mk(4ull * nc * W, b_us) || !mk(sizeof(PsWalker) * W, b_walk) || !mk(16, b_found) || !b_found.zero(&why))
        return fail(why);
    res.device_bytes = b_cls.bytes() + b_lits.bytes() + b_occs.bytes() + b_occ.bytes() + b_w.bytes() +
                       b_asg.bytes() + b_nt.bytes() + b_wh.bytes() + b_us.bytes() + b_walk.bytes() + b_found.bytes();

    PsLaunch L{};
    L.cl_start = b_cls.as<const ps_u32>();
    L.cl_lits = b_lits.as<const ps_i32>();
    L.occ_start = b_occs.as<const ps_u32>();
    L.occ = b_occ.as<const ps_u32>();
    L.weight = b_w.as<const float>();
    L.assign = b_asg.as<ps_u32>();
    L.ntrue = b_nt.as<ps_u8>();
    L.where = b_wh.as<ps_u32>();
    L.unsat = b_us.as<ps_u32>();
    L.walker = b_walk.as<PsWalker>();
    L.found = b_found.as<ps_u32>();
    L.flips_total = reinterpret_cast<ps_u64*>(b_found.as<unsigned char>() + 8);
    L.seed = opt.seed;
    L.num_vars = ps_u32(nv);
    L.num_clauses = nc;
    L.words = words;
    L.walkers = W;

    const unsigned sms = unsigned(std::max(1, d.sm_count()));
    auto init = [&](cuda::Kernel k, std::uint64_t total) {
        constexpr std::uint64_t chunk = 1ull << 26;
        for (std::uint64_t b = 0; b < total; b += chunk) {
            L.base = b;
            L.count = std::min(chunk, total - b);
            const unsigned grid = unsigned(std::min<std::uint64_t>((L.count + 255) / 256, sms * 32ull));
            if (!d.launch(k, grid, 256, L, s, &why) || !d.sync(s, &why)) return false;
        }
        return true;
    };
    if (!init(K.assign, std::uint64_t(words) * W) || !init(K.count, std::uint64_t(nc) * W)) return fail(why);
    res.setup_s = now_s() - t_setup;

    // Walk in short launches until a walker is done, or stop / timeout / max_flips.
    const std::uint64_t max_flips =
        opt.max_flips ? opt.max_flips : (opt.timeout_s > 0 || opt.stop) ? std::numeric_limits<std::uint64_t>::max()
                                                                         : 1000000ull;
    std::uint64_t done_flips = 0;
    double fpl = 256;  // flips per walker per launch, adapted to kLaunchTarget_s
    ps_u32 found = 0;
    const double t0 = now_s();
    const std::uint64_t threads = std::uint64_t(W) * (warp ? 32 : 1);
    const unsigned grid = unsigned((threads + kWalkBlock - 1) / kWalkBlock);
    const cuda::Kernel walk = warp ? K.walk_warp : K.walk;
    std::string stopped = "max_flips reached";
    while (done_flips < max_flips) {
        if (opt.stop && opt.stop->load(std::memory_order_relaxed)) {
            stopped = "stopped";
            break;
        }
        if (opt.timeout_s > 0 && now_s() - t0 > opt.timeout_s) {
            stopped = "timeout";
            break;
        }
        L.flips = ps_u32(std::min<std::uint64_t>(std::uint64_t(fpl), max_flips - done_flips));
        const double l0 = now_s();
        if (!d.launch(walk, grid, kWalkBlock, L, s, &why) || !d.sync(s, &why) ||
            !b_found.download(&found, 4, 0, &why))
            return fail(why);
        const double dt = now_s() - l0;
        done_flips += L.flips;
        if (found) break;
        fpl = std::clamp(fpl * (dt > 1e-6 ? kLaunchTarget_s / dt : 4.0), 32.0, double(1u << 20));
    }
    res.seconds = now_s() - t0;
    res.ran = true;
    ps_u64 flips = 0;
    b_found.download(&flips, 8, 8, &why);
    res.flips = flips;
    if (!found) {
        res.note += stopped + " after " + std::to_string(flips) + " flips: nothing found (not a proof of anything)";
        return res;
    }
    std::vector<ps_u32> bits(words);
    if (!b_asg.download(bits.data(), 4ull * words, 4ull * words * (found - 1), &why)) return fail(why);
    res.assignment.assign(nv + 1, 0);
    for (std::size_t v = 1; v <= nv; ++v) res.assignment[v] = std::int8_t((bits[v >> 5] >> (v & 31)) & 1u);
    // The kernel's claim is not trusted: every clause of the caller's CNF.
    if (!assignment_satisfies(cnf, res.assignment)) {
        res.assignment.clear();
        res.note += "walker " + std::to_string(found - 1) +
                    " claimed a model that fails the host re-check (kernel defect); discarded";
        return res;
    }
    res.found = true;
    res.note += "walker " + std::to_string(found - 1) + " of " + std::to_string(W) +
                " found a candidate model (host re-checked; Z3 validates)";
    return res;
}

std::optional<std::vector<std::int8_t>> gpu_probsat(const Cnf& cnf, std::uint64_t seed, std::uint64_t max_flips,
                                                    unsigned walkers, std::string* why) {
    GpuSlsOptions o;
    o.seed = seed;
    o.max_flips = max_flips;
    o.walkers = walkers;
    auto r = gpu_probsat(cnf, o);
    if (r.found) return std::move(r.assignment);
    if (why) *why = r.note;
    return std::nullopt;
}

bool cuda_probsat(const Cnf& cnf, std::uint64_t seed, std::uint64_t max_flips_per_walker, unsigned walkers,
                  std::vector<std::int8_t>& assignment) {
    auto a = gpu_probsat(cnf, seed, max_flips_per_walker, walkers);
    if (!a) return false;
    assignment = std::move(*a);
    return true;
}

}  // namespace prism::solver
