// PRISM thread-interleaving scheduler shared by the <thread>, <mutex> and
// <condition_variable> models.
//
// Threads are explored as a bounded, non-preemptive schedule: a started
// thread is queued, not run. At every synchronisation point (mutex unlock,
// condition_variable wait / notify, thread join, this_thread::yield) the
// solver picks, through __prism_havoc_bytes, whether and which queued thread
// runs to its next completion. Every choice is a separate path, so the
// verifier sees each order in which threads can complete relative to the
// sync points of the thread that started them.
//
// Bounds (an under-approximation, recorded in coverage-limits.md): a thread
// body runs to completion once picked (no preemption inside a body between
// two of its own sync points), at most __prism_sched_max threads are queued,
// and nesting is limited to __prism_sched_depth. A path past a bound reaches
// __prism_incomplete (NEEDS-HARNESS), never PROVED.
//
// Checks: CXX-DEADLOCK (a lock or wait that no remaining schedule can
// satisfy), CXX-THREAD-JOINABLE (std::thread destroyed or assigned while
// joinable), CXX-MUTEX-UNLOCK (see <mutex>).
#ifndef _PRISM_SCHED_H
#define _PRISM_SCHED_H 1

#pragma GCC system_header

extern "C" void __prism_check(int __ok, const char* __cls, const char* __msg);
extern "C" void __prism_havoc_bytes(void* __p, unsigned long __n);
extern "C" void __prism_incomplete(const char*);

namespace std {
namespace __prism_sched {

struct __task {
    void (*_M_run)(void*);
    void (*_M_del)(void*);
    void* _M_fn;
    bool _M_done;
};

inline constexpr unsigned __prism_sched_max = 8;
inline constexpr unsigned __prism_sched_depth = 4;

inline __task* __q[__prism_sched_max];
inline unsigned __n = 0;
inline unsigned __depth = 0;

inline unsigned __pick(unsigned __bound) {
    unsigned char __b = 0;
    __prism_havoc_bytes(&__b, 1);
    return static_cast<unsigned>(__b) % __bound;
}

inline void __enqueue(__task* __t) {
    if (__n == __prism_sched_max) {
        __prism_incomplete("more threads than the PRISM scheduler bound");
        __builtin_trap();
    }
    __q[__n++] = __t;
}

inline void __remove(unsigned __i) {
    for (unsigned __j = __i; __j + 1 < __n; ++__j) __q[__j] = __q[__j + 1];
    --__n;
}

inline void __run(unsigned __i) {
    __task* __t = __q[__i];
    __remove(__i);
    if (__depth == __prism_sched_depth) {
        __prism_incomplete("thread nesting past the PRISM scheduler bound");
        __builtin_trap();
    }
    ++__depth;
    __t->_M_run(__t->_M_fn);
    --__depth;
    __t->_M_done = true;
}

// Run the given task now if still queued (join).
inline void __run_task(__task* __t) {
    for (unsigned __i = 0; __i < __n; ++__i)
        if (__q[__i] == __t) { __run(__i); return; }
}

// A sync point: optionally run one queued thread (each choice is a path).
inline void __maybe_switch() {
    if (__n == 0) return;
    const unsigned __c = __pick(__n + 1);
    if (__c < __n) __run(__c);
}

// The current thread is blocked: some queued thread must run, or no
// schedule can unblock it.
inline void __must_switch(const char* __why) {
    if (__n == 0) {
        __prism_check(0, "CXX-DEADLOCK", __why);
        __builtin_trap();
    }
    __run(__pick(__n));
}

}  // namespace __prism_sched
}  // namespace std

#endif  // _PRISM_SCHED_H
