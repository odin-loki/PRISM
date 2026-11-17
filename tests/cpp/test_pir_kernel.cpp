// Kernel API specs, inline asm / atomics models and the unknown-call policy
// (docs/KERNEL_MODELS.md). Table-driven on testdata/pir/kernel_*.c: every
// `*_ok` function is PROVED-ASSUMING with the named assumption of its spec, every
// `*_bad` function is FAILED (a true and a false variant of each spec), and the
// same functions are NEEDS-HARNESS (UNENCODED) when the opt-in is off.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace pp = prism::pir;

struct Row {
    const char* function;
    const char* status;
    const char* tag;  // PROVED-ASSUMING: a substring of the assumption list that must be there ("" = none required)
};

bool skip_without_frontend(const prism::Config& cfg) {
    std::string why;
    if (!pp::frontend_usable(cfg, &why)) {
        MESSAGE(why);
        return true;
    }
    if (!pp::z3_available()) {
        MESSAGE("NOTRUN: pir kernel tests need Z3 (rebuild with -DPRISM_Z3=ON)");
        return true;
    }
    return false;
}

std::map<std::string, prism::Finding> run(const std::string& file, const std::string& kernel, bool unknown) {
    auto cfg = prism::default_config();
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    cfg.pir_kernel_api = kernel;
    cfg.pir_assume_unknown_calls = unknown;
    auto out = pp::run_pir({dir / file}, cfg);
    std::map<std::string, prism::Finding> m;
    for (auto& f : out)
        if (f.function) m[*f.function] = f;
    return m;
}

std::string assumptions_of(const prism::Finding& f) {
    auto it = f.extra.find("assumptions");
    return it == f.extra.end() ? std::string() : it->second;
}

void check_table(const std::string& file, const std::string& kernel, bool unknown, const std::vector<Row>& rows) {
    auto got = run(file, kernel, unknown);
    std::vector<std::string> wrong;
    for (auto& r : rows) {
        auto it = got.find(r.function);
        if (it == got.end()) {
            wrong.push_back(std::string(r.function) + " missing");
            continue;
        }
        auto& f = it->second;
        if (f.status != r.status) {
            wrong.push_back(std::string(r.function) + " status " + f.status + " != " + r.status + " (" + f.message + ")");
            continue;
        }
        if (f.status == prism::laws::PROVED_ASSUMING && r.tag[0] && assumptions_of(f).find(r.tag) == std::string::npos)
            wrong.push_back(std::string(r.function) + " assumptions lack '" + r.tag + "': " + assumptions_of(f));
        if (f.status == prism::laws::PROVED && !assumptions_of(f).empty())
            wrong.push_back(std::string(r.function) + " PROVED with assumptions");
    }
    for (auto& w : wrong) MESSAGE(w);
    CHECK(wrong.empty());
}

}  // namespace

TEST_CASE("pir kernel: Linux API specs (allocation, locking, user access, MMIO, logging, panic)") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_linux.c", "linux", false,
                {{"alloc_ok", PA, "linux-alloc"},
                 {"alloc_nocheck_bad", F, ""},
                 {"zalloc_ok", PA, "__GFP_ZERO is bit 0x100"},
                 {"malloc_uninit_bad", F, ""},
                 {"double_free_bad", F, ""},
                 {"use_after_free_bad", F, ""},
                 {"oob_bad", F, ""},
                 {"kfree_null_ok", PA, "linux-free"},
                 {"krealloc_ok", PA, "linux-krealloc"},
                 {"kstrdup_ok", PA, "linux-alloc"},
                 {"vmalloc_ok", PA, "linux-alloc"},
                 {"lock_ok", PA, "linux-lock"},
                 {"lock_null_bad", F, ""},
                 {"trylock_ok", PA, "linux-lock"},
                 {"spin_irqsave_ok", PA, "linux-lock"},
                 {"copy_from_user_ok", PA, "linux-uaccess"},
                 {"copy_from_user_trust_bad", F, ""},
                 {"copy_from_user_oob_bad", F, ""},
                 {"copy_to_user_ok", PA, "linux-uaccess"},
                 {"copy_to_user_oob_bad", F, ""},
                 {"mmio_ok", PA, "linux-mmio"},
                 {"mmio_trust_bad", F, ""},
                 {"log_delay_ok", PA, "linux-log"},
                 {"log_delay_bad", F, ""},
                 {"panic_bad", F, ""},
                 {"panic_ok", PA, "linux-bug"},
                 {"container_of_ok", "PROVED", ""},
                 {"container_of_bad", F, ""}});
}

TEST_CASE("pir kernel: FreeBSD API specs (malloc, mtx, copyin/copyout, bus_space, logging, panic)") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_freebsd.c", "freebsd", false,
                {{"waitok_ok", PA, "M_WAITOK allocations succeed"},
                 {"nowait_nocheck_bad", F, ""},
                 {"nowait_ok", PA, "freebsd-alloc"},
                 {"waitok_uninit_bad", F, ""},
                 {"free_twice_bad", F, ""},
                 {"mtx_ok", PA, "freebsd-lock"},
                 {"mtx_null_bad", F, ""},
                 {"copyin_ok", PA, "freebsd-copy"},
                 {"copyin_trust_bad", F, ""},
                 {"copyin_oob_bad", F, ""},
                 {"copyout_oob_bad", F, ""},
                 {"bus_ok", PA, "freebsd-bus-space"},
                 {"bus_trust_bad", F, ""},
                 {"log_ok", PA, "freebsd-log"},
                 {"panic_bad", F, ""}});
}

TEST_CASE("pir kernel: Linux W1 specs (driver data, socket lock, netlink payload, ceph room, Ethernet address)") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_linux_w1.c", "linux", false,
                {{"finish_wait_ok", PA, "linux-g2-finish_wait"},
                 {"prepare_to_wait_event_ok", PA, "linux-g2-prepare_to_wait_event"},
                 {"wait_for_completion_timeout_ok", PA, "linux-g3-wait_for_completion_timeout"},
                 {"wait_for_completion_timeout_bad", F, ""},
                 {"i2c_set_clientdata_ok", PA, "linux-g4-i2c_set_clientdata"},
                 {"pci_set_drvdata_ok", PA, "linux-g4-pci_set_drvdata"},
                 {"lock_sock_ok", PA, "linux-g4-lock_sock"},
                 {"nla_get_u8_ok", PA, "linux-g6-nla_get_u8"},
                 {"nla_get_u8_bad", F, ""},
                 {"ceph_has_room_ok", PA, "linux-g6-ceph_has_room"},
                 {"ceph_has_room_bad", F, ""},
                 {"is_valid_ether_addr_ok", PA, "linux-g6-is_valid_ether_addr"},
                 {"is_valid_ether_addr_bad", F, ""},
                 {"complete_ok", PA, "linux-w1-complete"},
                 {"complete_bad", F, ""},
                 {"wait_for_completion_ok", PA, "linux-w1-wait_for_completion"},
                 {"wait_for_completion_bad", F, ""},
                 {"__wake_up_ok", PA, "linux-w1-__wake_up"},
                 {"__wake_up_bad", F, ""},
                 {"nla_len_ok", PA, "linux-w1-nla_len"},
                 {"nla_len_bad", F, ""},
                 {"nla_data_ok", PA, "linux-w1-nla_data"},
                 {"nla_data_bad", F, ""},
                 {"nla_get_u32_ok", PA, "linux-w1-nla_get_u32"},
                 {"nla_get_u32_bad", F, ""},
                 {"nla_total_size_ok", PA, "linux-w1-nla_total_size"},
                 {"nla_total_size_bad", F, ""},
                 {"ether_addr_equal_ok", PA, "linux-w1-ether_addr_equal"},
                 {"ether_addr_equal_bad", F, ""},
                 {"ether_addr_copy_ok", PA, "linux-w1-ether_addr_copy"},
                 {"ether_addr_copy_bad", F, ""},
                 {"dma_mapping_error_ok", PA, "linux-w1-dma_mapping_error"},
                 {"dma_mapping_error_bad", F, ""}});
}

TEST_CASE("pir kernel: FreeBSD W1 specs (fueword, suword, subyte: user word and byte access)") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_freebsd_w1.c", "freebsd", false,
                {{"fueword_ok", PA, "freebsd-f3-fueword"},
                 {"suword_ok", PA, "freebsd-f2-suword"},
                 {"suword_bad", F, ""},
                 {"subyte_ok", PA, "freebsd-f2-subyte"},
                 {"subyte_bad", F, ""},
                 {"suword32_ok", PA, "freebsd-f1-suword32"},
                 {"suword32_bad", F, ""},
                 {"fueword32_ok", PA, "freebsd-f1-fueword32"},
                 {"fueword32_bad", F, ""},
                 {"critical_enter_ok", PA, "freebsd-f1-critical_enter"},
                 {"critical_exit_ok", PA, "freebsd-f1-critical_exit"},
                 {"fubyte_ok", PA, "freebsd-w1-fubyte"},
                 {"fubyte_bad", F, ""},
                 {"casueword32_ok", PA, "freebsd-w1-casueword32"},
                 {"casueword32_bad", F, ""},
                 {"__sx_xlock_ok", PA, "freebsd-w1-sx_xlock_inline"},
                 {"__sx_xlock_bad", F, ""},
                 {"__sx_xunlock_ok", PA, "freebsd-w1-sx_xunlock_inline"},
                 {"__sx_xunlock_bad", F, ""},
                 {"fillw_ok", PA, "freebsd-w1-fillw"},
                 {"fillw_bad", F, ""},
                 {"calculate_crc32c_ok", PA, "freebsd-w1-calculate_crc32c"},
                 {"calculate_crc32c_bad", F, ""}});
}

TEST_CASE("pir kernel: inline asm, atomics, barriers and volatile are named assumptions") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_asm.c", "linux", false,
                {{"asm_out_ok", PA, "asm-havoc: \"=r,"},
                 {"asm_out_bad", F, ""},
                 {"asm_clobber_ok", PA, "memory clobber"},
                 {"asm_clobber_bad", F, ""},
                 {"asm_nomem_ok", PA, "asm-havoc"},
                 {"asm_indirect_out_bad", F, ""},
                 {"asm_ud2_bad", F, ""},
                 {"asm_ud2_ok", PA, "asm-havoc"},
                 {"atomic_add_ok", PA, "atomics-sequential: atomicrmw add"},
                 {"atomic_sub_bad", F, ""},
                 {"atomic_load_store_bad", F, ""},
                 {"cmpxchg_ok", PA, "atomics-sequential: cmpxchg"},
                 {"cmpxchg_fail_bad", F, ""},
                 {"cmpxchg_weak_bad", F, ""},
                 {"xchg_ok", PA, "atomicrmw xchg"},
                 {"fence_ok", PA, "atomics-sequential: fence"},
                 {"volatile_ok", PA, "volatile-access"},
                 {"volatile_bad", F, ""},
                 {"asm_goto_ok", PA, "asm-goto"},
                 {"asm_goto_bad", F, ""}});
}

TEST_CASE("pir kernel: a call without a spec is UNENCODED; --assume-unknown-calls is the opt-in and shows in the verdict") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* NH = "NEEDS-HARNESS";
    // default and kernel mode alone: unchanged, the call stops the function
    check_table("kernel_unknown.c", "", false,
                {{"unk_result_ok", NH, ""}, {"unk_result_bad", NH, ""}, {"unk_pointer_arg_ok", NH, ""}});
    check_table("kernel_unknown.c", "linux", false,
                {{"unk_result_ok", NH, ""}, {"unk_result_bad", NH, ""}, {"unk_ptr_result_ok", NH, ""}});
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_unknown.c", "linux", true,
                {{"unk_result_ok", PA, "unknown-call: @e1000e_poll_hw"},
                 {"unk_result_bad", F, ""},
                 {"unk_nopointer_keeps_locals_ok", PA, "unknown-call"},
                 {"unk_pointer_arg_ok", PA, "unknown-call: @e1000e_fill"},
                 {"unk_pointer_arg_bad", F, ""},
                 {"unk_literal_arg_ok", PA, "unknown-call: @e1000e_name"},
                 {"unk_ptr_result_ok", PA, "unknown-call: @e1000e_get_buf"},
                 {"unk_ptr_result_nocheck_bad", F, ""}});
    // the opt-in does not need the kernel specs
    check_table("kernel_unknown.c", "", true,
                {{"unk_result_ok", PA, "unknown-call: @e1000e_poll_hw"}, {"unk_result_bad", F, ""}});
}

TEST_CASE("pir kernel: without --kernel-api asm, atomics and kernel calls stay UNENCODED (the default is unchanged)") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* NH = "NEEDS-HARNESS";
    check_table("kernel_asm.c", "", false,
                {{"asm_out_ok", NH, ""}, {"asm_clobber_ok", NH, ""}, {"atomic_add_ok", NH, ""},
                 {"cmpxchg_ok", NH, ""}, {"fence_ok", NH, ""}, {"asm_goto_ok", NH, ""}});
    check_table("kernel_linux.c", "", false,
                {{"alloc_ok", NH, ""}, {"lock_ok", NH, ""}, {"mmio_ok", NH, ""}, {"container_of_ok", "PROVED", ""}});
}
