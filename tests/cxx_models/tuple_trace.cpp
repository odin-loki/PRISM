// Differential trace of std::tuple (tests/cpp/test_cxx_tuple.cpp): compiled
// once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/tuple) first on the include path. Only arities
// 0, 1, and 2, and only an index that names a stored element.
#include <cstdio>
#include <tuple>
#include <type_traits>

static_assert(std::tuple_size<std::tuple<>>::value == 0);
static_assert(std::tuple_size<std::tuple<int>>::value == 1);
static_assert(std::tuple_size<std::tuple<int, int>>::value == 2);
static_assert(std::is_same<std::tuple_element<0, std::tuple<int>>::type, int>::value);
static_assert(std::is_same<std::tuple_element<0, std::tuple<int, int>>::type, int>::value);
static_assert(std::is_same<std::tuple_element<1, std::tuple<int, int>>::type, int>::value);

int main() {
    const int stored = 42;
    std::tuple<int> one(stored);
    std::printf("get0=%d\n", std::get<0>(one));
    std::get<0>(one) = 5;
    std::printf("set0=%d\n", std::get<0>(one));

    const std::tuple<int> cone(7);
    std::printf("cget0=%d\n", std::get<0>(cone));
    std::printf("rget0=%d\n", std::get<0>(std::tuple<int>(stored)));

    std::tuple<int, int> two(3, 9);
    std::printf("get0b=%d\n", std::get<0>(two));
    std::printf("get1=%d\n", std::get<1>(two));
    auto [x, y] = two;
    std::printf("bind=%d,%d\n", x, y);

    std::tuple<> empty;
    (void)empty;
    std::printf("empty=%zu\n", std::tuple_size<std::tuple<>>::value);
    std::printf("size1=%zu\n", std::tuple_size<std::tuple<int>>::value);
    std::printf("size2=%zu\n", std::tuple_size<std::tuple<int, int>>::value);

    std::tuple<int> zero;
    std::printf("zget=%d\n", std::get<0>(zero));
    return std::get<0>(std::tuple<int>(stored)) == stored ? 0 : 1;
}
