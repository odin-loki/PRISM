// Status strings in include/prism/laws.hpp. is_proof is the four PROVED*
// verdicts. BOUNDED, CLEAN, NOTRUN, HYPOTHESIS, FAILED, TIMEOUT, UNKNOWN,
// and NEEDS-HARNESS are not proofs. Those constants stay different strings.

#include <doctest/doctest.h>

#ifdef ERROR
#  undef ERROR
#endif

#include "prism/laws.hpp"

#include <string>

TEST_CASE("laws status: the four PROVED strings are proofs") {
    using namespace prism::laws;

    CHECK(is_proof(PROVED));
    CHECK(is_proof(PROVED_UNBOUNDED));
    CHECK(is_proof(PROVED_CERTIFIED));
    CHECK(is_proof(PROVED_ASSUMING));

    CHECK_FALSE(is_proof(BOUNDED));
    CHECK_FALSE(is_proof(CLEAN));
    CHECK_FALSE(is_proof(NOTRUN));
    CHECK_FALSE(is_proof(HYPOTHESIS));
    CHECK_FALSE(is_proof(FAILED));
    CHECK_FALSE(is_proof(TIMEOUT));
    CHECK_FALSE(is_proof(UNKNOWN));
    CHECK_FALSE(is_proof(NEEDS_HARNESS));

    CHECK(std::string(PROVED) != std::string(BOUNDED));
    CHECK(std::string(CLEAN) != std::string(PROVED));
    CHECK(std::string(NOTRUN) != std::string(CLEAN));
    CHECK(std::string(HYPOTHESIS) != std::string(PROVED));
}
