// Doctests: the loop idiom library (src/prism/pir/idioms/, docs/PIR.md "Loop
// idioms"; docs/TARGETED_PROVING.md W3). Shapes and matches of translated
// loops, and Houdini's idiom round: an idiom's candidates are checked like
// any other (base, step, final query), so a false twin is never proved.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include "../../src/prism/pir/idioms/idioms.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

namespace {

namespace pp = prism::pir;

bool idioms_skip() {
    std::string why;
    if (!pp::frontend_usable(prism::default_config(), &why)) {
        MESSAGE(why);
        return true;
    }
    if (!pp::z3_available()) {
        MESSAGE("NOTRUN: pir needs Z3 (rebuild with -DPRISM_Z3=ON)");
        return true;
    }
    return false;
}

void idioms_setenv(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v);
#else
    ::setenv(k, v, 1);
#endif
}

// Translated functions of a C source (no pointer parameters).
std::map<std::string, pp::Function> translate_all(const std::filesystem::path& src) {
    std::map<std::string, pp::Function> out;
    auto fe = pp::find_frontend(prism::default_config());
    std::string err;
    std::vector<pp::FoldedUb> folded;
    std::vector<std::pair<int, int>> sshl;
    auto ir = pp::lower_to_ir(fe, src, 60, err, &folded, &sshl);
    REQUIRE_MESSAGE(ir.has_value(), err);
    auto mod = pp::ir::parse_module(*ir);
    pp::TranslateOptions topt;
    topt.signed_shl = sshl;
    topt.folded = folded;
    for (auto& f : mod.functions) {
        auto tr = pp::translate(mod, f, topt);
        if (tr.fn) out.emplace(f.name, *tr.fn);
    }
    return out;
}

const char* kIdiomSrc = R"(#include <stdint.h>
int masked_ok(int n, int v) {
    int a[8] = {0};
    int i = 0;
    while (n > 0) { a[i] = v; i = (i + 1) & 7; n--; }
    return a[0];
}
int masked_bad(int n, int v) {
    int a[8] = {0};
    int i = 0;
    while (n > 0) { a[i] = v; i = (i + 1) & 15; n--; }   /* a[8..15] */
    return a[0];
}
int ring_ok(unsigned pattern, int steps) {
    unsigned char data[4];
    unsigned head = 0, tail = 0, count = 0, sum = 0;
    for (int i = 0; i < steps; i++) {
        if ((pattern >> (i & 31)) & 1) {
            if (count < 4) { data[head] = (unsigned char)i; head = (head + 1) & 3; count++; }
        } else if (count) {
            sum += data[tail]; tail = (tail + 1) & 3; count--;
        }
    }
    return (int)(sum & 0xff) + (int)count;
}
int ring_uninit_bad(unsigned pattern, int steps) {
    unsigned char data[4];
    unsigned head = 0, tail = 0, count = 0, sum = 0;
    for (int i = 0; i < steps; i++) {
        if ((pattern >> (i & 31)) & 1) {
            if (count < 4) { data[head] = (unsigned char)i; head = (head + 1) & 3; count++; }
        } else {
            sum += data[tail]; tail = (tail + 1) & 3; if (count) count--;   /* pops an empty ring */
        }
    }
    return (int)(sum & 0xff) + (int)count;
}
int nested_sum_ok(int a) {
    if (a < 0 || a > 1000000) return 0;
    int t = 0;
    for (int i = 0; i < 40; i++)
        for (int j = 0; j < 40; j++)
            t += a;
    return t;
}
int slot_count_ok(int n, int v) {
    int a[8] = {0};
    int i = 0;
    while (n > 0) {
        a[i] += v & 1;
        i = (i + 1) & 7;
        n--;
    }
    return a[0];
}
int slot_count_bad(int n, int v) {
    int a[2] = {0};
    int i = 0;
    while (n > 0) {
        a[i] += v;
        i = (i + 1) & 1;
        n--;
    }
    return a[0];
}
)";

}  // namespace

TEST_CASE("pir idioms: loop shapes and matches of translated loops (skips without clang/opt)") {
    if (idioms_skip()) return;
    auto dir = std::filesystem::temp_directory_path() / "prism_pir_idioms_shape";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "i.c") << kIdiomSrc;
    auto fns = translate_all(dir / "i.c");
    REQUIRE(fns.count("masked_ok"));
    auto names = [&](const std::string& f) {
        auto j = nlohmann::json::parse(pp::idioms::loop_shapes_json(fns.at(f)));
        std::set<std::string> s;
        for (auto& l : j)
            for (auto& n : l["idioms"]) s.insert(n.get<std::string>());
        return s;
    };
    auto m = names("masked_ok");
    CHECK(m.count("masked-index"));
    CHECK(m.count("countdown"));
    auto j = nlohmann::json::parse(pp::idioms::loop_shapes_json(fns.at("masked_ok")));
    REQUIRE(j.size() == 1);
    CHECK(j[0]["shape"].get<std::string>().find("+1&m") != std::string::npos);
    // the shape abstracts names and the mask's value: masked_bad has the same shape
    auto jb = nlohmann::json::parse(pp::idioms::loop_shapes_json(fns.at("masked_bad")));
    CHECK(jb[0]["shape"] == j[0]["shape"]);
    CHECK(names("ring_ok").count("ring-occupancy"));
    CHECK(names("nested_sum_ok").count("nested-fold"));
    // the instantiated templates: i <= 7 for the masked index
    auto loops = pp::idioms::find_loops(fns.at("masked_ok"));
    auto ctx = pp::idioms::default_context(fns.at("masked_ok"), loops);
    bool le7 = false;
    for (auto& c : pp::idioms::instantiate(fns.at("masked_ok"), loops, ctx))
        if (c.idiom == "masked-index" && c.kind == pp::idioms::TCand::Cmp && c.rel == pp::Op::Ule &&
            c.b.kind == pp::idioms::TTerm::Const && c.b.c == 7)
            le7 = true;
    CHECK(le7);
    CHECK(pp::idioms::catalogue().size() >= 10);
}

TEST_CASE("pir idioms: the idiom round proves, false twins are refuted (skips without clang/opt)") {
    if (idioms_skip()) return;
    auto dir = std::filesystem::temp_directory_path() / "prism_pir_idioms_e2e";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "i.c") << kIdiomSrc;
    idioms_setenv("PRISM_PIR_FN_CACHE", "0");
    auto cfg = prism::default_config();
    cfg.root = dir;
    cfg.jobs = 1;
    cfg.solver_cache = dir / "cache";
    std::map<std::string, prism::Finding> got;
    for (auto& f : pp::run_pir({dir / "i.c"}, cfg))
        if (f.function) got[*f.function] = f;
    // masked index: i <= 7 (k-induction may close it first; either way no a[i] is out of bounds)
    CHECK(got["masked_ok"].status == prism::laws::PROVED_UNBOUNDED);
    CHECK(got["masked_ok"].extra["loop_shapes"].find("masked-index") != std::string::npos);
    CHECK(got["masked_bad"].status == prism::laws::FAILED);
    // ring occupancy: slots [tail, tail + count) are initialised
    CHECK(got["ring_ok"].status == prism::laws::PROVED_UNBOUNDED);
    CHECK(got["ring_ok"].extra["idiom"].find("ring-occupancy") != std::string::npos);
    CHECK(got["ring_uninit_bad"].status == prism::laws::FAILED);
    CHECK(got["ring_uninit_bad"].cls == "UNINIT-READ");
    // nested fold: t == 40 * a * i (outer), t - entry(t) == a * j (inner)
    CHECK(got["nested_sum_ok"].status == prism::laws::PROVED_UNBOUNDED);
    CHECK(got["nested_sum_ok"].extra["idiom"].find("nested-fold") != std::string::npos);
    // slot count: every a[x] <= entry(n) - n (one slot per iteration, += 0 or 1)
    CHECK(got["slot_count_ok"].status == prism::laws::PROVED_UNBOUNDED);
    CHECK(got["slot_count_ok"].extra["idiom"].find("slot-count") != std::string::npos);
    CHECK(got["slot_count_bad"].status == prism::laws::FAILED);
}
