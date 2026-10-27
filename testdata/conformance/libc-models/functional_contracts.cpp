// std::function operational model (src/prism/pir/models/cxx/functional) against
// a function-pointer target. The pir stage puts the model headers first on
// the include path, so <functional> below is the model. Only std::function is
// used. An empty operator() is the bad_function_call precondition.
#include <functional>

static int stored(int x) { return x; }

int functional_call_true(int x) {
    std::function<int(int)> f = stored;
    return f(x);
}

int functional_empty_false(int x) {
    std::function<int(int)> f;
    if (x) return x;
    return f(x);
}
