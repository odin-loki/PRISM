// The <memory_resource> model is a partial monotonic_buffer_resource. These
// checks read the header text: an allocate larger than the caller buffer is
// a __prism_check, and the header does not call ::operator new.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Body of the nth function whose text contains `sig`, including braces.
std::string body_of(std::string_view src, std::string_view sig, std::size_t nth = 0) {
    std::size_t from = 0;
    for (std::size_t n = 0;; ++n) {
        const auto at = src.find(sig, from);
        if (at == std::string_view::npos) return {};
        from = at + sig.size();
        if (n != nth) continue;
        const auto brace = src.find('{', at);
        if (brace == std::string_view::npos) return {};
        int depth = 0;
        for (std::size_t i = brace; i < src.size(); ++i) {
            if (src[i] == '{') ++depth;
            else if (src[i] == '}') {
                if (--depth == 0) return std::string(src.substr(brace, i - brace + 1));
            }
        }
        return {};
    }
}

}  // namespace

TEST_CASE("cxx memory_resource model: oversized allocate is a check, header does not call ::operator new") {
    const fs::path header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "memory_resource";
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    const auto src = slurp(header);
    REQUIRE(!src.empty());

    CHECK(src.find("::operator new") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("class polymorphic_allocator") == std::string::npos);
    CHECK(src.find("class synchronized_pool_resource") == std::string::npos);
    CHECK(src.find("class unsynchronized_pool_resource") == std::string::npos);
    CHECK(src.find("polymorphic_allocator") != std::string::npos);
    CHECK(src.find("synchronized_pool_resource") != std::string::npos);
    CHECK(src.find("unsynchronized_pool_resource") != std::string::npos);

    const auto ctor = body_of(src, "monotonic_buffer_resource(void* __buffer, size_t __buffer_size)");
    REQUIRE(!ctor.empty());
    CHECK(ctor.find("__prism_check") != std::string::npos);
    CHECK(ctor.find("null upstream buffer with non-zero size") != std::string::npos);
    CHECK(ctor.find("::operator new") == std::string::npos);

    const auto alloc = body_of(src, "void* allocate(size_t __bytes, size_t __alignment)");
    REQUIRE(!alloc.empty());
    CHECK(alloc.find("__prism_check") != std::string::npos);
    CHECK(alloc.find("exceeds the caller buffer") != std::string::npos);
    CHECK(alloc.find("__pad <= __left && __bytes <= __left - __pad") != std::string::npos);
    const auto fit = alloc.find("exceeds the caller buffer");
    const auto formed = alloc.find("_M_buf +");
    REQUIRE(fit != std::string::npos);
    REQUIRE(formed != std::string::npos);
    CHECK(fit < formed);
    CHECK(alloc.find("return nullptr") != std::string::npos);
    CHECK(alloc.find("return nullptr") < formed);
    CHECK(alloc.find("return __p") != std::string::npos);
    CHECK(formed < alloc.find("return __p"));
    CHECK(alloc.find("::operator new") == std::string::npos);
    CHECK(alloc.find("_M_buf + _M_size") == std::string::npos);

    const auto dtor = body_of(src, "~monotonic_buffer_resource()");
    REQUIRE(dtor == "{}");

    const auto de = body_of(src, "void deallocate(void* __p, size_t __bytes, size_t __alignment)");
    REQUIRE(!de.empty());
    CHECK(de.find("__prism_check") != std::string::npos);
    CHECK(de.find("not handed out") != std::string::npos);
    CHECK(de.find("::operator new") == std::string::npos);
    CHECK(de.find("::operator delete") == std::string::npos);
    CHECK(de.find("delete") == std::string::npos);
    CHECK(de.find("free(") == std::string::npos);
}
