// Doctests: device-model format, module description, harness generation and the end-to-end
// check on the toy driver and toy device of testdata/module/toy (docs/DEVICE_MODEL.md).
// The end-to-end cases need clang, opt and Z3 and report NOTRUN-style skips without them
// (a missing tool is never a pass: the test says so and returns).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/module.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;
namespace pm = prism::module;

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream o;
    o << in.rdbuf();
    return o.str();
}

fs::path toy_dir() { return fs::path(PRISM_SOURCE_DIR) / "testdata" / "module" / "toy"; }

bool toolchain_missing(const prism::Config& cfg) {
    std::string why;
    if (!prism::pir::frontend_usable(cfg, &why)) {
        MESSAGE(why);
        return true;
    }
    if (!prism::pir::z3_available()) {
        MESSAGE("NOTRUN: module e2e needs Z3 (rebuild with -DPRISM_Z3=ON)");
        return true;
    }
    return false;
}

pm::Report run_toy(const std::vector<std::string>& defines, const std::string& device = "", const std::string& rows = "all") {
    auto cfg = prism::default_config();
    cfg.llm = false;
    cfg.timeout = 120;
    pm::RunOptions o;
    o.spec = toy_dir() / "toy.pmod";
    o.out = fs::temp_directory_path() / "prism-module-test";
    o.rows = {rows};
    o.defines = defines;
    o.device_override = device;
    return pm::check_module(o, cfg);
}

}  // namespace

TEST_CASE("module: the toy device model parses and describes itself") {
    auto m = pm::parse_device_model(slurp(toy_dir() / "toy.pdm"), "toy.pdm");
    CHECK(m.name == "toy");
    CHECK(m.window == 0x100);
    REQUIRE(m.regs.size() == 5);
    CHECK(m.regs[0].name == "CTRL");
    CHECK(m.regs[2].access == pm::Access::W1C);
    CHECK(m.regs[3].mask == 1);
    CHECK(m.regs[1].access == pm::Access::RO);
    CHECK(!m.regs[1].read_in.empty());
    CHECK(m.events.size() == 1);
    CHECK(m.irqs.size() == 1);
    CHECK(m.validation == "unvalidated");
    auto d = pm::describe_device(m);
    CHECK(d.find("STATUS @0x4 u32 RO") != std::string::npos);
}

TEST_CASE("module: the emitted C has the resolver interface and the write-1-to-clear effect") {
    auto m = pm::parse_device_model(slurp(toy_dir() / "toy.pdm"), "toy.pdm");
    auto c = pm::emit_device_c(m);
    CHECK(c.find("unsigned long long prism_dev_read(unsigned long off, unsigned width)") != std::string::npos);
    CHECK(c.find("void prism_dev_write(unsigned long off, unsigned long long val, unsigned width)") != std::string::npos);
    CHECK(c.find("int prism_dev_irq_level(unsigned line)") != std::string::npos);
    CHECK(c.find("static void prism_dev_event(void)") != std::string::npos);
    // INTSTAT is W1C: a write clears the bits that are set in the value
    CHECK(c.find("dm_INTSTAT & ~(dm_value & 0x1ULL)") != std::string::npos);
    // a rule of the protocol is a checked requirement, not an assumption
    CHECK(c.find("PRISM_DEV_REQUIRE((") != std::string::npos);
}

TEST_CASE("module: the shipped PL031 model parses") {
    // the model embedded in the binary is the file under src/prism/pir/models/device
    auto m = pm::parse_device_model(slurp(fs::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" / "device" / "pl031.pdm"), "pl031.pdm");
    CHECK(m.name == "pl031");
    CHECK(m.window == 0x1000);
    bool dr = false, id = false;
    for (auto& r : m.regs) {
        if (r.name == "DR") { dr = true; CHECK(r.offset == 0); CHECK(r.access == pm::Access::RO); }
        if (r.name == "ID") { id = true; CHECK(r.group); CHECK(r.group_end == 0x1000); }
    }
    CHECK(dr);
    CHECK(id);
}

TEST_CASE("module: device models that are wrong are refused with a line number") {
    auto bad = [](const char* text) {
        try {
            pm::parse_device_model(text, "bad.pdm");
        } catch (const pm::ParseError& e) {
            return std::string(e.what());
        }
        return std::string();
    };
    CHECK(bad("model m { register A @0 : u32 RW; register B @2 : u32 RW; }").find("overlaps") != std::string::npos);
    CHECK(bad("model m { window 4; register A @4 : u32 RW; }").find("outside the window") != std::string::npos);
    CHECK(bad("model m { register A @0 : u32 RW { read = nosuch + 1; } }").find("unknown name 'nosuch'") != std::string::npos);
    CHECK(bad("model m { state s : u32; event e { s = s / 2; } }").find("division") != std::string::npos);
    CHECK(bad("model m { register A @0 : u32 XX; }").find("access must be") != std::string::npos);
    CHECK(bad("model m { state a : u32; state a : u32; }").find("declared twice") != std::string::npos);
    CHECK(bad("model m { register A @0 : u32 RW { write { value = 1; } } }").find("read-only") != std::string::npos);
    CHECK(bad("model m { state s : u32; init { s = 1 } }").find("expected") != std::string::npos);
}

TEST_CASE("module: module description parses and entry points come from the registration table") {
    auto sp = pm::parse_module_spec(slurp(toy_dir() / "toy.pmod"), "toy.pmod");
    CHECK(sp.name == "toy");
    CHECK(sp.kernel == "toy");
    CHECK(sp.steps == 3);
    CHECK(sp.ops.size() == 3);
    CHECK(!sp.teardown_ops);
    // a registration-table style source: designated initialisers naming functions of the file
    const char* src =
        "static int a_probe(void) { return 0; }\n"
        "static void a_remove(void) {}\n"
        "static int a_read(int x) { return x; }\n"
        "static struct drv d = {\n"
        "\t.probe = a_probe,\n"
        "\t.remove = a_remove,\n"
        "\t.read = a_read,\n"
        "\t.flags = 3,\n"
        "};\n";
    pm::Spec s;
    s.probe = "return a_probe();";
    s.remove = "a_remove();";
    auto eps = pm::discover_entry_points(src, s);
    REQUIRE(eps.size() == 3);
    CHECK(eps[0].field == "probe");
    CHECK(eps[0].exercised);
    CHECK(eps[1].exercised);
    CHECK(!eps[2].exercised);   // a_read is registered but no entry block calls it
}

TEST_CASE("module: malformed module descriptions are refused") {
    CHECK_THROWS_AS(pm::parse_module_spec("module m; driver \"x.c\"; probe { return 0; }", "x.pmod"), pm::ParseError);  // no remove
    CHECK_THROWS_AS(pm::parse_module_spec("module m; driver \"x.c\"; bogus 1;", "x.pmod"), pm::ParseError);
    CHECK_THROWS_AS(pm::parse_module_spec("module m; kernel freebsd; driver \"x.c\";", "x.pmod"), pm::ParseError);
}

TEST_CASE("module e2e: the toy driver is correct for the toy device model (bounded, L4 with the model named)") {
    auto cfg = prism::default_config();
    if (toolchain_missing(cfg)) return;
    auto r = run_toy({});
    INFO(r.text());
    CHECK(r.verdict == "BOUNDED");          // PIR PROVED on a bounded harness is reported as BOUNDED
    CHECK(r.level == "L4 (bounded)");
    CHECK(r.device == "toy");
    bool names_model = false;
    for (auto& a : r.assumptions) if (a.find("device model toy") != std::string::npos) names_model = true;
    CHECK(names_model);
}

TEST_CASE("module e2e: planted bugs are found, each as its own property") {
    auto cfg = prism::default_config();
    if (toolchain_missing(cfg)) return;
    struct Case { const char* define; const char* expect; };
    // LEAK: the request_irq failure path forgets iounmap; LOCKLEAK: start() returns with the lock held;
    // NOCHECK: DATA written while the device is busy (a rule of the device model);
    // UAF: remove frees the state before free_irq (the handler runs on freed memory: a native memory error)
    for (auto c : {Case{"LEAK", "[module:pairing]"}, Case{"LOCKLEAK", "[module:lock]"}, Case{"NOCHECK", "[module:protocol]"},
                   Case{"UAF", "MEM-UAF"}}) {
        auto r = run_toy({c.define});
        INFO(c.define << "\n" << r.text());
        CHECK(r.verdict == "FAILED");
        CHECK((r.message.find(c.expect) != std::string::npos || r.cls == c.expect));
    }
}

TEST_CASE("module e2e: without a device model the driver can be driven out of bounds by the hardware") {
    auto cfg = prism::default_config();
    if (toolchain_missing(cfg)) return;
    // the status register's channel field is unconstrained: 3 indexes past table[3]
    auto r = run_toy({}, "none");
    INFO(r.text());
    CHECK(r.verdict == "FAILED");
    CHECK(r.cls == "MEM-OOB-READ");
    CHECK(r.level.rfind("none", 0) == 0);
}
