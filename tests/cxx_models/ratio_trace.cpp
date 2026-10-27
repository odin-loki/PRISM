// Trace of std::ratio. Compiled against libstdc++ or with PRISM's model
// header (src/prism/pir/models/cxx/ratio) first on the include path. Prints
// the reduced numerator and denominator. A zero denominator and a product
// that does not fit in intmax_t are ill-formed, so this program does not
// name them.
#include <ratio>
#include <cstdio>

static void show(const char* name, long long num, long long den) {
    std::printf("%s %lld/%lld\n", name, num, den);
}

int main() {
    show("r12", std::ratio<1, 2>::num, std::ratio<1, 2>::den);
    show("r24", std::ratio<2, 4>::num, std::ratio<2, 4>::den);
    show("neg", std::ratio<1, -2>::num, std::ratio<1, -2>::den);
    show("add", (std::ratio_add<std::ratio<1, 2>, std::ratio<1, 3>>::num),
         (std::ratio_add<std::ratio<1, 2>, std::ratio<1, 3>>::den));
    show("sub", (std::ratio_subtract<std::ratio<1, 2>, std::ratio<1, 3>>::num),
         (std::ratio_subtract<std::ratio<1, 2>, std::ratio<1, 3>>::den));
    show("mul", (std::ratio_multiply<std::ratio<1, 2>, std::ratio<2, 3>>::num),
         (std::ratio_multiply<std::ratio<1, 2>, std::ratio<2, 3>>::den));
    show("div", (std::ratio_divide<std::ratio<1, 2>, std::ratio<2, 3>>::num),
         (std::ratio_divide<std::ratio<1, 2>, std::ratio<2, 3>>::den));
    show("milli", std::milli::num, std::milli::den);
    show("kilo", std::kilo::num, std::kilo::den);
    return 0;
}
