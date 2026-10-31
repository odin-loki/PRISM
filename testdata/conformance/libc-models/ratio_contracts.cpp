// std::ratio stand-in (src/prism/pir/models/cxx/ratio). Reads the reduced
// numerator and denominator of a small ratio. A zero denominator is
// ill-formed, so this harness does not name one.
#include <ratio>
#include <cassert>

int ratio_reduced_true() {
    assert((std::ratio<1, 2>::num == 1));
    assert((std::ratio<1, 2>::den == 2));
    assert((std::ratio<2, 4>::num == 1));
    assert((std::ratio<2, 4>::den == 2));
    return 0;
}
