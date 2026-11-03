// PRISM thread-interleaving scheduler shared by the <thread>, <mutex> and
// <condition_variable> models.
//
// Threads are explored as bounded schedules. A started thread is queued; the
// solver may also start it at once (eager start, chosen at construction).
// At every synchronisation point of the running thread (mutex unlock,
// condition_variable wait / notify, every std::atomic operation, thread join,
// this_thread::yield / sleep) the solver picks, through __prism_havoc_bytes,
// whether and which queued thread runs next: the running thread is then
// preempted partway through its body at that point and resumes when the
// other thread finishes or blocks. Every choice is a separate path.
//
// Bounds (an under-approximation, recorded in coverage-limits.md):
// - at most PRISM_SCHED_PREEMPTIONS (default 2) optional switches per path
//   (CHESS-style preemption bound); switches forced by a blocked thread do
//   not count;
// - a thread that was switched to runs on the stack of the thread it
//   preempted, so it can itself be preempted (nested, depth
//   __prism_sched_depth), but the preempted thread cannot run again until it
//   finishes or blocks. When it blocks while some preempted thread could
//   still continue, that schedule is not representable and the path is
//   dropped (__prism_assume(0)); the schedules without that switch are
//   still explored;
// - at most __prism_sched_max queued threads.
// A path past a size bound reaches __prism_incomplete (NEEDS-HARNESS).
// Memory order is sequentially consistent: every atomic is a switch point,
// and weaker orders add no extra behaviour in this model.
//
// Checks: CXX-DEADLOCK (mapped to CONC-DEADLOCK in translate.cpp: every
// thread on the stack is blocked and none is queued), CXX-THREAD-JOINABLE
// (mapped to CXX-TERMINATE), CXX-MUTEX-UNLOCK (see <mutex>).
#ifndef _PRISM_SCHED_H
#define _PRISM_SCHED_H 1

#pragma GCC system_header

extern "C" void __prism_check(int __ok, const char* __cls, const char* __msg);
extern "C" void __prism_havoc_bytes(void* __p, unsigned long __n);
extern "C" void __prism_incomplete(const char*);
extern "C" void __prism_assume(int __cond);

#ifndef PRISM_SCHED_PREEMPTIONS
#define PRISM_SCHED_PREEMPTIONS 2
#endif

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
inline unsigned __budget = PRISM_SCHED_PREEMPTIONS;
// __blk[d]: the thread at stack depth d is blocked (true) or was preempted
// and could continue (false).
inline bool __blk[__prism_sched_depth + 1];

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

inline void __run(unsigned __i, bool __caller_blocked = true) {
    __task* __t = __q[__i];
    __remove(__i);
    if (__depth == __prism_sched_depth) {
        __prism_incomplete("thread nesting past the PRISM scheduler bound");
        __builtin_trap();
    }
    __blk[__depth] = __caller_blocked;
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

// A sync point: optionally preempt the running thread for one queued
// thread (each choice is a path), within the preemption budget.
inline void __maybe_switch() {
    if (__n == 0 || __budget == 0) return;
    const unsigned __c = __pick(__n + 1);
    if (__c < __n) {
        --__budget;
        __run(__c, false);
    }
}

// Thread construction: optionally start the new (last queued) thread now.
inline void __maybe_start() {
    if (__n == 0 || __budget == 0) return;
    if (__pick(2) == 1) {
        --__budget;
        __run(__n - 1, false);
    }
}

// The current thread is blocked: some queued thread must run, or no
// schedule can unblock it.
inline void __must_switch(const char* __why) {
    if (__n == 0) {
        for (unsigned __d = 0; __d < __depth; ++__d)
            if (!__blk[__d]) {
                // a preempted thread below could continue: not representable
                __prism_assume(0);
                __builtin_trap();
            }
        __prism_check(0, "CXX-DEADLOCK", __why);
        __builtin_trap();
    }
    __run(__pick(__n), true);
}

}  // namespace __prism_sched
}  // namespace std

#endif  // _PRISM_SCHED_H
