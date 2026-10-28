// The <random> model is the file src/prism/pir/models/cxx/random. This
// translation unit includes that file and also reads it as text. The first
// minstd_rand output after seed(1) is 48271. The first minstd_rand0 output
// after seed(1) is 16807. The header text says random_device is not a proof.
// minstd_rand0 is declared. mt19937, ranlux, knuth_b, and
// default_random_engine stay in the omitted-engines list.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#undef _GLIBCXX_RANDOM
#include "../../src/prism/pir/models/cxx/random"

namespace fs = std::filesystem;

TEST_CASE("cxx random model: seed 1 is 48271 and random_device is not a proof") {
    const fs::path header = fs::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" / "cxx" / "random";
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    std::ifstream in(header, std::ios::binary);
    REQUIRE(in);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    CHECK(text.find("random_device is not a proof") != std::string::npos);
    CHECK(text.find("not a measure of entropy") != std::string::npos);
    CHECK(text.find("not PROVED") != std::string::npos);
    CHECK(text.find("not CLEAN") != std::string::npos);
    CHECK(text.find("#ifndef _GLIBCXX_RANDOM") != std::string::npos);
    CHECK(text.find("#pragma GCC system_header") != std::string::npos);
    CHECK(text.find("namespace std") != std::string::npos);
    CHECK(text.find("#include <vector>") == std::string::npos);
    CHECK(text.find("#include <functional>") == std::string::npos);
    CHECK(text.find("#include <memory>") == std::string::npos);
    CHECK(text.find("return 0") == std::string::npos);
    CHECK(text.find("class minstd_rand0") != std::string::npos);
    CHECK(text.find("uniform_int_distribution") != std::string::npos);
    CHECK(text.find("shuffle") != std::string::npos);

    const auto omitted = text.find("the other engines (");
    REQUIRE(omitted != std::string::npos);
    const auto omitted_end = text.find(')', omitted);
    REQUIRE(omitted_end != std::string::npos);
    const std::string engines = text.substr(omitted, omitted_end - omitted);
    CHECK(engines.find("minstd_rand0") == std::string::npos);
    CHECK(engines.find("mt19937") != std::string::npos);
    CHECK(engines.find("ranlux") != std::string::npos);
    CHECK(engines.find("knuth_b") != std::string::npos);
    CHECK(engines.find("default_random_engine") != std::string::npos);

    static_assert(std::minstd_rand::multiplier == 48271);
    static_assert(std::minstd_rand::modulus == 2147483647);
    static_assert(std::minstd_rand::default_seed == 1);

    std::minstd_rand eng;
    eng.seed(1);
    CHECK(eng() == static_cast<std::minstd_rand::result_type>(48271));

    std::minstd_rand again;
    again.seed(1);
    (void)again();
    CHECK(again() == static_cast<std::minstd_rand::result_type>(182605794));

    std::minstd_rand a;
    a.seed(1);
    std::minstd_rand twin;
    twin.seed(1);
    a.discard(0);
    CHECK(a() == twin());
}

TEST_CASE("cxx random model: minstd_rand0 seed 1 is 16807") {
    static_assert(std::minstd_rand0::multiplier == 16807);
    static_assert(std::minstd_rand0::increment == 0);
    static_assert(std::minstd_rand0::modulus == 2147483647);
    static_assert(std::minstd_rand0::default_seed == 1);
    static_assert(std::minstd_rand0::min() == 1);
    static_assert(std::minstd_rand0::max() == std::minstd_rand0::modulus - 1);

    std::minstd_rand0 eng;
    eng.seed(1);
    CHECK(eng() == static_cast<std::minstd_rand0::result_type>(16807));

    std::minstd_rand0 again;
    again.seed(1);
    (void)again();
    CHECK(again() == static_cast<std::minstd_rand0::result_type>(282475249));

    std::minstd_rand0 a;
    a.seed(1);
    std::minstd_rand0 twin;
    twin.seed(1);
    a.discard(0);
    const auto first = a();
    CHECK(first == static_cast<std::minstd_rand0::result_type>(16807));
    CHECK(first == twin());

    std::minstd_rand0 skipped;
    skipped.seed(1);
    skipped.discard(1);
    CHECK(skipped() == static_cast<std::minstd_rand0::result_type>(282475249));

    std::minstd_rand0 zero;
    zero.seed(0);
    CHECK(zero() == static_cast<std::minstd_rand0::result_type>(16807));

    std::minstd_rand0 wrapped;
    wrapped.seed(std::minstd_rand0::modulus);
    CHECK(wrapped() == static_cast<std::minstd_rand0::result_type>(16807));

    const auto two_mod = static_cast<std::minstd_rand0::result_type>(
        static_cast<unsigned long long>(std::minstd_rand0::modulus) * 2ull);
    std::minstd_rand0 twice;
    twice.seed(two_mod);
    CHECK(twice() == static_cast<std::minstd_rand0::result_type>(16807));
}
