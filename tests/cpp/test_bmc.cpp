// Table-driven doctests for prism::run_bmc (declared in prism/stages.hpp).
// Ports the testdata oracle rows from tests/test_bmc.py TestBMC and k-induction
// extras from TestKInduction. Linked into prism_tests next to test_main.cpp.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/models.hpp"
#include "prism/stages.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace laws = prism::laws;

fs::path td_root() { return fs::path(PRISM_SOURCE_DIR) / "testdata"; }

using Names = std::vector<std::string_view>;

prism::FunctionInfo load_fn(std::string_view file, std::string_view name) {
    auto p = td_root() / file;
    for (auto& f : prism::extract_functions(p, p.string()))
        if (f.name == name) return f;
    FAIL("missing " << name << " in " << file);
    return {};
}

prism::Finding bmc_one(const prism::FunctionInfo& fn, int unwind = 8) {
    auto recs = prism::run_bmc({fn}, unwind);
    REQUIRE(recs.size() == 1);
    return recs[0];
}

bool one_of(std::string_view s, const Names& xs) {
    return std::find(xs.begin(), xs.end(), s) != xs.end();
}

std::string extra_get(const prism::Finding& f, std::string_view key) {
    auto it = f.extra.find(std::string(key));
    return it == f.extra.end() ? std::string() : it->second;
}

const Names kProof = {laws::PROVED, laws::PROVED_UNBOUNDED};
const Names kProofOrBounded = {laws::PROVED, laws::PROVED_UNBOUNDED, laws::BOUNDED};
const Names kFsmOk = {laws::FAILED, laws::PROVED, laws::PROVED_UNBOUNDED, laws::BOUNDED};

struct BmcRow {
    std::string_view id;
    std::string_view file;
    std::string_view func;
    std::string_view exact_status;  // if set, status must match
    Names allow_status;
    Names forbid_status;
    std::string_view exact_cls;
    Names allow_cls;
    bool counterexample = false;
    std::vector<std::pair<std::string_view, std::string_view>> extra;
};

const std::vector<BmcRow>& bmc_rows() {
    static const std::vector<BmcRow> rows = {
        {"overflow", "add_overflow.c", "add_overflow", laws::FAILED, {}, {laws::ERROR, laws::NOTRUN},
         "INT-SIGNED-OVF", {}, true, {{"incremental_k", "1"}, {"param_premise", "named"}}},
        {"div0", "div_param.c", "div_param", laws::FAILED, {}, {laws::ERROR}, "INT-DIV-ZERO", {}},
        {"oob", "oob_write.c", "oob_write", laws::FAILED, {}, {laws::ERROR}, {},
         {"MEM-OOB-WRITE", "MEM-OOB-READ"}},
        {"abs", "abs_ok.c", "abs_ok", {}, kProof, {laws::ERROR, laws::NEEDS_HARNESS}, {}, {}},
        {"fsm", "fsm.c", "fsm_step", {}, kFsmOk, {laws::ERROR}, {}, {}},
        {"masked_switch", "masked_switch.c", "masked_switch", {}, kFsmOk, {laws::ERROR}, {}, {}},
        {"loop_prove", "loop_prove.c", "loop_prove", {}, kProof, {laws::ERROR}, {}, {}},
        {"loop_overflow", "loop_overflow.c", "loop_overflow", laws::FAILED, {}, {laws::ERROR}, "INT-SIGNED-OVF", {}},
        {"taut_bound_ok", "taut_bound.c", "taut_bound_ok", {}, kProofOrBounded, {laws::ERROR}, {}, {}},
        {"do_once", "subset.c", "do_once", {}, kProof, {laws::ERROR}, {}, {}},
        {"do_overflow", "subset.c", "do_overflow", laws::FAILED, {}, {laws::ERROR}, "INT-SIGNED-OVF", {}},
        {"sizeof_int", "subset.c", "sz_int", {}, kProof, {laws::ERROR}, {}, {}},
        {"sizeof_arr", "subset.c", "sz_arr", {}, kProof, {laws::ERROR}, {}, {}},
        {"ternary_pick", "subset.c", "pick", {}, kProof, {laws::ERROR}, {}, {}},
        {"ternary_abs_ovf", "subset.c", "abs_ter", laws::FAILED, {}, {laws::ERROR}, "INT-SIGNED-OVF", {}},
        {"trunc_ok", "trunc.c", "trunc_ok", {}, kProof, {laws::ERROR}, {}, {}},
        {"trunc_bad", "trunc.c", "trunc_bad", {}, {}, {laws::ERROR}, {}, {}},
        {"continue_skip", "continue.c", "cont_skip", {}, kProof, {laws::FAILED, laws::ERROR}, {}, {}},
        {"comma_sum", "comma.c", "comma_sum", {}, kProof, {laws::ERROR}, {}, {}},
        {"comma_ovf", "comma.c", "comma_ovf", laws::FAILED, {}, {laws::ERROR}, "INT-SIGNED-OVF", {}},
        {"goto_unstructured", "goto_structured.c", "goto_into_block", laws::NEEDS_HARNESS, {},
         {laws::PROVED, laws::PROVED_UNBOUNDED, laws::BOUNDED}, {}, {}},
        {"unsigned_add_wrap", "unsigned.c", "add_u", {}, kProof, {laws::FAILED, laws::ERROR}, {}, {}},
        {"unsigned_idx_ok", "unsigned.c", "idx_u_ok", {}, kProof, {laws::ERROR}, {}, {}},
        {"unsigned_idx_bad", "unsigned.c", "idx_u_bad", laws::FAILED, {}, {laws::ERROR}, {},
         {"MEM-OOB-READ", "MEM-OOB-WRITE"}},
        {"unsigned_dead", "unsigned.c", "dead_u", {}, kProof, {laws::FAILED, laws::ERROR}, {}, {}},
        {"vla_bad", "vla.c", "vla_bad", laws::NEEDS_HARNESS, {},
         {laws::PROVED, laws::PROVED_UNBOUNDED, laws::BOUNDED}, {}, {}},
        {"vla_ok", "vla.c", "vla_ok", {}, kProofOrBounded, {laws::NEEDS_HARNESS, laws::ERROR}, {}, {}},
        {"nested_ovf", "nested.c", "nested_ovf", laws::FAILED, {}, {laws::ERROR}, "INT-SIGNED-OVF",
         {}, false, {{"k_induction", "not-needed"}}},
        {"nested_ok", "nested.c", "nested_ok", {}, kProofOrBounded, {laws::ERROR, laws::FAILED}, {}, {}},
        {"long_long_ovf", "longlong.c", "add_ll", laws::FAILED, {}, {laws::ERROR}, "INT-SIGNED-OVF", {}},
        {"long_long_ok", "longlong.c", "add_ll_ok", {}, kProof, {laws::ERROR}, {}, {}},
        {"cxx_move", "use_after_move.cpp", "move_bad", laws::NEEDS_HARNESS, {}, {laws::ERROR}, {}, {}},
        {"kinduct_closed", "kinduct.c", "kinduct_closed", laws::PROVED_UNBOUNDED, {}, {laws::ERROR, laws::FAILED},
         {}, {}, false, {{"k_induction", "closed"}, {"k_induction_k", "1"}}},
        {"kinduct_step_open", "kinduct.c", "kinduct_step_open", laws::BOUNDED, {}, {laws::FAILED, laws::ERROR},
         {}, {}, false, {{"k_induction_tried", "1"}}},
        {"unchecked_alloc", "unchecked_alloc.c", "unchecked_alloc", laws::NEEDS_HARNESS, {}, {laws::ERROR}, {},
         {}},
        {"esc_bad", "stack_escape.c", "esc_bad", laws::NEEDS_HARNESS, {}, {laws::ERROR}, {}, {}},
        {"arr_esc_bad", "stack_escape.c", "arr_esc_bad", laws::NEEDS_HARNESS, {},
         {laws::PROVED, laws::PROVED_UNBOUNDED, laws::BOUNDED, laws::FAILED, laws::ERROR}, {}, {}},
    };
    return rows;
}

void check_row(const BmcRow& row) {
    INFO("row " << row.id << " " << row.file << ":" << row.func);
    auto fn = load_fn(row.file, row.func);
    auto r = bmc_one(fn);
    CHECK(r.function == row.func);
    CHECK(r.stage == "bmc");
    if (!row.exact_status.empty()) {
        CHECK_MESSAGE(r.status == row.exact_status, r.status << " " << r.message);
    } else if (!row.allow_status.empty()) {
        CHECK_MESSAGE(one_of(r.status, row.allow_status), r.status << " " << r.message);
    }
    for (auto bad : row.forbid_status) CHECK_MESSAGE(r.status != bad, r.status << " " << r.message);
    if (!row.exact_cls.empty()) CHECK(r.cls == row.exact_cls);
    else if (!row.allow_cls.empty()) CHECK_MESSAGE(one_of(r.cls, row.allow_cls), r.cls << " " << r.message);
    if (row.counterexample) CHECK_FALSE(r.counterexample.empty());
    for (auto [k, v] : row.extra)
        CHECK_MESSAGE(extra_get(r, k) == v, k << "=" << extra_get(r, k) << " expected " << v);
    if (row.id == "goto_unstructured") CHECK(r.message.find("goto") != std::string::npos);
}

}  // namespace

TEST_CASE("bmc: missing Z3 is NOTRUN, never a proof or clean") {
#ifndef PRISM_HAS_Z3
    prism::FunctionInfo fn;
    fn.file = "abs_ok.c";
    fn.name = "abs_ok";
    fn.kind = "SCALAR";
    fn.line = 1;
    fn.signature = "int abs_ok(int x)";
    fn.params = {{"int", "x"}};
    fn.body = "return x < 0 ? -x : x;";
    auto recs = prism::run_bmc({fn}, 8);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == std::string(laws::NOTRUN));
    CHECK(recs[0].status != std::string(laws::CLEAN));
    CHECK(recs[0].status != std::string(laws::PROVED));
    CHECK_FALSE(laws::is_proof(recs[0].status));
    CHECK(recs[0].message.find("z3") != std::string::npos);
    CHECK(extra_get(recs[0], "install").find("PRISM_Z3") != std::string::npos);
#else
    MESSAGE("PRISM_HAS_Z3: skip NOTRUN row");
#endif
}

#ifdef PRISM_HAS_Z3
TEST_CASE("bmc: testdata table (run_bmc)") {
    const auto& rows = bmc_rows();
    REQUIRE(rows.size() >= 15);
    for (const auto& row : rows) check_row(row);
}

TEST_CASE("run_bmc never silent-empty on a function list") {
    auto fn = load_fn("abs_ok.c", "abs_ok");
    auto recs = prism::run_bmc({fn}, 8);
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].function == "abs_ok");
    auto empty = prism::run_bmc({}, 8);
    CHECK(empty.empty());
}
#endif
