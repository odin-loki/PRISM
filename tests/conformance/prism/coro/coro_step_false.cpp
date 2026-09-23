// PRISM conformance task coro/coro_step_false.cpp: expected false (no-div0)
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
    co_yield start;
    co_yield start + step;
}
int coro_step_false(int s) {
    if (s < 0 || s > 100) return 0;
    Gen g = counter(0, s);
    g.next();
    return 100 / g.next(); /* s == 0 */
}
