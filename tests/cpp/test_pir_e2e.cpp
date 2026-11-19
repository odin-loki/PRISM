// Table-driven PIR stage on testdata/pir; edit this file directly when adding cases.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <cstdlib>
#include <filesystem>
#include <map>
#include <fstream>
#ifdef _WIN32
#  include <process.h>
#  define getpid _getpid
#else
#  include <unistd.h>
#endif
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace pp = prism::pir;

struct Row {
    const char* file;
    const char* function;
    const char* status;
    const char* cls;  // empty = do not check
};

const std::vector<Row>& rows() {
    static const std::vector<Row> k{
        {"asm_contract.c", "asm_bad", "FAILED", "MEM-OOB-READ"},
        {"asm_contract.c", "asm_nocontract", "NEEDS-HARNESS", ""},
        {"asm_contract.c", "asm_ok", "PROVED-ASSUMING", ""},
        {"assert.c", "assert_bad", "FAILED", "FUNC-CONTRACT"},
        {"assert.c", "assert_ok", "PROVED", ""},
        {"calls.c", "caller_bad", "FAILED", ""},
        {"calls.c", "caller_ok", "PROVED", ""},
        {"calls.c", "helper", "FAILED", ""},
        {"casts.c", "sext_mul_ok", "PROVED", ""},
        {"casts.c", "trunc_ok", "PROVED", ""},
        {"casts.c", "uchar_ok", "PROVED", ""},
        {"casts.c", "widen_bad", "FAILED", ""},
        {"casts.c", "widen_ok", "PROVED", ""},
        {"c23_static_assert.c", "sa_bad", "FAILED", "FUNC-CONTRACT"},
        {"c23_static_assert.c", "sa_ok", "PROVED", ""},
        {"coro_gen.cpp", "coro_bad", "FAILED", "INT-DIV-ZERO"},
        {"coro_gen.cpp", "coro_ok", "PROVED", ""},
        {"cxx.cpp", "constexpr_ok", "PROVED", ""},
        {"unused_constexpr.cpp", "unused_cexpr_bad", "FAILED", "INT-SIGNED-OVF"},
        {"unused_constexpr.cpp", "unused_cexpr_ok", "PROVED", ""},
        {"cxx17_shift.cpp", "shl17_bad", "FAILED", "INT-SHIFT-UB"},
        {"cxx17_shift.cpp", "shl17_sign_ok", "PROVED", ""},
        {"cxx.cpp", "cxx_add_bad", "FAILED", ""},
        {"cxx.cpp", "cxx_shl_ok", "FAILED", "INT-SHIFT-UB"},  // C++17 default: a << 4 can exceed unsigned int
        {"cxx.cpp", "get", "NEEDS-HARNESS", ""},
        {"cxx.cpp", "use_twice_ok", "PROVED", ""},
        {"div0.c", "div_bad", "FAILED", "INT-DIV-ZERO"},
        {"div0.c", "div_ok", "PROVED", ""},
        {"div0.c", "mod_min_bad", "FAILED", ""},
        {"div0.c", "urem_bad", "FAILED", ""},
        {"eh_catch.cpp", "eh_base_ok", "PROVED", ""},
        {"eh_catch.cpp", "eh_catch_bad", "FAILED", "INT-DIV-ZERO"},
        {"eh_catch.cpp", "eh_catch_ok", "PROVED", ""},
        {"eh_catch.cpp", "eh_cleanup_bad", "FAILED", "FUNC-CONTRACT"},
        {"eh_catch.cpp", "eh_cleanup_ok", "PROVED", ""},
        {"eh_catch.cpp", "eh_noexcept_bad", "FAILED", "CXX-THROW-NOEXCEPT"},
        {"eh_catch.cpp", "eh_noexcept_ok", "PROVED", ""},
        {"eh_catch.cpp", "eh_rethrow_ok", "PROVED", ""},
        {"eh_catch.cpp", "eh_std_bad", "FAILED", "CXX-THROW-NOEXCEPT"},
        {"eh_catch.cpp", "eh_std_ok", "PROVED", ""},
        {"eh_catch.cpp", "eh_wrong_type_bad", "FAILED", "CXX-THROW-NOEXCEPT"},
        {"eh_uncaught.cpp", "main", "FAILED", "CXX-UNCAUGHT"},
        {"gnu_c.c", "asm_barrier_ok", "NEEDS-HARNESS", ""},  // `asm` is unencoded: nothing is claimed
        {"gnu_c.c", "auto_type_bad", "FAILED", "INT-SIGNED-OVF"},
        {"gnu_c.c", "flag_div", "FAILED", "INT-DIV-ZERO"},
        {"gnu_c.c", "overflow_bad", "FAILED", "INT-SIGNED-OVF"},
        {"gnu_c.c", "read_once_bad", "FAILED", "INT-DIV-ZERO"},
        {"gnu_c.c", "read_once_ok", "PROVED", ""},
        {"gnu_c.c", "stmt_expr_ok", "PROVED", ""},
        {"strict_c17.c", "typeof_name_bad", "FAILED", "INT-DIV-ZERO"},
        {"strict_c17.c", "typeof_name_ok", "PROVED", ""},
        {"folded.c", "fold_div_bad", "FAILED", ""},
        {"folded.c", "fold_ok", "PROVED", ""},
        {"folded.c", "fold_shift_bad", "FAILED", ""},
        {"folded.c", "fold_shift_assign_bad", "FAILED", "INT-SHIFT-UB"},
        {"folded.c", "fold_shift_assign_ok", "PROVED", ""},
        {"folded.c", "fold_shl_neg_assign_bad", "FAILED", "INT-SHIFT-UB"},
        {"fp_arith.c", "fp_add_bad", "FAILED", "FUNC-CONTRACT"},
        {"fp_arith.c", "fp_add_ok", "PROVED", ""},
        {"fp_arith.c", "fp_cast_bad", "FAILED", "FLOAT-CAST-OVF"},
        {"fp_arith.c", "fp_cast_ok", "PROVED", ""},
        {"fp_arith.c", "fp_div", "PROVED", ""},
        {"fp_arith.c", "fp_div_guard", "PROVED", ""},
        {"fp_arith.c", "fp_exact_ok", "PROVED", ""},
        {"fp_arith.c", "fp_float_round_bad", "FAILED", "FUNC-CONTRACT"},
        {"fp_arith.c", "fp_half_way_ok", "PROVED", ""},
        {"fp_arith.c", "fp_nan_guard_ok", "PROVED", ""},
        {"fp_arith.c", "fp_sin_bad", "FAILED", "FLOAT-CAST-OVF"},
        {"fp_arith.c", "fp_sin_ok", "PROVED", ""},
        {"fp_arith.c", "fp_to_unsigned_bad", "FAILED", "FLOAT-CAST-OVF"},
        {"fp_arith.c", "twice_ok", "PROVED", ""},
        {"kind_mem.c", "kind_mem_free_bounded", "BOUNDED", ""},
        {"kind_mem.c", "kind_mem_read_bad", "FAILED", "MEM-OOB-READ"},
        {"kind_mem.c", "kind_mem_read_closed", "PROVED-UNBOUNDED", ""},
        {"kind_mem.c", "kind_mem_write_closed", "PROVED-UNBOUNDED", ""},
        {"kind_mem.c", "kind_mem_write_uninit", "BOUNDED", ""},
        {"kinduct.c", "kind_closed", "PROVED-UNBOUNDED", ""},
        {"kinduct.c", "kind_countdown", "PROVED-UNBOUNDED", ""},
        {"kinduct.c", "kind_open", "BOUNDED", ""},
        {"loops.c", "count4_ok", "PROVED", ""},
        {"loops.c", "loop_long_sum", "PROVED-UNBOUNDED", ""},  // triangular template (2s == i(i-1))
        {"loops.c", "loop_long_inv", "PROVED-UNBOUNDED", ""},
        {"loops.c", "loop_ovf_bad", "FAILED", ""},
        {"loops.c", "nested_ok", "PROVED", ""},
        {"mem_array.c", "arr_2d_ok", "PROVED", ""},
        {"mem_array.c", "arr_read_bad", "FAILED", "MEM-OOB-READ"},
        {"mem_array.c", "arr_read_ok", "PROVED", ""},
        {"mem_array.c", "arr_write_bad", "FAILED", "MEM-OOB-WRITE"},
        {"mem_array.c", "arr_write_ok", "PROVED", ""},
        {"mem_array.c", "global_bad", "FAILED", "MEM-OOB-READ"},
        {"mem_array.c", "global_ok", "PROVED", ""},
        {"mem_array.c", "struct_ok", "PROVED", ""},
        {"mem_array.c", "uninit_mem_bad", "FAILED", "UNINIT-READ"},
        {"mem_array.c", "uninit_mem_ok", "PROVED", ""},
        {"mem_contract.c", "count_pos_ok", "PROVED-ASSUMING", ""},
        {"mem_contract.c", "draft_count", "NEEDS-HARNESS", ""},
        {"mem_contract.c", "first_last_ok", "PROVED-ASSUMING", ""},
        {"mem_contract.c", "no_contract", "NEEDS-HARNESS", ""},
        {"mem_contract.c", "off_by_one_bad", "FAILED", "MEM-OOB-WRITE"},
        {"mem_contract.c", "past_end_bad", "FAILED", "MEM-OOB-READ"},
        {"mem_contract.c", "readonly_write_bad", "FAILED", "MEM-WRITE-CONST"},
        {"mem_cxx.cpp", "delete_twice_bad", "FAILED", "MEM-DOUBLE-FREE"},
        {"mem_cxx.cpp", "mismatch_bad", "FAILED", "MEM-MISMATCHED-FREE"},
        {"mem_cxx.cpp", "new_array_ok", "PROVED", ""},
        {"mem_cxx.cpp", "new_delete_ok", "PROVED", ""},
        {"mem_cxx.cpp", "new_oob_bad", "FAILED", "MEM-OOB-READ"},
        {"mem_heap.c", "calloc_ok", "PROVED", ""},
        {"mem_heap.c", "double_free_bad", "FAILED", "MEM-DOUBLE-FREE"},
        {"mem_heap.c", "free_null_ok", "PROVED", ""},
        {"mem_heap.c", "free_offset_bad", "FAILED", "MEM-INVALID-FREE"},
        {"mem_heap.c", "heap_ok", "PROVED", ""},
        {"mem_heap.c", "heap_oob_bad", "FAILED", "MEM-OOB-WRITE"},
        {"mem_heap.c", "heap_uninit_bad", "FAILED", "UNINIT-READ"},
        {"mem_heap.c", "invalid_free_bad", "FAILED", "MEM-INVALID-FREE"},
        {"mem_heap.c", "null_bad", "FAILED", "PTR-NULL-DEREF"},
        {"mem_heap.c", "realloc_ok", "PROVED", ""},
        {"mem_heap.c", "realloc_stale_bad", "FAILED", "MEM-UAF"},
        {"mem_heap.c", "uaf_bad", "FAILED", "MEM-UAF"},
        {"mem_libc.c", "abs_bad", "FAILED", "INT-SIGNED-OVF"},
        {"mem_libc.c", "abs_ok", "PROVED", ""},
        {"mem_libc.c", "atoi_ok", "PROVED", ""},
        {"mem_libc.c", "fclose_twice_bad", "FAILED", "MEM-DOUBLE-FREE"},
        {"mem_libc.c", "fgets_bad", "FAILED", "MEM-OOB-WRITE"},
        {"mem_libc.c", "fgets_ok", "PROVED", ""},
        {"mem_libc.c", "fopen_bad", "FAILED", "PTR-NULL-DEREF"},
        {"mem_libc.c", "generic_ok", "PROVED", ""},
        {"mem_libc.c", "getenv_bad", "FAILED", "PTR-NULL-DEREF"},
        {"mem_libc.c", "getenv_ok", "PROVED", ""},
        {"mem_libc.c", "printf_args_bad", "FAILED", "FMT-ARGS"},
        {"mem_libc.c", "printf_n_bad", "FAILED", "FMT-PERCENT-N"},
        {"mem_libc.c", "printf_ok", "PROVED", ""},
        {"mem_libc.c", "printf_type_bad", "FAILED", "FMT-ARGS"},
        {"mem_libc.c", "setjmp_unenc", "PROVED", ""},
        {"mem_libc.c", "snprintf_ok", "PROVED", ""},
        {"mem_libc.c", "sprintf_bad", "FAILED", "MEM-OOB-WRITE"},
        {"mem_libc.c", "vla_ok", "PROVED", ""},
        {"mem_libc.c", "vla_oob_bad", "FAILED", "MEM-OOB-WRITE"},
        {"mem_libc.c", "vla_size_bad", "FAILED", "MEM-VLA-SIZE"},
        {"mem_ptr.c", "aligned_ok", "PROVED", ""},
        {"mem_ptr.c", "arith_bad", "FAILED", "MEM-PTR-ARITH"},
        {"mem_ptr.c", "arith_ok", "PROVED", ""},
        {"mem_ptr.c", "byval_ok", "PROVED", ""},
        {"mem_ptr.c", "cmp_bad", "FAILED", "PTR-COMPARE"},
        {"mem_ptr.c", "cmp_ok", "PROVED", ""},
        {"mem_ptr.c", "diff_ok", "PROVED", ""},
        {"mem_ptr.c", "escape_bad", "FAILED", "MEM-STACK-ESCAPE"},
        {"mem_ptr.c", "escape_ok", "PROVED", ""},
        {"mem_ptr.c", "lifetime_bad", "FAILED", "MEM-UAF"},
        {"mem_ptr.c", "literal_read_ok", "PROVED", ""},
        {"mem_ptr.c", "literal_write_bad", "FAILED", "MEM-WRITE-CONST"},
        {"mem_ptr.c", "misaligned_bad", "FAILED", "MEM-MISALIGNED"},
        {"mem_ptr.c", "null_deref_bad", "FAILED", "PTR-NULL-DEREF"},
        {"mem_ptr.c", "null_deref_ok", "PROVED", ""},
        {"mem_ptr.c", "one_past_ok", "PROVED", ""},
        {"mem_stl.cpp", "array_bad", "FAILED", "MEM-OOB-READ"},
        {"mem_stl.cpp", "array_ok", "PROVED", ""},
        {"mem_stl.cpp", "optional_bad", "FAILED", "CXX-OPTIONAL-NULL"},
        {"mem_stl.cpp", "optional_ok", "PROVED", ""},
        {"mem_stl.cpp", "span_bad", "FAILED", "MEM-OOB-READ"},
        {"mem_stl.cpp", "span_ok", "PROVED", ""},
        {"mem_stl.cpp", "unique_bad", "FAILED", "PTR-NULL-DEREF"},
        {"mem_stl.cpp", "unique_ok", "PROVED", ""},
        {"mem_stl.cpp", "vector_at_throws", "PROVED", ""},
        {"mem_stl.cpp", "vector_bad", "FAILED", "MEM-OOB-READ"},
        {"mem_stl.cpp", "vector_ok", "PROVED", ""},
        {"mem_str.c", "memcpy_bad", "FAILED", "MEM-OOB-WRITE"},
        {"mem_str.c", "memcpy_ok", "PROVED", ""},
        {"mem_str.c", "memcpy_self_bad", "FAILED", "MEM-OVERLAP"},
        {"mem_str.c", "memmove_ok", "PROVED", ""},
        {"mem_str.c", "memset_bad", "FAILED", "MEM-OOB-WRITE"},
        {"mem_str.c", "overlap_bad", "FAILED", "MEM-OVERLAP"},
        {"mem_str.c", "strcat_bad", "FAILED", "MEM-OOB-WRITE"},
        {"mem_str.c", "strcpy_bad", "FAILED", "MEM-OOB-WRITE"},
        {"mem_str.c", "strcpy_ok", "PROVED", ""},
        {"mem_str.c", "strlen_ok", "PROVED", ""},
        {"mem_str.c", "struct_self_assign_ok", "PROVED", ""},
        {"mem_str.c", "unterminated_bad", "FAILED", "MEM-OOB-READ"},
        {"overflow.c", "add_bad", "FAILED", "INT-SIGNED-OVF"},
        {"overflow.c", "add_ok", "PROVED", ""},
        {"overflow.c", "add_unsigned_ok", "PROVED", ""},
        {"overflow.c", "mul_bad", "FAILED", ""},
        {"overflow.c", "neg_bad", "FAILED", ""},
        {"select.c", "abs_bad", "FAILED", ""},
        {"select.c", "abs_ok", "PROVED", ""},
        {"select.c", "max_ok", "PROVED", ""},
        {"select.c", "sel_div_bad", "FAILED", ""},
        {"shift.c", "ashr_ok", "PROVED", ""},
        {"shift.c", "shl_amount_bad", "FAILED", "INT-SHIFT-UB"},
        {"shift.c", "shl_masked_ok", "PROVED", ""},
        {"shift.c", "shl_sign_bad", "FAILED", "INT-SHIFT-UB"},
        {"shift.c", "shl_sign_ok", "PROVED", ""},
        {"shift_static.c", "ss_fold_shift_bad", "FAILED", "INT-SHIFT-UB"},
        {"shift_static.c", "ss_shl_neg_bad", "FAILED", "INT-SHIFT-UB"},
        {"shift_static.c", "ss_shl_ok", "PROVED", ""},
        {"shift_static.c", "ss_shl_unsigned_ok", "PROVED", ""},
        {"sjlj_basic.c", "sjlj_bad", "FAILED", "INT-DIV-ZERO"},
        {"sjlj_basic.c", "sjlj_dead_frame_bad", "FAILED", "CTRL-LONGJMP-INVALID"},
        {"sjlj_basic.c", "sjlj_ok", "PROVED", ""},
        {"sjlj_basic.c", "sjlj_zero_ok", "PROVED", ""},
        {"static_init_ok.cpp", "main", "PROVED", ""},
        {"static_init_throw.cpp", "main", "NEEDS-HARNESS", ""},
        {"static_init_value.cpp", "main", "NEEDS-HARNESS", ""},
        {"switch.c", "sw_bad", "FAILED", "INT-DIV-ZERO"},
        {"switch.c", "sw_ok", "PROVED", ""},
        {"unencoded.c", "deref_ptr", "NEEDS-HARNESS", ""},
        {"unencoded.c", "local_array", "PROVED", ""},
        {"unencoded.c", "recurse", "NEEDS-HARNESS", ""},
        {"unencoded.c", "twice_double", "PROVED", ""},
        {"uninit.c", "uninit_bad", "FAILED", "UNINIT-READ"},
        {"uninit.c", "uninit_ok", "PROVED", ""},
        {"virt_dispatch.cpp", "fnptr_bad", "FAILED", "INT-SIGNED-OVF"},
        {"virt_dispatch.cpp", "fnptr_ok", "PROVED", ""},
        {"virt_dispatch.cpp", "virt_bad", "FAILED", "INT-DIV-ZERO"},
        {"virt_dispatch.cpp", "virt_ok", "PROVED", ""},
    };
    return k;
}

const std::vector<const char*>& pir_files_all() {
    static const std::vector<const char*> k{
        "asm_contract.c",
        "assert.c",
        "calls.c",
        "c23_static_assert.c",
        "casts.c",
        "coro_gen.cpp",
        "cxx.cpp",
        "cxx17_shift.cpp",
        "unused_constexpr.cpp",
        "div0.c",
        "eh_catch.cpp",
        "eh_uncaught.cpp",
        "folded.c",
        "gnu_c.c",
        "strict_c17.c",
        "fp_arith.c",
        "kind_mem.c",
        "kinduct.c",
        "loops.c",
        "mem_array.c",
        "mem_contract.c",
        "mem_cxx.cpp",
        "svcomp_sink.c",
        "mem_heap.c",
        "mem_libc.c",
        "mem_ptr.c",
        "mem_stl.cpp",
        "mem_str.c",
        "overflow.c",
        "select.c",
        "shift.c",
        "shift_static.c",
        "sjlj_basic.c",
        "static_init_ok.cpp",
        "static_init_throw.cpp",
        "static_init_value.cpp",
        "switch.c",
        "unencoded.c",
        "uninit.c",
        "virt_dispatch.cpp",
    };
    return k;
}

const std::vector<const char*>& pir_files() { return pir_files_all(); }

std::map<std::pair<std::string, std::string>, const prism::Finding*> index(
    const std::vector<prism::Finding>& out) {
    std::map<std::pair<std::string, std::string>, const prism::Finding*> m;
    for (auto& f : out) {
        if (!f.function) continue;
        auto key = std::make_pair(fs::path(f.file).filename().string(), *f.function);
        m[key] = &f;
    }
    return m;
}

bool skip_without_frontend(const prism::Config& cfg) {
    std::string why;
    if (!pp::frontend_usable(cfg, &why)) {
        MESSAGE(why);
        return true;
    }
    if (!pp::z3_available()) {
        MESSAGE("NOTRUN: pir e2e needs Z3 (rebuild with -DPRISM_Z3=ON)");
        return true;
    }
    return false;
}

}  // namespace

TEST_CASE("pir e2e: testdata/pir verdict table (NOTRUN without clang/opt/LP64 probe)") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    std::vector<fs::path> paths;
    for (auto* name : pir_files()) paths.push_back(dir / name);
    auto out = pp::run_pir(paths, cfg);
    auto got = index(out);
    std::vector<std::string> wrong;
    for (auto& r : rows()) {
        auto it = got.find({r.file, r.function});
        if (it == got.end()) {
            wrong.push_back(std::string(r.file) + ":" + r.function + " missing");
            continue;
        }
        auto& f = *it->second;
        // real bugs past unwind 8 (kind_open overflows at n = 31, a[1] of
        // kind_mem_write_uninit stays uninitialised): unwind deepening may
        // find them within the budget. FAILED (validated, with the deepened
        // unwind recorded) or BOUNDED are both right; PROVED never is.
        const std::string fn_name = r.function;
        const bool deep_bug = fn_name == "kind_open" || fn_name == "kind_mem_write_uninit";
        if (deep_bug && f.status == prism::laws::FAILED && f.extra.count("unwind_deepened_to")) {
            if (f.counterexample.empty())
                wrong.push_back(std::string(r.file) + ":" + r.function + " FAILED without counterexample");
            continue;
        }
        if (f.status != r.status) {
            wrong.push_back(std::string(r.file) + ":" + r.function + " status " + f.status + " != " + r.status);
            continue;
        }
        if (r.cls[0] && f.cls != r.cls)
            wrong.push_back(std::string(r.file) + ":" + r.function + " cls " + f.cls + " != " + r.cls);
        if (f.status == prism::laws::FAILED) {
            if (f.counterexample.empty())
                wrong.push_back(std::string(r.file) + ":" + r.function + " FAILED without counterexample");
            else if (f.extra.count("cex") && f.extra.at("cex") != f.counterexample)
                wrong.push_back(std::string(r.file) + ":" + r.function + " cex mismatch");
        }
    }
    for (auto& w : wrong) MESSAGE(w);
    CHECK(wrong.empty());
}

TEST_CASE("pir e2e: Law 9 holds translation validation without --allow-exec") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    auto out = pp::run_pir({dir / "overflow.c"}, cfg);
    auto got = index(out);
    REQUIRE(got.contains({"overflow.c", "add_ok"}));
    CHECK(got.at({"overflow.c", "add_ok"})->extra.at("tv") == "NOTRUN (needs --allow-exec)");
    int notrun_rows = 0;
    for (auto& f : out)
        if (f.status == prism::laws::NOTRUN) ++notrun_rows;
    CHECK(notrun_rows == 1);
    CHECK(out.back().extra.at("reason") == "executes-scanned-code");
}

TEST_CASE("pir e2e: --fp-checks is opt-in (NOTRUN without clang/opt)") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.fp_checks = true;
    auto out = pp::run_pir({dir / "fp_arith.c"}, cfg);
    auto got = index(out);
    REQUIRE(got.contains({"fp_arith.c", "fp_div"}));
    auto& f = *got.at({"fp_arith.c", "fp_div"});
    CHECK(f.status == prism::laws::FAILED);
    CHECK((f.cls == "FLOAT-DIV-ZERO" || f.cls == "FLOAT-INVALID" || f.cls == "FLOAT-OVERFLOW"));
    REQUIRE(got.contains({"fp_arith.c", "fp_div_guard"}));
    CHECK(got.at({"fp_arith.c", "fp_div_guard"})->status == prism::laws::PROVED);
}

TEST_CASE("pir e2e: --strict-aliasing is opt-in (NOTRUN without clang/opt)") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    {
        auto out = pp::run_pir({dir / "mem_alias.c"}, cfg);
        auto got = index(out);
        REQUIRE(got.contains({"mem_alias.c", "pun_bad"}));
        CHECK(got.at({"mem_alias.c", "pun_bad"})->status == prism::laws::PROVED);
    }
    cfg.strict_aliasing = true;
    auto out = pp::run_pir({dir / "mem_alias.c"}, cfg);
    auto got = index(out);
    REQUIRE(got.contains({"mem_alias.c", "pun_bad"}));
    auto& f = *got.at({"mem_alias.c", "pun_bad"});
    CHECK(f.status == prism::laws::FAILED);
    CHECK(f.cls == "MEM-STRICT-ALIAS");
    REQUIRE(got.contains({"mem_alias.c", "char_ok"}));
    CHECK(got.at({"mem_alias.c", "char_ok"})->status == prism::laws::PROVED);
}

// Regression (OOM at 12.8 GB): run_pir lowers, analyses and validates in
// batches and frees each batch's IR. Batch size 1 must give the same verdicts
// as one batch holding every unit.
TEST_CASE("pir e2e: PRISM_PIR_BATCH=1 gives the same verdicts as the default") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 1;
    std::vector<fs::path> paths;
    for (const char* n : {"overflow.c", "casts.c", "loops.c"})
        if (fs::exists(dir / n)) paths.push_back(dir / n);
    REQUIRE(paths.size() >= 2);
    auto verdicts = [&](const char* batch) {
#ifdef _WIN32
        _putenv_s("PRISM_PIR_BATCH", batch);
#else
        ::setenv("PRISM_PIR_BATCH", batch, 1);
#endif
        std::map<std::pair<std::string, std::string>, std::string> m;
        for (auto& f : pp::run_pir(paths, cfg))
            if (f.function) m[{fs::path(f.file).filename().string(), *f.function}] = f.status;
        return m;
    };
    auto one = verdicts("1");
    auto all = verdicts("1000");
#ifdef _WIN32
    _putenv_s("PRISM_PIR_BATCH", "");
#else
    ::unsetenv("PRISM_PIR_BATCH");
#endif
    CHECK(!one.empty());
    CHECK(one == all);
}

// Regression (straggler): without a per-function default budget one function's
// Z3 query held a whole batch for 10+ minutes on full testdata.
TEST_CASE("pir: the default per-function budget is bounded and follows --timeout") {
    auto cfg = prism::default_config();
    CHECK(pp::default_function_budget_s(cfg) > 0);
    CHECK(pp::default_function_budget_s(cfg) >= 300.0);
    cfg.timeout = 120;
    CHECK(pp::default_function_budget_s(cfg) == doctest::Approx(1200.0));
}

// Gap category 1 (pir-vs-bmc, 242 of 339 superset gaps): clang rejected the
// whole unit for one function bmc's front end skipped; the other functions
// had no verdict. Now the rejected function is cut out (NEEDS-HARNESS) and
// the rest of the unit is checked.
TEST_CASE("pir e2e: a function clang rejects is cut out and the rest of the unit is checked") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "planted";
    cfg.root = dir;
    cfg.jobs = 1;
    std::vector<fs::path> paths;
    for (const char* n : {"aligned_unenc.c", "nested_fn.c"})
        if (fs::exists(dir / n)) paths.push_back(dir / n);
    if (paths.empty()) return;
    std::map<std::string, prism::Finding> m;
    for (auto& f : pp::run_pir(paths, cfg))
        if (f.function) m[*f.function] = f;
    if (fs::exists(dir / "aligned_unenc.c")) {
        REQUIRE(m.count("aligned_unenc_bad"));
        CHECK(m["aligned_unenc_bad"].status == "NEEDS-HARNESS");
        CHECK(m["aligned_unenc_bad"].message.find("front end rejects") != std::string::npos);
        REQUIRE(m.count("aligned_unenc_ok"));
        CHECK(m["aligned_unenc_ok"].status.starts_with("PROVED"));
    }
    if (fs::exists(dir / "nested_fn.c")) {
        REQUIRE(m.count("nested_fn_ok"));
        CHECK(m["nested_fn_ok"].status.starts_with("PROVED"));
        // the nested function itself is hoisted to file scope and checked
        REQUIRE(m.count("inner"));
        CHECK(m["inner"].status.starts_with("PROVED"));
    }
}

// Gap (hard conflicts, 115 of 117): reach_error's assert(0) is FAILED alone.
// Discharged at the call sites only when every caller inlined it and is proved.
TEST_CASE("pir e2e: an error sink is proved assuming its proved callers, and stays FAILED otherwise") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const auto dir = fs::temp_directory_path() / ("prism_sink_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    auto put = [&](const char* name, const char* main_body) {
        std::ofstream(dir / name) << "extern void __assert_fail(const char*, const char*, unsigned, const char*)"
                                     " __attribute__((__noreturn__));\n"
                                     "void reach_error(void) { __assert_fail(\"0\", \"x.c\", 1, \"reach_error\"); }\n"
                                     "int main(void) {\n" << main_body << "\n}\n";
        return dir / name;
    };
    std::vector<fs::path> paths{put("sink_safe.c", "  int x = 3; if (x != 3) reach_error(); return 0;"),
                                put("sink_bad.c", "  reach_error(); return 0;")};
    cfg.root = dir;
    cfg.jobs = 1;
    std::map<std::pair<std::string, std::string>, std::string> m;
    for (auto& f : pp::run_pir(paths, cfg))
        if (f.function) m[{fs::path(f.file).filename().string(), *f.function}] = f.status;
    fs::remove_all(dir);
    CHECK(m[{"sink_safe.c", "reach_error"}] == "PROVED-ASSUMING");
    // The sink itself is discharged to its callers either way; the failure is
    // reported where it is reached (main), not lost.
    CHECK(m[{"sink_bad.c", "reach_error"}] == "PROVED-ASSUMING");
    CHECK(m[{"sink_bad.c", "main"}] == "FAILED");
    CHECK(m[{"sink_safe.c", "main"}].starts_with("PROVED"));
}

// Gap (harness.h nd_char): a static inline helper in a header that no unit
// calls is emitted by no unit; the header is lowered alone for it.
TEST_CASE("pir: unused static inline function in a header gets a verdict") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const auto dir = fs::temp_directory_path() / ("prism_hdr_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    std::ofstream(dir / "h.h") << "static inline int used_h(int x) { return x & 7; }\n"
                                  "static inline int unused_h(int x) { return x / 2; }\n";
    std::ofstream(dir / "u.c") << "#include \"h.h\"\nint user(int x) { return used_h(x); }\n";
    cfg.root = dir;
    cfg.jobs = 1;
    std::map<std::pair<std::string, std::string>, std::string> m;
    for (auto& f : pp::run_pir({dir / "u.c", dir / "h.h"}, cfg))
        if (f.function) m[{fs::path(f.file).filename().string(), *f.function}] = f.status;
    fs::remove_all(dir);
    CHECK(m[{"h.h", "unused_h"}].starts_with("PROVED"));
    CHECK(m[{"u.c", "user"}].starts_with("PROVED"));
}

// Regression: the Linux kernel's include/asm-generic/rwonce.h pattern
// (typeof(_Generic(...)), statement expressions, __auto_type, `asm`) failed
// strict -std=c17 with `error: expected ')'`, also on a preprocessed copy.
namespace {
fs::path cc_scratch(const char* tag) {
    auto d = fs::temp_directory_path() / (std::string("prism_cc_") + tag + "_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}
void write_text(const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }
}  // namespace

TEST_CASE("pir e2e: GNU C unit (rwonce.h typeof/_Generic) compiles, and a unit that is valid C17 stays strict C17") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    auto out = pp::run_pir({dir / "gnu_c.c", dir / "strict_c17.c"}, cfg);
    auto got = index(out);
    for (auto& f : out)
        CHECK_MESSAGE(f.message.find("clang front end failed") == std::string::npos, f.message);
    REQUIRE(got.contains({"gnu_c.c", "read_once_ok"}));
    CHECK(got.at({"gnu_c.c", "read_once_ok"})->status == prism::laws::PROVED);
    REQUIRE(got.contains({"gnu_c.c", "read_once_bad"}));
    CHECK(got.at({"gnu_c.c", "read_once_bad"})->status == prism::laws::FAILED);
    // `asm` is not claimed to be encoded
    REQUIRE(got.contains({"gnu_c.c", "asm_barrier_ok"}));
    CHECK(got.at({"gnu_c.c", "asm_barrier_ok"})->status == prism::laws::NEEDS_HARNESS);
    // typeof and asm are identifiers here: only strict C17 parses the unit, so it was not switched to gnu17
    REQUIRE(got.contains({"strict_c17.c", "typeof_name_ok"}));
    CHECK(got.at({"strict_c17.c", "typeof_name_ok"})->status == prism::laws::PROVED);
    // no --compile-commands: no flags note
    CHECK(got.at({"gnu_c.c", "read_once_ok"})->extra.count("front_end_flags") == 0);
}

TEST_CASE("pir e2e: --compile-commands gives a unit its own flags; warnings and UB-defining flags are not taken") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    const auto tmp = cc_scratch("db");
    const auto unit = dir / "gnu_c.c";
    write_text(tmp / "compile_commands.json",
               "[{\"directory\":\"" + tmp.generic_string() + "\",\"file\":\"" + unit.generic_string() +
                   "\",\"arguments\":[\"clang\",\"-std=gnu11\",\"-w\",\"-Wno-shift-overflow\",\"-fno-strict-overflow\","
                   "\"-fwrapv\",\"-ftrivial-auto-var-init=pattern\",\"-DFIX_DIV=7\",\"-fshort-wchar\",\"-c\","
                   "\"gnu_c.c\",\"-o\",\"x.o\"]}]");
    cfg.root = dir;
    cfg.compile_commands = tmp / "compile_commands.json";
    auto out = pp::run_pir({unit}, cfg);
    auto got = index(out);
    // -DFIX_DIV=7 reached the front end: 100 / 7 is proved (without the flag it is 100 / n)
    REQUIRE(got.contains({"gnu_c.c", "flag_div"}));
    CHECK(got.at({"gnu_c.c", "flag_div"})->status == prism::laws::PROVED);
    // -fno-strict-overflow / -fwrapv were NOT taken: signed overflow is still found
    REQUIRE(got.contains({"gnu_c.c", "overflow_bad"}));
    CHECK(got.at({"gnu_c.c", "overflow_bad"})->status == prism::laws::FAILED);
    CHECK(got.at({"gnu_c.c", "overflow_bad"})->cls == "INT-SIGNED-OVF");
    const auto& note = got.at({"gnu_c.c", "overflow_bad"})->extra.at("front_end_flags");
    CHECK(note.find("-std=gnu11") != std::string::npos);
    for (const char* dropped : {"-w", "-fno-strict-overflow", "-fwrapv", "-ftrivial-auto-var-init=pattern"})
        CHECK_MESSAGE(note.find(dropped) != std::string::npos, dropped);
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

TEST_CASE("pir e2e: --pir-build-semantics takes the build's -fwrapv / -fno-strict-overflow; shift amounts and division stay UB") {
    auto base = prism::default_config();
    if (skip_without_frontend(base)) return;
    namespace L = prism::laws;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    const auto unit = dir / "build_semantics.c";
    for (const char* flag : {"-fno-strict-overflow", "-fwrapv"}) {
        const auto tmp = cc_scratch("sem");
        write_text(tmp / "compile_commands.json",
                   "[{\"directory\":\"" + tmp.generic_string() + "\",\"file\":\"" + unit.generic_string() +
                       "\",\"arguments\":[\"clang\",\"-std=gnu11\",\"" + flag + "\",\"-c\",\"build_semantics.c\",\"-o\",\"x.o\"]}]");
        auto cfg = base;
        cfg.root = dir;
        cfg.compile_commands = tmp / "compile_commands.json";
        // without the flag: ISO C, the build's option is not taken
        auto iso = index(pp::run_pir({unit}, cfg));
        for (const char* f : {"wrap_add", "wrap_mul", "wrap_sub", "wrap_shl_sign"}) {
            REQUIRE_MESSAGE(iso.contains({"build_semantics.c", f}), f);
            CHECK_MESSAGE(iso.at({"build_semantics.c", f})->status == L::FAILED, f);
        }
        for (const char* f : {"folded_ovf", "folded_shl"}) {
            REQUIRE_MESSAGE(iso.contains({"build_semantics.c", f}), f);
            CHECK_MESSAGE(iso.at({"build_semantics.c", f})->status != L::PROVED, f);
        }
        cfg.pir_build_semantics = true;
        auto sem = index(pp::run_pir({unit}, cfg));
        for (const char* f : {"wrap_add", "wrap_mul", "wrap_sub", "wrap_shl_sign", "folded_ovf", "folded_shl"}) {
            REQUIRE_MESSAGE(sem.contains({"build_semantics.c", f}), f);
            const auto* r = sem.at({"build_semantics.c", f});
            CHECK_MESSAGE(r->status == L::PROVED, (std::string(flag) + " " + f + ": " + r->status + " " + r->message));
            const auto fe = r->extra.find("front_end_flags");
            if (std::string(f) == "wrap_add") CHECK(fe != r->extra.end());  // the unit's note (one function suffices)
            if (fe != r->extra.end()) CHECK(fe->second.find("taken (--pir-build-semantics): -fwrapv") != std::string::npos);
        }
        // shift amount, division by zero and INT_MIN / -1 are undefined in the -fwrapv build too: never proved
        for (const char* f : {"shift_amount_bad", "div_bad", "min_div_bad"}) {
            REQUIRE_MESSAGE(sem.contains({"build_semantics.c", f}), f);
            CHECK_MESSAGE(sem.at({"build_semantics.c", f})->status == L::FAILED, (std::string(flag) + " " + f));
        }
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }
}

TEST_CASE("pir e2e: --compile-commands on a preprocessed copy takes only language flags and finds the functions of the .c") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const auto tmp = cc_scratch("pre");
    // x.i as `clang -E` leaves it: GNU C, #line markers naming x.c. The entry's
    // -include of a header that does not exist must not be applied again.
    write_text(tmp / "pre.i",
               "# 1 \"pre.c\"\n"
               "int pre_ok(int n) {\n"
               "    typeof(n) v = n | 1;\n"
               "    return 100 / v;\n"
               "}\n"
               "int pre_bad(int n) {\n"
               "    typeof(n) v = n;\n"
               "    return 100 / v;\n"
               "}\n");
    write_text(tmp / "pre.c", "/* the unit's source */\n");
    write_text(tmp / "compile_commands.json",
               "[{\"directory\":\"" + tmp.generic_string() + "\",\"file\":\"" + (tmp / "pre.c").generic_string() +
                   "\",\"arguments\":[\"clang\",\"-std=gnu11\",\"-include\",\"/nonexistent/kconfig.h\","
                   "\"-I\",\"/nonexistent/inc\",\"-DKERNEL=1\",\"-nostdinc\",\"-c\",\"pre.c\"]}]");
    cfg.root = tmp;
    cfg.compile_commands = tmp / "compile_commands.json";
    auto out = pp::run_pir({tmp / "pre.i"}, cfg);
    auto got = index(out);
    REQUIRE(got.contains({"pre.i", "pre_ok"}));
    CHECK(got.at({"pre.i", "pre_ok"})->status == prism::laws::PROVED);
    REQUIRE(got.contains({"pre.i", "pre_bad"}));
    CHECK(got.at({"pre.i", "pre_bad"})->status == prism::laws::FAILED);
    CHECK(got.at({"pre.i", "pre_ok"})->extra.at("front_end_flags").find("preprocessed unit") != std::string::npos);
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

TEST_CASE("pir e2e: --ops-table checks an indirect call against the targets of its table field; default unchanged") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    // without the option the unit's 19 address-taken functions exceed the 16-target limit: UNENCODED
    {
        auto got = index(pp::run_pir({dir / "ops_table.c"}, cfg));
        REQUIRE(got.contains({"ops_table.c", "ops_div_bad"}));
        CHECK(got.at({"ops_table.c", "ops_div_bad"})->status != prism::laws::FAILED);
        CHECK(got.at({"ops_table.c", "ops_safe_ok"})->status != prism::laws::PROVED);
    }
    cfg.ops_table = dir / "ops_table.tsv";
    auto got = index(pp::run_pir({dir / "ops_table.c"}, cfg));
    REQUIRE(got.contains({"ops_table.c", "ops_safe_ok"}));
    CHECK(got.at({"ops_table.c", "ops_safe_ok"})->status == prism::laws::PROVED);
    REQUIRE(got.contains({"ops_table.c", "ops_div_bad"}));
    CHECK(got.at({"ops_table.c", "ops_div_bad"})->status == prism::laws::FAILED);
    CHECK(got.at({"ops_table.c", "ops_div_bad"})->cls == "INT-DIV-ZERO");
    REQUIRE(got.contains({"ops_table.c", "ops_div_ok"}));
    CHECK(got.at({"ops_table.c", "ops_div_ok"})->status == prism::laws::PROVED);
    // nine targets for one field: beyond the default maximum of 8, stays UNENCODED with the reason
    REQUIRE(got.contains({"ops_table.c", "ops_wide_many"}));
    CHECK(got.at({"ops_table.c", "ops_wide_many"})->status != prism::laws::PROVED);
    CHECK(got.at({"ops_table.c", "ops_wide_many"})->message.find("too many indirect targets") != std::string::npos);
}

TEST_CASE("pir e2e: a violation that needs mutable-global values is NEEDS-HARNESS, one reachable from the inputs stays FAILED") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    namespace L = prism::laws;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    const auto out = pp::run_pir({dir / "global_state.c"}, cfg);  // the rows must outlive the index
    auto got = index(out);
    const auto at = [&](const char* f) -> const prism::Finding& {
        REQUIRE_MESSAGE(got.contains({"global_state.c", f}), f);
        return *got.at({"global_state.c", f});
    };
    const auto globals_note = [&](const prism::Finding& r) {
        const auto it = r.extra.find("globals");
        return it == r.extra.end() ? std::string() : it->second;
    };
    // the violation needs a global value the initializer does not give: NEEDS-HARNESS, naming the globals, with both runs recorded
    for (const char* f : {"glob_ovf", "glob_oob", "glob_two"}) {
        const auto& r = at(f);
        CHECK_MESSAGE(r.status == L::NEEDS_HARNESS, (std::string(f) + ": " + r.status + " " + r.message));
        CHECK_MESSAGE(r.message.find("violation needs mutable-global values") != std::string::npos, (std::string(f) + ": " + r.message));
        const std::string note = globals_note(r);
        const std::string why = std::string(f) + ": [" + note + "]";
        CHECK_MESSAGE(note.find("arbitrary: FAILED") != std::string::npos, why);
        CHECK_MESSAGE(note.find("initial values: PROVED") != std::string::npos, why);
    }
    CHECK(at("glob_ovf").message.find("g_base") != std::string::npos);
    CHECK(at("glob_oob").message.find("g_idx") != std::string::npos);
    CHECK(at("glob_two").message.find("g_base") != std::string::npos);
    CHECK(at("glob_two").message.find("g_idx") != std::string::npos);
    // the violation persists with the initial values (NULL pointer), or comes from the inputs alone: stays FAILED
    for (const char* f : {"glob_null", "in_div", "in_ovf", "two_sites"}) {
        const auto& r = at(f);
        CHECK_MESSAGE(r.status == L::FAILED, (std::string(f) + ": " + r.status + " " + r.message));
    }
    CHECK(at("in_div").cls == "INT-DIV-ZERO");
    CHECK(at("two_sites").cls == "INT-DIV-ZERO");
    CHECK(at("in_ovf").extra.count("globals") == 0);  // no mutable global at all
    // an arbitrary-globals violation is never turned into a proof, and a proof is untouched
    CHECK(at("glob_ok").status == L::PROVED);
    for (const char* f : {"glob_ovf", "glob_oob", "glob_two", "glob_null", "in_div", "in_ovf", "two_sites"})
        CHECK_MESSAGE((at(f).status != L::PROVED && at(f).status != L::PROVED_UNBOUNDED && at(f).status != L::PROVED_ASSUMING), f);
}
