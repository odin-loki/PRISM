// PRISM conformance task coro/coro_gen_false.cpp: expected false (no-div0)
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
static Gen counter(int start, int step) {
    for (int i = 0;; i += step) co_yield start + i;
}
int coro_gen_false(int x) {
    if (x > 1000 || x < -1000) return 0;
    Gen g = counter(x, 1);
    int a = g.next();
    int b = g.next();
    return 100 / (b - a - 1);
}
