// The <spanstream> model is a partial basic_spanbuf. These checks read the
// header text: sputc at the size is a __prism_check and does not write,
// sgetc at the size is a __prism_check and does not read, sungetc at offset
// 0 is a __prism_check and does not read before the buffer, sputn stores
// min(count, remaining) and traps on a bad count without writing, and the
// destructor is defaulted or does not call free.

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

// Body of the first function whose text contains `sig`, including braces.
// A defaulted declaration has no brace and yields an empty string.
std::string body_of(std::string_view src, std::string_view sig) {
    const auto at = src.find(sig);
    if (at == std::string_view::npos) return {};
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

}  // namespace

TEST_CASE("cxx spanstream model: write at size is a check, destructor does not free") {
    const auto header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "spanstream";
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const auto src = slurp(header);
    REQUIRE(!src.empty());

    const auto sputc = body_of(src, "int sputc(");
    REQUIRE(!sputc.empty());
    CHECK(sputc.find("__prism_check") != std::string::npos);
    CHECK(sputc.find("_M_off < _M_size") != std::string::npos);
    const auto guard = sputc.find("if (_M_off < _M_size)");
    const auto store = sputc.find("_M_ptr[_M_off]");
    REQUIRE(guard != std::string::npos);
    REQUIRE(store != std::string::npos);
    CHECK(guard < store);

    CHECK(sputc.find("MEM-OOB-WRITE") != std::string::npos);
    CHECK(sputc.find("MEM-OOB-READ") == std::string::npos);

    const bool defaulted = src.find("~basic_spanbuf() noexcept = default") != std::string::npos ||
                           src.find("~basic_spanbuf() = default") != std::string::npos;
    const auto dtor = body_of(src, "~basic_spanbuf()");
    const bool no_free = dtor.find("free(") == std::string::npos && dtor.find("::operator delete") == std::string::npos;
    CHECK((defaulted || no_free));
    if (defaulted) CHECK(src.find("free(") == std::string::npos);

    const auto ctor_at = src.find("basic_spanbuf(char* __p, size_t __n)");
    REQUIRE(ctor_at != std::string::npos);
    const auto ctor_brace = src.find('{', ctor_at);
    REQUIRE(ctor_brace != std::string::npos);
    const std::string ctor_head = src.substr(ctor_at, ctor_brace - ctor_at);
    CHECK(ctor_head.find("_M_size(__n)") != std::string::npos);
    CHECK(ctor_head.find("_M_size(0)") == std::string::npos);
    const auto ctor = body_of(src, "basic_spanbuf(char* __p, size_t __n)");
    REQUIRE(!ctor.empty());
    CHECK(ctor.find("PTR-NULL-DEREF") != std::string::npos);
    CHECK(ctor.find("__p != nullptr || __n == 0") != std::string::npos);
    CHECK(ctor.find("_M_size") == std::string::npos);
    CHECK(ctor.find("_M_ptr") == std::string::npos);

    CHECK(src.find("#include <span>") == std::string::npos);
    CHECK(src.find("#include <sstream>") == std::string::npos);
    CHECK(src.find("#include <string>") == std::string::npos);
    CHECK(src.find("#include <string_view>") == std::string::npos);
    CHECK(src.find("class spanstream") == std::string::npos);
    CHECK(src.find("class ispanstream") == std::string::npos);
    CHECK(src.find("class ospanstream") == std::string::npos);
    CHECK(src.find("class basic_spanstream") == std::string::npos);
    CHECK(src.find("class basic_ispanstream") == std::string::npos);
    CHECK(src.find("class basic_ospanstream") == std::string::npos);
}

TEST_CASE("cxx spanstream model: sgetc at size is a read check and does not load") {
    const auto header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "spanstream";
    const auto src = slurp(header);
    REQUIRE(!src.empty());

    const auto sgetc = body_of(src, "int sgetc(");
    REQUIRE(!sgetc.empty());
    CHECK(sgetc.find("MEM-OOB-READ") != std::string::npos);
    CHECK(sgetc.find("MEM-OOB-WRITE") == std::string::npos);
    CHECK(sgetc.find("_M_off < _M_size") != std::string::npos);

    const auto check = sgetc.find("__prism_check");
    const auto guard = sgetc.find("if (_M_off < _M_size)");
    const std::string load_expr = "static_cast<int>(static_cast<unsigned char>(_M_ptr[_M_off]))";
    const auto load = sgetc.find(load_expr);
    const auto adv = sgetc.find("++_M_off");
    const auto eof = sgetc.find("return -1;");
    REQUIRE(check != std::string::npos);
    REQUIRE(guard != std::string::npos);
    REQUIRE(load != std::string::npos);
    REQUIRE(adv != std::string::npos);
    REQUIRE(eof != std::string::npos);
    CHECK(check < guard);
    CHECK(guard < load);
    CHECK(load < adv);
    CHECK(adv < eof);
    CHECK(sgetc.find("_M_ptr", load + load_expr.size()) == std::string::npos);
    CHECK(sgetc.find("return -1;", eof + 1) == std::string::npos);
    CHECK(sgetc.find("_M_ptr[_M_off] =") == std::string::npos);
    CHECK(sgetc.substr(eof).find("_M_ptr") == std::string::npos);

    const auto in_range = sgetc.substr(guard, eof - guard);
    CHECK(in_range.find("return -1") == std::string::npos);
    CHECK(in_range.find("++_M_off") != std::string::npos);
}

TEST_CASE("cxx spanstream model: sungetc at offset 0 is a read check and does not load") {
    const auto header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "spanstream";
    const auto src = slurp(header);
    REQUIRE(!src.empty());

    const auto sungetc = body_of(src, "int sungetc(");
    REQUIRE(!sungetc.empty());
    CHECK(sungetc.find("MEM-OOB-READ") != std::string::npos);
    CHECK(sungetc.find("MEM-OOB-WRITE") == std::string::npos);
    CHECK(sungetc.find("std::basic_spanbuf::sungetc read before the buffer") != std::string::npos);
    CHECK(sungetc.find("_M_off > 0") != std::string::npos);
    CHECK(sungetc.find("_M_off < _M_size") == std::string::npos);

    const auto check = sungetc.find("__prism_check");
    const auto guard = sungetc.find("if (_M_off > 0)");
    const auto dec = sungetc.find("--_M_off");
    const std::string load_expr = "static_cast<int>(static_cast<unsigned char>(_M_ptr[_M_off]))";
    const auto load = sungetc.find(load_expr);
    const auto eof = sungetc.find("return -1;");
    REQUIRE(check != std::string::npos);
    REQUIRE(guard != std::string::npos);
    REQUIRE(dec != std::string::npos);
    REQUIRE(load != std::string::npos);
    REQUIRE(eof != std::string::npos);
    CHECK(check < guard);
    CHECK(guard < dec);
    CHECK(dec < load);
    CHECK(load < eof);
    const auto ptr = sungetc.find("_M_ptr");
    REQUIRE(ptr != std::string::npos);
    CHECK(ptr > dec);
    CHECK(ptr >= load);
    CHECK(ptr < load + load_expr.size());
    CHECK(sungetc.find("_M_ptr", load + load_expr.size()) == std::string::npos);
    CHECK(sungetc.find("--_M_off", dec + 1) == std::string::npos);
    CHECK(sungetc.find("++_M_off") == std::string::npos);
    CHECK(sungetc.find("_M_off -") == std::string::npos);
    CHECK(sungetc.find("_M_off-") == std::string::npos);
    CHECK(sungetc.find("return -1;", eof + 1) == std::string::npos);
    CHECK(sungetc.find("_M_ptr[_M_off] =") == std::string::npos);
    CHECK(sungetc.substr(eof).find("_M_ptr") == std::string::npos);
    CHECK(sungetc.substr(eof).find("--_M_off") == std::string::npos);

    const auto before = sungetc.substr(0, guard);
    CHECK(before.find("__prism_check") != std::string::npos);
    CHECK(before.find("--_M_off") == std::string::npos);
    CHECK(before.find("_M_off =") == std::string::npos);
    CHECK(before.find("_M_ptr") == std::string::npos);
    CHECK(before.find("return -1") == std::string::npos);

    const auto in_range = sungetc.substr(guard, eof - guard);
    CHECK(in_range.find("return -1") == std::string::npos);
    CHECK(in_range.find("--_M_off") != std::string::npos);
    CHECK(in_range.find(load_expr) != std::string::npos);
}

TEST_CASE("cxx spanstream model: sputn stores min(count, room) and traps on a bad count") {
    const auto header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "spanstream";
    const auto src = slurp(header);
    REQUIRE(!src.empty());
    CHECK(src.find("sputn is not PROVED and not CLEAN") != std::string::npos);

    const auto sputc = body_of(src, "int sputc(");
    const auto sgetc = body_of(src, "int sgetc(");
    const auto sungetc = body_of(src, "int sungetc(");
    REQUIRE(!sputc.empty());
    REQUIRE(!sgetc.empty());
    REQUIRE(!sungetc.empty());
    CHECK(sputc.find("sputn") == std::string::npos);
    CHECK(sgetc.find("sputn") == std::string::npos);
    CHECK(sungetc.find("sputn") == std::string::npos);
    CHECK(sputc.find("__builtin_trap") == std::string::npos);
    CHECK(sgetc.find("__builtin_trap") == std::string::npos);
    CHECK(sungetc.find("__builtin_trap") == std::string::npos);

    const auto sputn = body_of(src, "std::ptrdiff_t sputn(");
    REQUIRE(!sputn.empty());
    CHECK(sputn.find("MEM-OOB-WRITE") == std::string::npos);
    CHECK(sputn.find("MEM-OOB-READ") == std::string::npos);

    const auto neg = sputn.find("if (__n < 0)");
    const auto zero = sputn.find("} else if (__n == 0)");
    const auto nullp = sputn.find("} else if (__s == nullptr)");
    const auto fit = sputn.find("} else if (static_cast<size_t>(__n) > (static_cast<size_t>(-1) / sizeof(_CharT)))");
    const auto ok = sputn.find("} else {");
    REQUIRE(neg != std::string::npos);
    REQUIRE(zero != std::string::npos);
    REQUIRE(nullp != std::string::npos);
    REQUIRE(fit != std::string::npos);
    REQUIRE(ok != std::string::npos);
    CHECK(neg < zero);
    CHECK(zero < nullp);
    CHECK(nullp < fit);
    CHECK(fit < ok);

    const auto neg_check = sputn.find(
        "__prism_check(0, \"FUNC-CONTRACT\", \"std::basic_spanbuf::sputn negative count\")");
    const auto neg_trap = sputn.find("__builtin_trap()", neg_check);
    REQUIRE(neg_check != std::string::npos);
    REQUIRE(neg_trap != std::string::npos);
    CHECK(neg < neg_check);
    CHECK(neg_check < neg_trap);
    CHECK(neg_trap < zero);
    const auto neg_arm = sputn.substr(neg, zero - neg);
    CHECK(neg_arm.find("_M_ptr") == std::string::npos);
    CHECK(neg_arm.find("__s[") == std::string::npos);
    CHECK(neg_arm.find("return") == std::string::npos);
    CHECK(neg_arm.find("* sizeof") == std::string::npos);

    const auto zero_arm = sputn.substr(zero, nullp - zero);
    CHECK(zero_arm.find("return 0;") != std::string::npos);
    CHECK(zero_arm.find("_M_ptr") == std::string::npos);
    CHECK(zero_arm.find("__s[") == std::string::npos);
    CHECK(zero_arm.find("__builtin_trap") == std::string::npos);

    const auto null_check =
        sputn.find("__prism_check(0, \"PTR-NULL-DEREF\", \"std::basic_spanbuf::sputn null source\")");
    const auto null_trap = sputn.find("__builtin_trap()", null_check);
    REQUIRE(null_check != std::string::npos);
    REQUIRE(null_trap != std::string::npos);
    CHECK(nullp < null_check);
    CHECK(null_check < null_trap);
    CHECK(null_trap < fit);
    const auto null_arm = sputn.substr(nullp, fit - nullp);
    CHECK(null_arm.find("_M_ptr") == std::string::npos);
    CHECK(null_arm.find("__s[") == std::string::npos);
    CHECK(null_arm.find("return") == std::string::npos);

    const auto fit_check = sputn.find(
        "__prism_check(0, \"FUNC-CONTRACT\", \"std::basic_spanbuf::sputn count times element size does not fit\")");
    const auto fit_trap = sputn.find("__builtin_trap()", fit_check);
    REQUIRE(fit_check != std::string::npos);
    REQUIRE(fit_trap != std::string::npos);
    CHECK(fit < fit_check);
    CHECK(fit_check < fit_trap);
    CHECK(fit_trap < ok);
    const auto fit_arm = sputn.substr(fit, ok - fit);
    CHECK(fit_arm.find("_M_ptr") == std::string::npos);
    CHECK(fit_arm.find("__s[") == std::string::npos);
    CHECK(fit_arm.find("return") == std::string::npos);
    CHECK(fit_arm.find("* sizeof") == std::string::npos);
    CHECK(fit_arm.find("wrapped length") != std::string::npos);

    const auto prod = sputn.find("const size_t __bytes = static_cast<size_t>(__n) * sizeof(_CharT);");
    const auto room = sputn.find("const size_t __room = _M_off < _M_size ? _M_size - _M_off : 0;");
    const auto stored = sputn.find("const size_t __stored = __bytes < __room ? __bytes : __room;");
    const auto loop = sputn.find("for (size_t __i = 0; __i < __stored; ++__i)");
    const auto store = sputn.find("_M_ptr[_M_off] = __s[__i];");
    const auto adv = sputn.find("++_M_off;");
    const auto ret = sputn.find("return static_cast<std::ptrdiff_t>(__stored);");
    REQUIRE(prod != std::string::npos);
    REQUIRE(room != std::string::npos);
    REQUIRE(stored != std::string::npos);
    REQUIRE(loop != std::string::npos);
    REQUIRE(store != std::string::npos);
    REQUIRE(adv != std::string::npos);
    REQUIRE(ret != std::string::npos);
    CHECK(ok < prod);
    CHECK(prod < room);
    CHECK(room < stored);
    CHECK(stored < loop);
    CHECK(loop < store);
    CHECK(store < adv);
    CHECK(adv < ret);
    CHECK(sputn.find("__i < __bytes") == std::string::npos);
    CHECK(sputn.find("__i < __n") == std::string::npos);
    CHECK(sputn.find("_M_off +=") == std::string::npos);
    CHECK(sputn.find("return __n") == std::string::npos);
    CHECK(sputn.find("return static_cast<std::ptrdiff_t>(__n)") == std::string::npos);
    CHECK(sputn.find("return static_cast<std::ptrdiff_t>(__bytes)") == std::string::npos);
    CHECK(sputn.find("_M_ptr", ret) == std::string::npos);
    const auto prod_end = sputn.find(';', prod);
    REQUIRE(prod_end != std::string::npos);
    CHECK(sputn.find("* sizeof", prod_end + 1) == std::string::npos);
}
