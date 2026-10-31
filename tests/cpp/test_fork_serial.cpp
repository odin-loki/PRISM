// Regression: pir over several units with --jobs 2 spun forever. Two workers
// spawning clang at the same moment both ran Z3's pthread_atfork prepare
// handler (scoped_timer::finalize) and livelocked on its timer-pool locks.
// detail::serialized_fork() lets one thread fork at a time; this hammers it
// from several threads while others run Z3 checks with timeouts (which start
// scoped_timer workers), and fails instead of hanging if it livelocks.
#include <doctest/doctest.h>

#include "../../src/prism/proc.hpp"

#include <z3++.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#if !defined(_WIN32)
TEST_CASE("proc: concurrent forks with live Z3 timers do not livelock") {
    std::atomic<bool> stop{false};
    std::atomic<int> runs{0}, fails{0};
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        for (int i = 0; i < 1200 && !done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!done.load()) {
            std::fprintf(stderr, "proc: concurrent fork livelock (serialized_fork regression)\n");
            std::_Exit(3);
        }
    });
    std::vector<std::thread> ts;
    for (int t = 0; t < 2; ++t)
        ts.emplace_back([&] {
            while (!stop.load()) {
                z3::context c;
                z3::solver s(c);
                z3::params p(c);
                p.set("timeout", 20u);
                s.set(p);
                auto x = c.bv_const("x", 64), y = c.bv_const("y", 64);
                auto n = c.bv_val(static_cast<uint64_t>(0x123456789ULL), 64);
                s.add(x * y == n && x != 1 && y != 1 && x != n);
                (void)s.check();
            }
        });
    for (int t = 0; t < 3; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < 60; ++i) {
                prism::detail::RunSpec sp;
                sp.argv = {"/bin/true"};
                auto r = prism::detail::run(sp);
                ++runs;
                if (r.rc != 0) ++fails;
            }
        });
    for (std::size_t i = 2; i < ts.size(); ++i) ts[i].join();
    stop = true;
    ts[0].join();
    ts[1].join();
    done = true;
    watchdog.join();
    CHECK(runs.load() == 180);
    CHECK(fails.load() == 0);
}
#endif
