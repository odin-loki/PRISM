// Trace of the partial <numbers> model (src/prism/pir/models/cxx/numbers).
// The constants are variable templates. The long double literals are not
// exact, and a comparison against them is not a proof of a numeric identity.
#include <numbers>

int main() {
    const auto pi = std::numbers::pi<long double>;
    const auto e = std::numbers::e<long double>;
    if (!(pi > 3 && pi < 4)) return 1;
    if (!(e > 2 && e < 3)) return 1;
    return 0;
}
