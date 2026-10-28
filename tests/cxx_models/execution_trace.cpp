// Trace of std::execution::seq. `#include <execution>` is this model when
// its directory is first on the include path, and libstdc++ otherwise.
// seq is the sequenced policy tag. par, par_unseq, and unseq are not
// named: a parallel policy that ran on one thread would be a fake
// parallel execution. No algorithm is called. This header is not a
// parallelism proof.
#include <cstdio>
#include <execution>
#include <type_traits>

static_assert(std::is_same<decltype(std::execution::seq), const std::execution::sequenced_policy>::value);

int main() {
    constexpr std::execution::sequenced_policy policy = std::execution::seq;
    const int same =
        std::is_same<decltype(std::execution::seq), const std::execution::sequenced_policy>::value ? 1 : 0;
    const int copy = std::is_same<decltype(policy), const std::execution::sequenced_policy>::value ? 1 : 0;
    const int sz = static_cast<int>(sizeof(std::execution::sequenced_policy));
    std::printf("same %d\n", same);
    std::printf("copy %d\n", copy);
    std::printf("size %d\n", sz);
    if (!same || !copy) return 1;
    return 0;
}
