// PIR tasks: C++20 coroutines (docs/PIR.md "Coroutines"). LLVM's coroutine
// passes (coro-early, coro-split, coro-cleanup) lower each coroutine to a
// ramp function plus resume/destroy functions over a heap frame; PRISM then
// encodes them like any other code (the resume call is an indirect call
// through the frame, the frame a `new` object).
#include <coroutine>

struct Gen {
    struct promise_type {
        int value = 0;
        Gen get_return_object() { return Gen{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(int v) noexcept {
            value = v;
            return {};
        }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {}
    };
    std::coroutine_handle<promise_type> h;
    ~Gen() {
        if (h) h.destroy();
    }
    int next() {
        h.resume();
        return h.promise().value;
    }
};

static Gen counter(int start) {
    for (int i = 0;; ++i) co_yield start + i;
}

int coro_ok(int x) {
    if (x > 1000 || x < -1000) return 0;
    Gen g = counter(x);
    int a = g.next();
    int b = g.next();
    return 100 / (b - a);  // b == a + 1
}

int coro_bad(int x) {
    if (x > 1000 || x < -1000) return 0;
    Gen g = counter(x);
    int a = g.next();
    int b = g.next();
    return 100 / (b - a - 1);  // always a division by zero
}
