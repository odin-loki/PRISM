// run_tree (proctree.hpp) on prism::exec / FORGE: a new session per command,
// the whole tree killed on the timeout or on an interrupt of the scorer.
#include "proctree.hpp"

#include "prism/exec.hpp"

#include <forge/os/process.hpp>

#include <exception>

namespace prism::qa {

std::string which(const std::string& name) {
    if (name.empty()) return {};
    return forge::os::find_executable(name);
}

void install_interrupt_cleanup() noexcept { prism::exec::install_interrupt_cleanup(); }

RunResult run_tree(const std::vector<std::string>& argv, const RunOpts& opts) {
    RunResult r;
    if (argv.empty()) {
        r.start_failed = true;
        r.err = "no argv";
        return r;
    }
    prism::exec::Spec spec;
    spec.argv = argv;
    spec.cwd = opts.cwd.string();
    for (const auto& [k, v] : opts.env) spec.env_set[k] = v;
    spec.timeout_ms = opts.timeout_s > 0 ? static_cast<std::uint64_t>(opts.timeout_s * 1000) : 0;
    spec.memory_bytes = opts.mem_limit_mb > 0 ? static_cast<std::uint64_t>(opts.mem_limit_mb) << 20 : 0;
    spec.new_session = true;
    spec.max_output = ~0ull;
    prism::exec::Result o;
    try {
        o = prism::exec::run(spec);
    } catch (const std::exception& e) {  // Law 8: a --no-*-check flag is refused, and says so
        r.start_failed = true;
        r.err = std::string("refused: ") + e.what();
        return r;
    }
    r.out = std::move(o.out);
    r.err = std::move(o.err);
    r.rc = o.rc();
    r.timed_out = o.timed_out;
    if (o.failed) {
        r.start_failed = true;
        r.err = argv[0] + ": " + (r.err.empty() ? std::string("could not start") : r.err);
    }
    return r;
}

}  // namespace prism::qa
