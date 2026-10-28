// The <typeindex> model stores the type_info address it was constructed
// from. operator== calls type_info::operator==. hash_code() and name()
// forward to that object. std::hash<type_index> returns hash_code().
// Comparing type_index is not a proof of a program's behavior.
//
// CMake's prism_tests source list is explicit (not a glob). The header
// is included by path here (`#include <typeindex>` would be the platform
// header). The same expressions are read from the header text.

#include <doctest/doctest.h>

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4068)  // #pragma GCC system_header in the model
#endif
#include "../../src/prism/pir/models/cxx/typeindex"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path header_path() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "typeindex"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("cxx typeindex stores the type_info and compares with type_info::operator==") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    CHECK(header.filename() == "typeindex");
    CHECK(header.extension().empty());

    const std::string src = slurp(header);
    REQUIRE(!src.empty());

    CHECK(src.find("_GLIBCXX_TYPEINDEX") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std") != std::string::npos);
    CHECK(src.find("#include <typeinfo>") != std::string::npos);
    CHECK(src.find("#include <string>") == std::string::npos);
    CHECK(src.find("#include <functional>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <compare>") == std::string::npos);

    // The constructor stores the address. operator== uses type_info::operator==
    // on the two objects. hash_code() and name() forward. std::hash<type_index>
    // returns that hash_code. No stand-in name.
    CHECK(src.find("const type_info* _M_target") != std::string::npos);
    CHECK(src.find("type_index(const type_info& __rhs) noexcept : _M_target(&__rhs)") != std::string::npos);
    CHECK(src.find("return *_M_target == *__rhs._M_target;") != std::string::npos);
    CHECK(src.find("return _M_target->hash_code();") != std::string::npos);
    CHECK(src.find("return _M_target->name();") != std::string::npos);
    CHECK(src.find("return _M_target->before(*__rhs._M_target);") != std::string::npos);
    CHECK(src.find("template<typename _Tp>") != std::string::npos);
    CHECK(src.find("struct hash;") != std::string::npos);
    CHECK(src.find("struct hash<type_index>") != std::string::npos);
    CHECK(src.find("size_t operator()(const type_index& __ti) const noexcept { return __ti.hash_code(); }") !=
          std::string::npos);
    CHECK(src.find("unknown") == std::string::npos);
    CHECK(src.find("\"main\"") == std::string::npos);
    CHECK(src.find("return 0") == std::string::npos);
    CHECK(src.find("strcmp") == std::string::npos);

    const std::type_index a(typeid(int));
    const std::type_index b(typeid(int));
    const std::type_index c(typeid(double));
    CHECK(a == b);
    CHECK_FALSE(a == c);
    CHECK(a.hash_code() == typeid(int).hash_code());
    CHECK(c.hash_code() == typeid(double).hash_code());
    const char* an = a.name();
    const char* cn = c.name();
    CHECK(an == typeid(int).name());
    CHECK(cn == typeid(double).name());
    REQUIRE(an != nullptr);
    REQUIRE(cn != nullptr);
    CHECK(std::strcmp(an, "unknown") != 0);
    CHECK(std::strcmp(an, "main") != 0);
    CHECK(std::strcmp(cn, "unknown") != 0);
    CHECK(std::strcmp(cn, "main") != 0);
    const int lt = (a < c) ? 1 : 0;
    const int before = typeid(int).before(typeid(double)) ? 1 : 0;
    CHECK(lt == before);

    // std::hash forwards hash_code(). Equal type_index objects share that
    // hash. The value is not taken from name().
    const std::hash<std::type_index> hasher;
    CHECK(hasher(a) == a.hash_code());
    CHECK(hasher(b) == b.hash_code());
    CHECK(hasher(c) == c.hash_code());
    CHECK(hasher(a) == hasher(b));
    CHECK(hasher(a) == typeid(int).hash_code());
    CHECK(hasher(c) == typeid(double).hash_code());
}
