// The <random> model is the file src/prism/pir/models/cxx/random. This
// translation unit includes that file and also reads it as text. The first
// minstd_rand output after seed(1) is 48271. The header text says
// random_device is not a proof.

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
