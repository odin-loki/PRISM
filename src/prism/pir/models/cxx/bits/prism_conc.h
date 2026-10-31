// PRISM: <thread>, <mutex>, <condition_variable>, and <atomic> for the conc
// stage (src/prism/conc, docs/CONCURRENCY.md). The conc stage lowers C++
// units with -D__PRISM_CONC_THREADS=1, and these models then map straight
// onto the pthread calls and seq_cst atomics its lazy sequentialisation
// encodes. That encoder keeps a saved program counter per thread and lets
// every thread be preempted and resumed at every visible operation, up to
// its round bound: the bounded search is complete within that bound (no
// nested-stack under-approximation, unlike bits/prism_sched.h, which the pir
// stage keeps using).
//
// Shape rules (so the IR matches what src/prism/conc/extract.cpp accepts):
//  * every member function is always_inline, so after mem2reg/sroa the
//    pthread calls take the user's objects directly;
//  * std::thread is exactly one pthread_t, and the handle is the object
//    address (no member GEP); std::mutex is exactly one pthread_mutex_t and
//    std::condition_variable one pthread_cond_t, constant-initialised, so a
//    global mutex is the global itself;
//  * a stateless callable with no arguments runs through _S_entry<F>, which
//    takes no thread argument. Anything else passes the decayed closure as
//    the pthread argument, which the conc encoder reports as UNENCODED
//    (thread arguments need the PIR memory model): NEEDS-HARNESS, never a
//    verdict;
//  * atomics are __atomic builtins with __ATOMIC_SEQ_CST (atomicrmw /
//    cmpxchg / atomic load-store on the object), whatever order the caller
//    names. The conc stage separately refuses sources that name a weaker
//    order.
// Not modelled here (the units fall back to UNENCODED or fail to compile):
// timed waits, try_lock_for, recursive/timed mutexes, jthread stop tokens,
// atomic wait/notify.
#ifndef _PRISM_CONC_THREADS_H
#define _PRISM_CONC_THREADS_H 1

#pragma GCC system_header

#include <pthread.h>
#include <type_traits>
#include <utility>
#include <cstddef>

extern "C" void __prism_check(int __ok, const char* __cls, const char* __msg);

#define _PRISM_AI __attribute__((__always_inline__))

namespace std {

// ---- <mutex>
class mutex {
    pthread_mutex_t _M_m = PTHREAD_MUTEX_INITIALIZER;

public:
    constexpr mutex() noexcept = default;
    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;
    _PRISM_AI pthread_mutex_t* native_handle() noexcept { return reinterpret_cast<pthread_mutex_t*>(this); }
    _PRISM_AI void lock() { pthread_mutex_lock(native_handle()); }
    _PRISM_AI bool try_lock() noexcept { return pthread_mutex_trylock(native_handle()) == 0; }
    _PRISM_AI void unlock() noexcept { pthread_mutex_unlock(native_handle()); }
};

struct defer_lock_t { explicit defer_lock_t() = default; };
struct try_to_lock_t { explicit try_to_lock_t() = default; };
struct adopt_lock_t { explicit adopt_lock_t() = default; };
inline constexpr defer_lock_t defer_lock{};
inline constexpr try_to_lock_t try_to_lock{};
inline constexpr adopt_lock_t adopt_lock{};

template <class _M>
class lock_guard {
    _M& _M_m;

public:
    using mutex_type = _M;
    _PRISM_AI explicit lock_guard(_M& __m) : _M_m(__m) { _M_m.lock(); }
    _PRISM_AI lock_guard(_M& __m, adopt_lock_t) noexcept : _M_m(__m) {}
    _PRISM_AI ~lock_guard() { _M_m.unlock(); }
    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;
};

template <class... _Ms>
class scoped_lock;
template <class _M>
class scoped_lock<_M> {
    _M& _M_m;

public:
    _PRISM_AI explicit scoped_lock(_M& __m) : _M_m(__m) { _M_m.lock(); }
    _PRISM_AI ~scoped_lock() { _M_m.unlock(); }
    scoped_lock(const scoped_lock&) = delete;
    scoped_lock& operator=(const scoped_lock&) = delete;
};

template <class _M>
class unique_lock {
    _M* _M_m = nullptr;
    bool _M_own = false;

public:
    using mutex_type = _M;
    unique_lock() noexcept = default;
    _PRISM_AI explicit unique_lock(_M& __m) : _M_m(&__m) { lock(); }
    _PRISM_AI unique_lock(_M& __m, defer_lock_t) noexcept : _M_m(&__m) {}
    _PRISM_AI unique_lock(_M& __m, try_to_lock_t) : _M_m(&__m), _M_own(__m.try_lock()) {}
    _PRISM_AI unique_lock(_M& __m, adopt_lock_t) noexcept : _M_m(&__m), _M_own(true) {}
    unique_lock(const unique_lock&) = delete;
    unique_lock& operator=(const unique_lock&) = delete;
    _PRISM_AI ~unique_lock() {
        if (_M_own) _M_m->unlock();
    }
    _PRISM_AI void lock() {
        __prism_check(_M_m != nullptr && !_M_own ? 1 : 0, "FUNC-CONTRACT", "unique_lock::lock without a mutex or already owned");
        _M_m->lock();
        _M_own = true;
    }
    _PRISM_AI bool try_lock() { return _M_own = _M_m->try_lock(); }
    _PRISM_AI void unlock() {
        __prism_check(_M_own ? 1 : 0, "FUNC-CONTRACT", "unique_lock::unlock without ownership");
        _M_own = false;
        _M_m->unlock();
    }
    _PRISM_AI bool owns_lock() const noexcept { return _M_own; }
    _PRISM_AI explicit operator bool() const noexcept { return _M_own; }
    _PRISM_AI _M* mutex() const noexcept { return _M_m; }
};

// ---- <condition_variable>
enum class cv_status { no_timeout, timeout };

class condition_variable {
    pthread_cond_t _M_c = PTHREAD_COND_INITIALIZER;

public:
    constexpr condition_variable() noexcept = default;
    condition_variable(const condition_variable&) = delete;
    condition_variable& operator=(const condition_variable&) = delete;
    _PRISM_AI void notify_one() noexcept { pthread_cond_signal(reinterpret_cast<pthread_cond_t*>(this)); }
    _PRISM_AI void notify_all() noexcept { pthread_cond_broadcast(reinterpret_cast<pthread_cond_t*>(this)); }
    // The encoder treats a wait as unlock; <switch point>; lock (POSIX allows
    // spurious wake-ups, so that covers every wait exactly).
    _PRISM_AI void wait(unique_lock<mutex>& __l) {
        pthread_cond_wait(reinterpret_cast<pthread_cond_t*>(this), __l.mutex()->native_handle());
    }
    template <class _Pred>
    _PRISM_AI void wait(unique_lock<mutex>& __l, _Pred __p) {
        while (!static_cast<bool>(__p())) wait(__l);
    }
};

// ---- <thread>
class thread {
    pthread_t _M_h;  // no initializer: pthread_create writes it (a store would hide the handle)
    bool _M_j = false;  // joinable(): set by the launching constructor, cleared by join()

    template <class _Fd>
    static void* _S_entry(void*) {
        _Fd __f{};
        __f();
        return nullptr;
    }
    template <class _L>
    static void* _S_entry_arg(void* __p) {
        (*static_cast<_L*>(__p))();
        delete static_cast<_L*>(__p);
        return nullptr;
    }
    _PRISM_AI pthread_t* _M_handle() noexcept { return reinterpret_cast<pthread_t*>(this); }

public:
    class id {
        pthread_t _M_i = 0;
        friend class thread;

    public:
        id() noexcept = default;
        explicit id(pthread_t __i) noexcept : _M_i(__i) {}
        friend bool operator==(id __a, id __b) noexcept { return __a._M_i == __b._M_i; }
    };

    thread() noexcept = default;

    template <class _Fn, class... _Args, class = enable_if_t<!is_same_v<remove_cvref_t<_Fn>, thread>>>
    _PRISM_AI explicit thread(_Fn&& __f, _Args&&... __a) {
        using _Fd = decay_t<_Fn>;
        if constexpr (sizeof...(_Args) == 0 && is_empty_v<_Fd> && is_default_constructible_v<_Fd>) {
            pthread_create(_M_handle(), nullptr, &_S_entry<_Fd>, nullptr);
            _M_j = true;
        } else {
            auto* __fn = new auto([__g = _Fd(std::forward<_Fn>(__f)), ... __b = decay_t<_Args>(std::forward<_Args>(__a))]() mutable {
                std::move(__g)(std::move(__b)...);
            });
            using _L = remove_pointer_t<decltype(__fn)>;
            pthread_create(_M_handle(), nullptr, &_S_entry_arg<_L>, __fn);
            _M_j = true;
        }
    }
    thread(const thread&) = delete;
    thread& operator=(const thread&) = delete;

    // join() on a non-joinable thread throws std::system_error(invalid_argument)
    // in libstdc++/libc++; modelled as a trap so the verdict reports it.
    _PRISM_AI void join() {
        if (!_M_j) __builtin_trap();
        pthread_join(*_M_handle(), nullptr);
        _M_j = false;
    }
    _PRISM_AI bool joinable() const noexcept { return _M_j; }
    static unsigned hardware_concurrency() noexcept { return 2; }
};

namespace this_thread {
_PRISM_AI inline void yield() noexcept { sched_yield(); }
}  // namespace this_thread

// ---- <atomic>
enum class memory_order : int { relaxed, consume, acquire, release, acq_rel, seq_cst };
inline constexpr memory_order memory_order_relaxed = memory_order::relaxed;
inline constexpr memory_order memory_order_consume = memory_order::consume;
inline constexpr memory_order memory_order_acquire = memory_order::acquire;
inline constexpr memory_order memory_order_release = memory_order::release;
inline constexpr memory_order memory_order_acq_rel = memory_order::acq_rel;
inline constexpr memory_order memory_order_seq_cst = memory_order::seq_cst;
_PRISM_AI inline void atomic_thread_fence(memory_order) noexcept { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
_PRISM_AI inline void atomic_signal_fence(memory_order) noexcept {}

template <class _Tp>
struct atomic {
    static_assert(is_integral_v<_Tp> || is_pointer_v<_Tp>, "PRISM conc model: integral or pointer atomics only");
    using value_type = _Tp;
    _Tp _M_v;

    constexpr atomic() noexcept : _M_v() {}
    constexpr atomic(_Tp __v) noexcept : _M_v(__v) {}
    atomic(const atomic&) = delete;
    atomic& operator=(const atomic&) = delete;

    _PRISM_AI _Tp load(memory_order = memory_order_seq_cst) const noexcept { return __atomic_load_n(&_M_v, __ATOMIC_SEQ_CST); }
    _PRISM_AI void store(_Tp __v, memory_order = memory_order_seq_cst) noexcept { __atomic_store_n(&_M_v, __v, __ATOMIC_SEQ_CST); }
    _PRISM_AI operator _Tp() const noexcept { return load(); }
    _PRISM_AI _Tp operator=(_Tp __v) noexcept { store(__v); return __v; }
    _PRISM_AI _Tp exchange(_Tp __v, memory_order = memory_order_seq_cst) noexcept { return __atomic_exchange_n(&_M_v, __v, __ATOMIC_SEQ_CST); }
    _PRISM_AI bool compare_exchange_strong(_Tp& __e, _Tp __d, memory_order = memory_order_seq_cst, memory_order = memory_order_seq_cst) noexcept {
        return __atomic_compare_exchange_n(&_M_v, &__e, __d, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    // No spurious failure here (strong): the pir-stage model covers that.
    _PRISM_AI bool compare_exchange_weak(_Tp& __e, _Tp __d, memory_order __o = memory_order_seq_cst, memory_order __f = memory_order_seq_cst) noexcept {
        return compare_exchange_strong(__e, __d, __o, __f);
    }
    template <class _U = _Tp, class = enable_if_t<is_integral_v<_U>>>
    _PRISM_AI _Tp fetch_add(_Tp __v, memory_order = memory_order_seq_cst) noexcept { return __atomic_fetch_add(&_M_v, __v, __ATOMIC_SEQ_CST); }
    template <class _U = _Tp, class = enable_if_t<is_integral_v<_U>>>
    _PRISM_AI _Tp fetch_sub(_Tp __v, memory_order = memory_order_seq_cst) noexcept { return __atomic_fetch_sub(&_M_v, __v, __ATOMIC_SEQ_CST); }
    template <class _U = _Tp, class = enable_if_t<is_integral_v<_U>>>
    _PRISM_AI _Tp fetch_and(_Tp __v, memory_order = memory_order_seq_cst) noexcept { return __atomic_fetch_and(&_M_v, __v, __ATOMIC_SEQ_CST); }
    template <class _U = _Tp, class = enable_if_t<is_integral_v<_U>>>
    _PRISM_AI _Tp fetch_or(_Tp __v, memory_order = memory_order_seq_cst) noexcept { return __atomic_fetch_or(&_M_v, __v, __ATOMIC_SEQ_CST); }
    template <class _U = _Tp, class = enable_if_t<is_integral_v<_U>>>
    _PRISM_AI _Tp fetch_xor(_Tp __v, memory_order = memory_order_seq_cst) noexcept { return __atomic_fetch_xor(&_M_v, __v, __ATOMIC_SEQ_CST); }
    template <class _U = _Tp, class = enable_if_t<is_integral_v<_U>>>
    _PRISM_AI _Tp operator++() noexcept { return fetch_add(1) + 1; }
    template <class _U = _Tp, class = enable_if_t<is_integral_v<_U>>>
    _PRISM_AI _Tp operator++(int) noexcept { return fetch_add(1); }
    template <class _U = _Tp, class = enable_if_t<is_integral_v<_U>>>
    _PRISM_AI _Tp operator--() noexcept { return fetch_sub(1) - 1; }
    template <class _U = _Tp, class = enable_if_t<is_integral_v<_U>>>
    _PRISM_AI _Tp operator--(int) noexcept { return fetch_sub(1); }
    template <class _U = _Tp, class = enable_if_t<is_integral_v<_U>>>
    _PRISM_AI _Tp operator+=(_Tp __v) noexcept { return fetch_add(__v) + __v; }
    template <class _U = _Tp, class = enable_if_t<is_integral_v<_U>>>
    _PRISM_AI _Tp operator-=(_Tp __v) noexcept { return fetch_sub(__v) - __v; }
    static constexpr bool is_always_lock_free = true;
    bool is_lock_free() const noexcept { return true; }
};

struct atomic_flag {
    bool _M_f = false;
    constexpr atomic_flag() noexcept = default;
    _PRISM_AI bool test_and_set(memory_order = memory_order_seq_cst) noexcept { return __atomic_exchange_n(&_M_f, true, __ATOMIC_SEQ_CST); }
    _PRISM_AI void clear(memory_order = memory_order_seq_cst) noexcept { __atomic_store_n(&_M_f, false, __ATOMIC_SEQ_CST); }
    _PRISM_AI bool test(memory_order = memory_order_seq_cst) const noexcept { return __atomic_load_n(&_M_f, __ATOMIC_SEQ_CST); }
};
#define ATOMIC_FLAG_INIT {}

using atomic_bool = atomic<bool>;
using atomic_char = atomic<char>;
using atomic_int = atomic<int>;
using atomic_uint = atomic<unsigned>;
using atomic_long = atomic<long>;
using atomic_ulong = atomic<unsigned long>;
using atomic_llong = atomic<long long>;
using atomic_ullong = atomic<unsigned long long>;
using atomic_size_t = atomic<size_t>;
using atomic_ptrdiff_t = atomic<ptrdiff_t>;

}  // namespace std

#undef _PRISM_AI

#endif  // _PRISM_CONC_THREADS_H
