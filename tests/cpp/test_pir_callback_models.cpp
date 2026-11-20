// Driver environment models (--pir-callback-models, docs/DRIVER_ENV_MODELS.md): preconditions the kernel core
// guarantees for the parameters of driver callbacks, each a named assumption. A model applies only to a function the
// unit registers at that container field (or registration call); a violation that does not depend on the model is
// still found; a pointer bound by a model is an invented object, so a violation under it is NEEDS-HARNESS, never a
// proof.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/cli.hpp"
#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace pp = prism::pir;
namespace L = prism::laws;

const fs::path kDir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
bool has(const std::vector<std::string>& v, const std::string& s) {
    for (auto& x : v)
        if (has(x, s)) return true;
    return false;
}

// ---- translation level: hand-written IR ------------------------------------------------------------------------

const char* kIr = R"IR(%struct.ops = type { ptr, ptr }
%struct.drv = type { ptr, ptr, ptr }
%struct.id = type { i64 }
%struct.ndev = type { i32, [60 x i8] }
@ids = internal constant [3 x %struct.id] [%struct.id { i64 0 }, %struct.id { i64 1 }, %struct.id zeroinitializer]
@table = internal constant [2 x i32] [i32 5, i32 6]
@the_ops = global %struct.ops { ptr @cb_div, ptr @cb_div_any }
@the_drv = global %struct.drv { ptr null, ptr @ids, ptr @cb_probe }
define i32 @cb_div(i32 %x) {
entry:
  %d = sub i32 %x, 11
  %q = sdiv i32 100, %d
  ret i32 %q
}
define i32 @cb_div_any(i32 %x) {
entry:
  %q = sdiv i32 100, %x
  ret i32 %q
}
define i32 @not_registered(i32 %x) {
entry:
  %d = sub i32 %x, 11
  %q = sdiv i32 100, %d
  ret i32 %q
}
define i32 @cb_probe(ptr %p, ptr %id) {
entry:
  %v = load i64, ptr %id
  %e = getelementptr inbounds [2 x i32], ptr @table, i64 0, i64 %v
  %r = load i32, ptr %e
  ret i32 %r
}
define ptr @priv(ptr %d) {
entry:
  %p = getelementptr inbounds i8, ptr %d, i64 64
  ret ptr %p
}
declare ptr @alloc_ndev(i32)
define ptr @setup() {
entry:
  %d = call ptr @alloc_ndev(i32 32)
  ret ptr %d
}
define i32 @reg_user(ptr %dev) {
entry:
  %r = call i32 @register_cb(ptr @cb_open, ptr %dev)
  ret i32 %r
}
declare i32 @register_cb(ptr, ptr)
declare ptr @alloc_ndev_small(i32)
declare i32 @register_small(ptr)
define ptr @setup_small() {
entry:
  %d = call ptr @alloc_ndev_small(i32 16)
  %r = call i32 @register_small(ptr @cb_open_small)
  ret ptr %d
}
define i32 @cb_open_small(ptr %d) {
entry:
  %m = getelementptr inbounds %struct.ndev, ptr %d, i32 0, i32 0
  %x = load i32, ptr %m
  %p = call ptr @priv(ptr %d)
  %e = getelementptr inbounds i8, ptr %p, i64 28
  %y = load i32, ptr %e
  ret i32 %y
}
define i32 @cb_open(ptr %d) {
entry:
  %m = getelementptr inbounds %struct.ndev, ptr %d, i32 0, i32 0
  %x = load i32, ptr %m
  %p = call ptr @priv(ptr %d)
  %e = getelementptr inbounds i8, ptr %p, i64 28
  %y = load i32, ptr %e
  ret i32 %y
}
)IR";

pp::CallbackModel row(const char* container, uint64_t off, int param, const char* kind, const char* a, const char* b) {
    pp::CallbackModel r;
    r.interface_ = "test";
    r.container = container;
    r.field_off = off;
    r.callback = "cb";
    r.param = param;
    r.kind = kind;
    r.a = a;
    r.b = b;
    r.status = "UNVALIDATED";
    r.source = "test spec";
    return r;
}

struct Run {
    std::string status, msg;
    std::vector<std::string> assumptions;
};

Run tr_run(const std::string& fn, std::vector<pp::CallbackModel> rows, bool container_of = false) {
    static std::vector<pp::ir::Module> keep;
    keep.push_back(pp::ir::parse_module(kIr));
    const auto* f = keep.back().find(fn);
    REQUIRE(f != nullptr);
    pp::TranslateOptions o;
    o.auto_harness.on = true;
    o.container_of = container_of;
    o.callback_models = std::make_shared<const std::vector<pp::CallbackModel>>(std::move(rows));
    auto t = pp::translate(keep.back(), *f, o);
    if (!t.fn) return {std::string(L::NEEDS_HARNESS), t.reason, {}};
    auto v = pp::check_function(*t.fn, 4, 30.0);
    return {v.status, v.message, t.fn->assumptions};
}

}  // namespace

TEST_CASE("pir callback models: the TSV loader needs 10 columns, a known kind, a container, a status and a source") {
    const auto dir = fs::temp_directory_path() / "prism_cbm_loader_test";
    fs::create_directories(dir);
    auto write = [&](const std::string& name, const std::string& text) {
        std::ofstream(dir / name, std::ios::binary) << text;
        return dir / name;
    };
    std::string err;
    auto ok = pp::load_callback_models(
        write("ok.tsv", "# attribution\ninterface\tcontainer\tfield_off\tcallback\tparam\tkind\ta\tb\tstatus\tsource\n"
                        "pci\t%struct.pci_driver\t16\tprobe\t1\tid-entry\t8\t-\tUNVALIDATED\tKlever spec x\n"
                        "net\t%struct.net_device_ops\t*\tndo_*\t0\ttrailing-alloc\talloc_netdev_mqs:0\t%struct.net_device\tU\ts\n"
                        "irq\tcall:request_irq\t1\thandler\t1\trange\t0\t5\tU\ts\n"),
        err);
    CHECK(err.empty());
    REQUIRE(ok.size() == 3);
    CHECK(ok[0].field_off == 16);
    CHECK(ok[0].param == 1);
    CHECK(ok[1].field_off == ~uint64_t{0});
    CHECK(ok[2].container == "call:request_irq");
    pp::load_callback_models(write("cols.tsv", "pci\t%struct.p\t1\n"), err);
    CHECK(has(err, "10 tab-separated"));
    pp::load_callback_models(write("kind.tsv", "pci\t%struct.p\t1\tc\t0\tvalid\t1\t-\tU\ts\n"), err);
    CHECK(has(err, "unknown kind"));
    pp::load_callback_models(write("cont.tsv", "pci\tpci_driver\t1\tc\t0\trange\t1\t2\tU\ts\n"), err);
    CHECK(has(err, "container"));
    pp::load_callback_models(write("src.tsv", "pci\t%struct.p\t1\tc\t0\trange\t1\t2\tU\t\n"), err);
    CHECK(has(err, "source"));
    pp::load_callback_models(write("rng.tsv", "pci\t%struct.p\t1\tc\t0\trange\t5\t2\tU\ts\n"), err);
    CHECK(has(err, "bad number"));
    pp::load_callback_models(dir / "missing.tsv", err);
    CHECK(has(err, "cannot read"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("pir callback models: a model applies only to the functions registered at its field") {
    pp::ir::Module m = pp::ir::parse_module(kIr);
    const std::vector<pp::CallbackModel> rows{row("%struct.ops", 0, 0, "range", "0", "10")};
    CHECK(pp::resolve_callback_bindings(m, *m.find("cb_div"), rows).size() == 1);
    CHECK(pp::resolve_callback_bindings(m, *m.find("cb_div_any"), rows).empty());   // the other field
    CHECK(pp::resolve_callback_bindings(m, *m.find("not_registered"), rows).empty());
    const std::vector<pp::CallbackModel> any{row("%struct.ops", ~uint64_t{0}, 0, "range", "0", "10")};
    CHECK(pp::resolve_callback_bindings(m, *m.find("cb_div_any"), any).size() == 1);
    const std::vector<pp::CallbackModel> call{row("call:register_cb", 0, 0, "trailing-alloc", "alloc_ndev:0", "%struct.ndev")};
    auto b = pp::resolve_callback_bindings(m, *m.find("cb_open"), call);
    REQUIRE(b.size() == 1);
    CHECK(b[0].extra_bytes == 32);
    CHECK(has(b[0].text, "driver environment model: test callback cb"));
    CHECK(has(b[0].text, "from test spec"));
}

#ifdef PRISM_HAS_Z3
TEST_CASE("pir callback models: range: the model removes a violation outside it, not one inside it") {
    CHECK(tr_run("cb_div", {}).status == L::FAILED);  // x == 11
    auto on = tr_run("cb_div", {row("%struct.ops", 0, 0, "range", "0", "10")});
    CHECK_MESSAGE(on.status == L::PROVED, (on.status + " " + on.msg));
    CHECK(has(on.assumptions, "driver environment model: test callback cb (@cb_div, parameter 0): x is in [0, 10]"));
    // a range that contains the bad value removes nothing
    CHECK(tr_run("cb_div", {row("%struct.ops", 0, 0, "range", "0", "11")}).status == L::FAILED);
    // a violation independent of the model (x == 0 is in the range) is still FAILED
    CHECK(tr_run("cb_div_any", {row("%struct.ops", 8, 0, "range", "0", "10")}).status == L::FAILED);
    // the same body, not registered: no model, still FAILED
    CHECK(tr_run("not_registered", {row("%struct.ops", 0, 0, "range", "0", "10")}).status == L::FAILED);
}

TEST_CASE("pir callback models: id-entry binds the probe's id to an entry of the driver's own table") {
    // without the model the id's driver_data is arbitrary: table[v] is out of bounds
    CHECK(tr_run("cb_probe", {}).status == L::FAILED);
    auto on = tr_run("cb_probe", {row("%struct.drv", 16, 1, "id-entry", "8", "-")});
    CHECK_MESSAGE(on.status == L::PROVED, (on.status + " " + on.msg));
    CHECK(has(on.assumptions, "points to one of the entries 0..1 of the driver's id table @ids"));
    // a table offset that is not the id table field: not applied, nothing proved
    auto wrong = tr_run("cb_probe", {row("%struct.drv", 16, 1, "id-entry", "0", "-")});
    CHECK(wrong.status == L::FAILED);
    CHECK_FALSE(has(wrong.assumptions, "driver environment model"));
}

TEST_CASE("pir callback models: trailing-alloc gives the object the private area the unit allocates, no more") {
    // byte 92..95 is past the 64-byte struct: out of bounds without the model
    CHECK(tr_run("cb_open", {}).status == L::FAILED);
    auto on = tr_run("cb_open", {row("call:register_cb", 0, 0, "trailing-alloc", "alloc_ndev:0", "%struct.ndev")});
    CHECK_MESSAGE(on.status == L::PROVED, (on.status + " " + on.msg));
    CHECK(has(on.assumptions, "allocated by this driver"));
    CHECK(has(on.assumptions, "object size 96 bytes"));
    // another struct type: the model does not apply
    auto other = tr_run("cb_open", {row("call:register_cb", 0, 0, "trailing-alloc", "alloc_ndev:0", "%struct.other")});
    CHECK(other.status == L::FAILED);
    // with --pir-container-of the harness follows priv() and grows the object to cover byte 95 on its own (2 x 64 bytes,
    // one object): proved without the model; the model replaces that invented size with the allocation's
    CHECK(tr_run("cb_open", {}, true).status == L::PROVED);
    auto grown = tr_run("cb_open", {row("call:register_cb", 0, 0, "trailing-alloc", "alloc_ndev:0", "%struct.ndev")}, true);
    CHECK_MESSAGE(grown.status == L::PROVED, (grown.status + " " + grown.msg));
    CHECK(has(grown.assumptions, "d points to the start of a valid object of 96 bytes"));
    // a private area smaller than the access (the unit allocates 16 bytes): out of bounds under the model
    auto small = tr_run("cb_open_small", {row("call:register_small", 0, 0, "trailing-alloc", "alloc_ndev_small:0", "%struct.ndev")}, true);
    CHECK_MESSAGE(small.status == L::FAILED, (small.status + " " + small.msg));
    CHECK(has(small.assumptions, "object size 80 bytes"));
    // an allocator the unit does not call: no size, not applied
    CHECK(tr_run("cb_open", {row("call:register_cb", 0, 0, "trailing-alloc", "alloc_other:0", "%struct.ndev")}).status ==
          L::FAILED);
}
#endif

// ---- pir stage end to end: testdata/pir/callback_models.c ------------------------------------------------------

namespace {

bool skip_without_frontend() {
    auto cfg = prism::default_config();
    std::string why;
    if (!pp::frontend_usable(cfg, &why)) {
        MESSAGE(why);
        return true;
    }
    if (!pp::z3_available()) {
        MESSAGE("NOTRUN: pir needs Z3 (rebuild with -DPRISM_Z3=ON)");
        return true;
    }
    return false;
}

std::map<std::string, prism::Finding> run(bool models) {
    auto cfg = prism::default_config();
    cfg.root = kDir;
    cfg.jobs = 2;
    cfg.pir_harness = true;
    if (models) cfg.pir_callback_models = kDir / "callback_models.tsv";
    std::map<std::string, prism::Finding> m;
    for (auto& f : pp::run_pir({kDir / "callback_models.c"}, cfg))
        if (f.function) m[*f.function] = f;
    return m;
}

std::string extra(const prism::Finding& f, const char* k) {
    auto it = f.extra.find(k);
    return it == f.extra.end() ? std::string() : it->second;
}

}  // namespace

TEST_CASE("pir callback models: stage: models are named assumptions; real violations stay FAILED or NEEDS-HARNESS") {
    if (skip_without_frontend()) return;
    auto off = run(false);
    auto on = run(true);
    for (auto* f : {"good_probe", "bad_probe", "good_write", "bad_read", "unregistered_write", "good_open", "bad_open",
                    "good_show", "bad_show"}) {
        REQUIRE_MESSAGE(off.contains(f), f);
        REQUIRE_MESSAGE(on.contains(f), f);
        // without the option nothing changes and nothing is named
        CHECK_FALSE(has(extra(off[f], "assumptions"), "driver environment model"));
    }
    // model-dependent callbacks: not proved without the model, proved under it (named)
    for (auto* f : {"good_probe", "good_write", "good_open"}) {
        CHECK_MESSAGE(off[f].status != L::PROVED_ASSUMING, (std::string(f) + ": " + off[f].status));
        CHECK_MESSAGE(off[f].status != L::PROVED, f);
    }
    for (auto* f : {"good_probe", "good_write", "good_open", "good_show"}) {
        CHECK_MESSAGE(on[f].status == L::PROVED_ASSUMING, (std::string(f) + ": " + on[f].status + " (" + on[f].message + ")"));
        CHECK_MESSAGE(has(extra(on[f], "assumptions"), "driver environment model:"), f);
    }
    // the automatic harness alone sizes buf to cover the constant index it sees (4001 or 4097 bytes): the buffer model
    // replaces that invented size with the real one, so bad_show's write at byte 4096 is reported under the model
    CHECK(off["good_show"].status == L::PROVED_ASSUMING);
    // a scalar callback with a violation independent of the model is FAILED with and without it
    CHECK(off["bad_read"].status == L::FAILED);
    CHECK_MESSAGE(on["bad_read"].status == L::FAILED, on["bad_read"].message);
    CHECK(off["good_write"].status == L::FAILED);
    // the same body, not registered as a callback: the model does not apply
    CHECK(on["unregistered_write"].status == L::FAILED);
    // pointer callbacks with a real violation: still reported under the model, never proved
    for (auto* f : {"bad_probe", "bad_open", "bad_show"}) {
        CHECK_MESSAGE(on[f].status == L::NEEDS_HARNESS, (std::string(f) + ": " + on[f].status + " (" + on[f].message + ")"));
        CHECK_MESSAGE(has(on[f].message, "violation only under a drafted or automatic harness"), f);
    }
}

TEST_CASE("pir callback models: cli flag, off by default") {
    auto parse = [](std::initializer_list<const char*> args) {
        std::vector<const char*> v(args);
        return prism::parse_cli(std::span<const char* const>(v.data(), v.size()));
    };
    auto r = parse({"fixture.c", "--pir-callback-models", "m.tsv"});
    REQUIRE(r);
    CHECK(r->cfg.pir_callback_models.filename() == "m.tsv");
    auto d = parse({"fixture.c"});
    REQUIRE(d);
    CHECK(d->cfg.pir_callback_models.empty());
    CHECK_FALSE(parse({"fixture.c", "--pir-callback-models"}));
}
