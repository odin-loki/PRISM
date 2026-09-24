// PRISM conformance task cxx/cxx_vector_at_true.cpp: expected true (no-uncaught)
// std::vector::at out of range in a noexcept function (std::terminate) (guarded)
#include <vector>
#include <stdexcept>
int cxx_vector_at_true(int i) noexcept {
    std::vector<int> v(4, 1);
    try {
        return v.at(static_cast<std::size_t>(i & 7));
    } catch (const std::out_of_range &) {
        return -1;
    }
}
