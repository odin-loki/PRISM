// C23 memset_explicit: declared by the force-included
// models/libc/forced/prism_c23_decls.h (the libc may not declare it) and
// checked by its model in models/libc/string.cpp (memset's contract).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#ifdef _WIN32
#  include <process.h>
#  define getpid _getpid
#else
#  include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
namespace pp = prism::pir;

bool skip_without_frontend(const prism::Config& cfg) {
    std::string why;
    if (!pp::frontend_usable(cfg, &why)) {
        MESSAGE(why);
        return true;
    }
    if (!pp::z3_available()) {
        MESSAGE("NOTRUN: needs Z3 (rebuild with -DPRISM_Z3=ON)");
        return true;
    }
    return false;
}
}  // namespace

TEST_CASE("pir: C23 memset_explicit compiles without a libc declaration and is checked like memset") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const auto dir = fs::temp_directory_path() / ("prism_memset_explicit_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    // no #include <string.h>: the forced-include header declares it
    std::ofstream(dir / "mx.c") << R"(int mx_ok(int x) { unsigned char k[16]; memset_explicit(k, x, sizeof k); return k[15]; }
int mx_oob(int x) { unsigned char k[16]; memset_explicit(k, 0, 17); return x; }
int mx_null(unsigned char *p) { memset_explicit(p, 0, 4); return 0; }
int mx_zero_len(int x) { memset_explicit(0, 0, 0); return x; }
)";
    std::ofstream(dir / "mx_inc.c") << R"(#include <string.h>
int mx_inc_ok(int x) { char k[4]; memset_explicit(k, x, 4); return k[3]; }
int mx_inc_oob(int x) { char k[4]; memset_explicit(k, x, (unsigned)(x & 7)); return k[0]; }
)";
    cfg.root = dir;
    cfg.jobs = 1;
    std::map<std::string, prism::Finding> m;
    for (auto& f : pp::run_pir({dir / "mx.c", dir / "mx_inc.c"}, cfg))
        if (f.function) m[*f.function] = f;
    fs::remove_all(dir);
    for (const char* fn : {"mx_ok", "mx_oob", "mx_null", "mx_zero_len", "mx_inc_ok", "mx_inc_oob"}) {
        CAPTURE(fn);
        REQUIRE(m.count(fn));
        CAPTURE(m[fn].status);
        CAPTURE(m[fn].message);
    }
    CHECK(m["mx_ok"].status.starts_with("PROVED"));
    CHECK(m["mx_zero_len"].status.starts_with("PROVED"));
    CHECK(m["mx_inc_ok"].status.starts_with("PROVED"));
    CHECK(m["mx_oob"].status == prism::laws::FAILED);
    CHECK(m["mx_inc_oob"].status == prism::laws::FAILED);
    // an unchecked pointer parameter is never PROVED
    CHECK_FALSE(m["mx_null"].status.starts_with("PROVED"));
}
