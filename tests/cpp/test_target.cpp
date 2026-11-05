// --target / PRISM_TARGET: the analysed platform (prism/abi_model.hpp).
// Windows x64 (MSVC, LLP64) next to Linux LP64 / ILP32: triples, widths,
// the CLI, and pir verdicts that differ between LP64 and LLP64
// (testdata/conformance/prism/datamodel).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/abi_model.hpp"
#include "prism/cli.hpp"
#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace pp = prism::pir;

prism::CliResult<prism::CliOptions> parse(std::initializer_list<const char*> args) {
    std::vector<const char*> v(args);
    return prism::parse_cli(std::span<const char* const>(v.data(), v.size()));
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// "target: TRIPLE" and the expected label of one datamodel task sidecar
struct DmTask {
    fs::path c;
    std::string fn, target;
    bool expected = false;
};

std::vector<DmTask> datamodel_tasks() {
    std::vector<DmTask> out;
    const fs::path dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "conformance" / "prism" / "datamodel";
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".yml") continue;
        DmTask t;
        t.c = e.path();
        t.c.replace_extension(".c");
        t.fn = e.path().stem().string();
        t.expected = t.fn.ends_with("_true");
        const std::string y = slurp(e.path());
        const auto p = y.find("target: ");
        if (p != std::string::npos) t.target = y.substr(p + 8, y.find_first_of("\r\n", p) - p - 8);
        out.push_back(t);
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.fn < b.fn; });
    return out;
}

// pir's verdict for fn in file under target; "" with a MESSAGE when the
// frontend cannot lower for that target here (Law 1: NOTRUN, not a pass)
std::string pir_status(const fs::path& file, const std::string& fn, const std::string& target,
                       std::map<std::string, std::string>* extra = nullptr) {
    auto cfg = prism::default_config();
    cfg.target = target;
    cfg.root = file.parent_path();
    cfg.jobs = 1;
    std::string why;
    if (!pp::frontend_usable(cfg, &why)) {
        MESSAGE(why);
        return {};
    }
    if (!pp::z3_available()) {
        MESSAGE("NOTRUN: pir needs Z3 (rebuild with -DPRISM_Z3=ON)");
        return {};
    }
    for (auto& f : pp::run_pir({file}, cfg))
        if (f.function && *f.function == fn) {
            if (extra) *extra = f.extra;
            return f.status;
        }
    return "missing";
}

}  // namespace

TEST_CASE("target: triples parse to the three supported data models, nothing else") {
    auto w = prism::parse_target("x86_64-pc-windows-msvc");
    REQUIRE(w);
    CHECK(w->msvc());
    CHECK(w->triple == "x86_64-pc-windows-msvc");
    CHECK(w->data_model() == "LLP64");
    CHECK(w->abi.long_w == 32);
    CHECK(w->abi.long_long_w == 64);
    CHECK(w->abi.ptr_w == 64);
    CHECK(w->abi.size_w == 64);
    CHECK(w->abi.wchar_w == 16);
    CHECK(w->cxx_stdlib() == "MSVC STL");
    // clang's IR spelling (MSVC version suffix) and the short forms
    for (auto* s : {"x86_64-pc-windows-msvc19.44.35207", "x86_64-windows-msvc", "X86_64-PC-Windows-MSVC",
                    "amd64-pc-windows-msvc"})
        CHECK_MESSAGE(prism::parse_target(s).has_value(), s);
    auto l = prism::parse_target("x86_64-linux-gnu");
    REQUIRE(l);
    CHECK(l->triple == "x86_64-unknown-linux-gnu");
    CHECK(l->data_model() == "LP64");
    CHECK(l->abi.long_w == 64);
    CHECK(l->abi.wchar_w == 32);
    CHECK(l->cxx_stdlib() == "libstdc++");
    auto i = prism::parse_target("i686-pc-linux-gnu");
    REQUIRE(i);
    CHECK(i->data_model() == "ILP32");
    CHECK(i->abi.ptr_w == 32);
    // unsupported: never mapped onto another ABI
    for (auto* s : {"", "aarch64-linux-gnu", "i686-pc-windows-msvc", "x86_64-w64-windows-gnu", "x86_64-apple-darwin",
                    "x86_64-pc-windows-msvcX", "windows", "x86_64-pc-linux-musl"})
        CHECK_MESSAGE(!prism::parse_target(s).has_value(), s);
    CHECK(prism::abi_from_data_model("llp64").long_w == 32);
    CHECK(prism::abi_from_data_model("LP64X").long_w == 64);  // a typo does not narrow widths
}

TEST_CASE("target: the default is the host, ILP32 (SV-COMP) wins over the triple") {
#if defined(_WIN32)
    CHECK(std::string(prism::host_target_triple()) == "x86_64-pc-windows-msvc");
#else
    CHECK(std::string(prism::host_target_triple()) == "x86_64-unknown-linux-gnu");
#endif
    auto d = prism::resolve_target("", "LP64");
    REQUIRE(d);
    CHECK(d->triple == prism::host_target_triple());
    auto s = prism::resolve_target("x86_64-pc-windows-msvc", "ILP32");
    REQUIRE(s);
    CHECK(s->kind == prism::TargetKind::linux_ilp32);
    CHECK_FALSE(prism::resolve_target("sparc-sun-solaris", "LP64").has_value());
    {
        prism::ActiveTargetScope scope(prism::TargetKind::windows_llp64);
        CHECK(prism::active_target().abi.long_w == 32);
        {
            prism::ActiveTargetScope inner(prism::TargetKind::linux_lp64);
            CHECK(prism::active_target().abi.long_w == 64);
        }
        CHECK(prism::active_target().msvc());
    }
    CHECK(prism::active_target().triple == prism::host_target_triple());
}

TEST_CASE("target: --target sets the config, an unsupported triple is exit 2 (never another scan)") {
    auto ok = parse({"src", "--target", "x86_64-pc-windows-msvc"});
    REQUIRE(ok);
    CHECK(ok->cfg.target == "x86_64-pc-windows-msvc");
    auto eq = parse({"src", "--target=x86_64-unknown-linux-gnu"});
    REQUIRE(eq);
    CHECK(eq->cfg.target == "x86_64-unknown-linux-gnu");
    auto bad = parse({"src", "--target", "aarch64-linux-gnu"});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().find("unsupported target 'aarch64-linux-gnu'") != std::string::npos);
    CHECK(bad.error().find("x86_64-pc-windows-msvc") != std::string::npos);
    CHECK_FALSE(parse({"src", "--target"}));
    CHECK(prism::cli_usage().find("--target TRIPLE") != std::string::npos);
    bool registered = false;
    for (auto& e : prism::env_vars()) registered |= std::string(e.name) == "PRISM_TARGET";
    CHECK(registered);
}

TEST_CASE("target: the frontend lowers with the target's triple and headers") {
    {
        pp::FrontendTargetScope s(prism::TargetKind::windows_llp64);
        CHECK(pp::frontend_msvc());
        CHECK(std::string(pp::frontend_target_triple()) == "x86_64-pc-windows-msvc");
        auto f = pp::frontend_target_flags();
        REQUIRE_FALSE(f.empty());
        CHECK(f.front() == "--target=x86_64-pc-windows-msvc");
        // printf & co stay library calls (no inline __stdio_common_vfprintf)
        CHECK(std::find(f.begin(), f.end(), "-D_NO_CRT_STDIO_INLINE") != f.end());
        // clang's MSVC driver adds -fwrapv; signed overflow must stay UB in the IR (Law 8)
        CHECK(std::find(f.begin(), f.end(), "-fno-wrapv") != f.end());
        // never the Linux sysroot for a Windows target
        for (auto& x : pp::lp64_clang_driver_flags()) CHECK(x.find("--sysroot") == std::string::npos);
    }
    {
        pp::FrontendTargetScope s(true);
        CHECK(pp::frontend_ilp32());
        CHECK_FALSE(pp::frontend_msvc());
    }
    CHECK(std::string(pp::frontend_target_triple()) == "x86_64-unknown-linux-gnu");
}

TEST_CASE("target: datamodel tasks come in LP64/LLP64 pairs with opposite answers") {
    auto tasks = datamodel_tasks();
    REQUIRE(tasks.size() >= 8);
    std::map<std::string, std::vector<const DmTask*>> by_stem;
    for (auto& t : tasks) {
        CAPTURE(t.fn);
        auto lp = prism::parse_target(t.target);
        REQUIRE(lp);
        by_stem[t.fn.substr(0, t.fn.rfind('_'))].push_back(&t);
    }
    for (auto& [stem, pair] : by_stem) {
        CAPTURE(stem);
        REQUIRE(pair.size() == 2);
        // one true and one false, for different data models, same body
        CHECK(pair[0]->expected != pair[1]->expected);
        CHECK(prism::parse_target(pair[0]->target)->kind != prism::parse_target(pair[1]->target)->kind);
        auto body = [](const DmTask& t) {
            std::string s = slurp(t.c);
            s = s.substr(s.find("\n", s.find("other data model")) + 1);
            for (auto p = s.find(t.fn); p != std::string::npos; p = s.find(t.fn)) s.replace(p, t.fn.size(), "F");
            return s;
        };
        CHECK(body(*pair[0]) == body(*pair[1]));
    }
}

TEST_CASE("target: pir gives each datamodel task its answer on both LP64 and LLP64") {
    for (auto& t : datamodel_tasks()) {
        CAPTURE(t.fn);
        CAPTURE(t.target);
        // its own target: the labelled answer
        auto st = pir_status(t.c, t.fn, t.target);
        if (!st.empty()) CHECK(st == (t.expected ? std::string(prism::laws::PROVED) : std::string(prism::laws::FAILED)));
        // the other data model: the opposite answer (the same body)
        const bool win = prism::parse_target(t.target)->msvc();
        auto other = pir_status(t.c, t.fn, win ? "x86_64-unknown-linux-gnu" : "x86_64-pc-windows-msvc");
        if (!other.empty())
            CHECK(other == (t.expected ? std::string(prism::laws::FAILED) : std::string(prism::laws::PROVED)));
    }
}

TEST_CASE("target: MSVC C++ is checked against the MSVC STL, never the libstdc++ models") {
    auto dir = fs::temp_directory_path() / "prism_target_msvc_stl";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto src = dir / "stl.cpp";
    std::ofstream(src, std::ios::binary) << "#include <array>\n"
                                            "extern \"C\" int stl_ok(int i) { std::array<int, 4> a{}; return a[i & 3]; }\n";
    std::map<std::string, std::string> extra;
    auto st = pir_status(src, "stl_ok", "x86_64-pc-windows-msvc", &extra);
    if (!st.empty() && st != "missing") {
        CHECK(extra["target"] == "x86_64-pc-windows-msvc");
        REQUIRE(extra.count("cxx_models"));
        CHECK(extra["cxx_models"].starts_with("MSVC STL"));
        CHECK(extra["cxx_models"].find("model:") == std::string::npos);
        // a proof here is a proof over the real MSVC STL code, or no proof
        CHECK(st != std::string(prism::laws::FAILED));
    }
    fs::remove_all(dir, ec);
}
