// PRISM conformance task cxx/cxx_ratio_true.cpp: expected true (no-overflow)
// Reads num and den of ratio<1,2>. Naming a ratio is not a proof.
#include <ratio>
int cxx_ratio_true() {
    return static_cast<int>(std::ratio<1, 2>::num + std::ratio<1, 2>::den);
}
