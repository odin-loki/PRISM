#include "prism/laws.hpp"

#include <stdexcept>
#include <string>
#include <unordered_set>

namespace prism::laws {

namespace {

const std::unordered_set<std::string>& proofs() {
    static const std::unordered_set<std::string> s{
        std::string(PROVED_UNBOUNDED), std::string(PROVED),
        std::string(PROVED_ASSUMING)};
    return s;
}

const std::unordered_set<std::string>& answered() {
    static const std::unordered_set<std::string> s{
        std::string(PROVED_UNBOUNDED), std::string(PROVED),
        std::string(PROVED_ASSUMING), std::string(BOUNDED), std::string(FAILED)};
    return s;
}

}  // namespace

bool is_proof(std::string_view status) {
    return proofs().contains(std::string(status));
}

bool is_answered(std::string_view status) {
    return answered().contains(std::string(status));
}

void refuse_merge(std::string_view a, std::string_view b) {
    if (a == b) return;
    static const std::unordered_set<std::string> formal{
        std::string(PROVED_UNBOUNDED), std::string(PROVED),
        std::string(PROVED_ASSUMING), std::string(BOUNDED)};
    std::string as(a), bs(b);
    if (formal.contains(as) && formal.contains(bs)) {
        throw std::runtime_error("refusing to merge formal statuses " + as + " and " + bs);
    }
    if ((as == CLEAN || bs == CLEAN) && (formal.contains(as) || formal.contains(bs))) {
        throw std::runtime_error("refusing to promote fuzz " + as + "/" + bs + " to a proof");
    }
}

}  // namespace prism::laws
