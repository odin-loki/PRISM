// Trace of std::ratio. Compiled against libstdc++ or with PRISM's model
// header (src/prism/pir/models/cxx/ratio) first on the include path. Prints
// the reduced numerator and denominator, and comparisons whose cross
// products fit in intmax_t. A zero denominator and a product that does not
// fit in intmax_t are ill-formed, so this program does not name them. A
// comparison whose cross product does not fit is not a boolean here.
#include <ratio>
#include <cstdio>

static void show(const char* name, long long num, long long den) {
    std::printf("%s %lld/%lld\n", name, num, den);
}

static void flag(const char* name, bool v) { std::printf("%s %d\n", name, v ? 1 : 0); }

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
    flag("eq", std::ratio_equal<std::ratio<2, 4>, std::ratio<1, 2>>::value);
    flag("ne", std::ratio_equal<std::ratio<1, 2>, std::ratio<1, 3>>::value);
    flag("lt", std::ratio_less<std::ratio<1, 3>, std::ratio<1, 2>>::value);
    flag("lt_eq", std::ratio_less<std::ratio<1, 2>, std::ratio<2, 4>>::value);
    flag("lt_neg", std::ratio_less<std::ratio<-1, 2>, std::ratio<1, 3>>::value);
    flag("neq", std::ratio_not_equal<std::ratio<1, 2>, std::ratio<1, 3>>::value);
    flag("neq_same", std::ratio_not_equal<std::ratio<2, 4>, std::ratio<1, 2>>::value);
    flag("gt", std::ratio_greater<std::ratio<1, 2>, std::ratio<1, 3>>::value);
    flag("gt_eq", std::ratio_greater<std::ratio<1, 2>, std::ratio<2, 4>>::value);
    flag("gt_neg", std::ratio_greater<std::ratio<-1, 3>, std::ratio<-1, 2>>::value);
    flag("le", std::ratio_less_equal<std::ratio<1, 3>, std::ratio<1, 2>>::value);
    flag("le_eq", std::ratio_less_equal<std::ratio<2, 4>, std::ratio<1, 2>>::value);
    flag("ge", std::ratio_greater_equal<std::ratio<1, 2>, std::ratio<1, 3>>::value);
    flag("ge_lt", std::ratio_greater_equal<std::ratio<1, 3>, std::ratio<1, 2>>::value);
    return 0;
}
