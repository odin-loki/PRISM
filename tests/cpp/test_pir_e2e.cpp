// Table-driven PIR stage on tests/pir (was tests/test_pir.py::TestPirStage).
// Regenerate: python tools/gen_test_pir_e2e_cpp.py
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"

#include <filesystem>
#include <map>
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
        {"coro_gen.cpp", "coro_bad", "FAILED", "INT-DIV-ZERO"},
        {"coro_gen.cpp", "coro_ok", "PROVED", ""},
        {"cxx.cpp", "constexpr_ok", "PROVED", ""},
        {"cxx.cpp", "cxx_add_bad", "FAILED", ""},
        {"cxx.cpp", "cxx_shl_ok", "PROVED", ""},
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
        {"folded.c", "fold_div_bad", "FAILED", ""},
        {"folded.c", "fold_ok", "PROVED", ""},
        {"folded.c", "fold_shift_bad", "FAILED", ""},
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
        {"loops.c", "loop_long_bounded", "BOUNDED", ""},
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

const std::vector<const char*>& pir_files() {
    static const std::vector<const char*> k{
        "asm_contract.c",
        "assert.c",
        "calls.c",
        "casts.c",
        "coro_gen.cpp",
        "cxx.cpp",
        "div0.c",
        "eh_catch.cpp",
        "eh_uncaught.cpp",
        "folded.c",
        "fp_arith.c",
        "kind_mem.c",
        "kinduct.c",
        "loops.c",
        "mem_array.c",
        "mem_contract.c",
        "mem_cxx.cpp",
        "mem_heap.c",
        "mem_libc.c",
        "mem_ptr.c",
        "mem_stl.cpp",
        "mem_str.c",
        "overflow.c",
        "select.c",
        "shift.c",
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

}  // namespace

TEST_CASE("pir e2e: tests/pir verdict table (skips without clang/opt)") {
    auto cfg = prism::default_config();
    auto fe = pp::find_frontend(cfg);
    if (!fe.clang || !fe.opt) {
        MESSAGE("clang/opt not on PATH: pir e2e skipped");
        return;
    }
    auto dir = fs::path(PRISM_SOURCE_DIR) / "tests" / "pir";
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
    auto fe = pp::find_frontend(cfg);
    if (!fe.clang || !fe.opt) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "tests" / "pir";
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

TEST_CASE("pir e2e: --fp-checks is opt-in (skips without clang/opt)") {
    auto cfg = prism::default_config();
    auto fe = pp::find_frontend(cfg);
    if (!fe.clang || !fe.opt) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "tests" / "pir";
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

TEST_CASE("pir e2e: --strict-aliasing is opt-in (skips without clang/opt)") {
    auto cfg = prism::default_config();
    auto fe = pp::find_frontend(cfg);
    if (!fe.clang || !fe.opt) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "tests" / "pir";
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
