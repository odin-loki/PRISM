// PRISM conformance task cxx/cxx_vector_at_false.cpp: expected false (no-uncaught)
// std::vector::at out of range in a noexcept function (std::terminate)
#include <vector>
#include <stdexcept>
int cxx_vector_at_false(int i) noexcept {
    std::vector<int> v(4, 1);
    return v.at(static_cast<std::size_t>(i & 7));  // out_of_range escapes noexcept
}
