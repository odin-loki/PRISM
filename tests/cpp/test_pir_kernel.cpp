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
                 {"asm_clobber_ok", PA, "memory clobber"},  // y never escapes: the asm cannot write it
                 {"asm_clobber_bad", F, ""},
                 {"asm_memclobber_ok", PA, "memory clobber: every object"},  // true variant: a memory-independent property
                 {"asm_memclobber_bad", F, ""},  // false variant: x escapes to a global, is no operand, is havocked
                 {"asm_memclobber_global_bad", F, ""},
                 {"asm_goto_memclobber_bad", F, ""},
                 {"asm_nomem_escaped_ok", PA, "asm-exact"},  // no memory clobber: escaped x unchanged (nop modelled exactly)
                 {"asm_nomem_ok", PA, "asm-exact"},  // nop: modelled exactly (translate_asm.inc)
                 {"asm_indirect_out_bad", F, ""},
                 {"asm_ud2_bad", F, ""},
                 {"asm_ud2_ok", PA, "asm-privileged"},  // ud2 is privileged: outputs and memory havocked
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
                 {"asm_goto_bad", F, ""},
                 {"asm_goto2_ok", PA, "asm-goto"},
                 {"asm_goto2_bad", F, ""}});
}

TEST_CASE("pir kernel: ptrtoint/inttoptr round-trip is exact; other integer-to-pointer casts are the named assumption inttoptr-opaque") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_ptrint.c", "linux", false,
                {{"roundtrip_ok", "PROVED", ""},
                 {"opaque_bad", F, ""},
                 {"errptr_ok", PA, "inttoptr-opaque"},
                 {"errptr_deref_ok", PA, "inttoptr-opaque"},
                 {"errptr_bad", F, ""}});
}

TEST_CASE("pir kernel: struct bitfield loads and stores encode (unsigned, signed sign-extension, division by a bitfield)") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* F = "FAILED";
    check_table("bitfield.c", "", false,
                {{"bf_ok", "PROVED", ""},
                 {"bf_signed_ok", "PROVED", ""},
                 {"bf_div_bad", F, ""}});
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
                 {"cmpxchg_ok", NH, ""}, {"fence_ok", NH, ""}, {"asm_goto_ok", NH, ""}, {"asm_goto2_ok", NH, ""}});
    check_table("kernel_linux.c", "", false,
                {{"alloc_ok", NH, ""}, {"lock_ok", NH, ""}, {"mmio_ok", NH, ""}, {"container_of_ok", "PROVED", ""}});
}

TEST_CASE("pir kernel: Linux W2 group a: string compare, rwsem, net_ratelimit") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    check_table("kernel_linux_w2_a.c", "linux", false,
                {{"strcmp_ok", "PROVED-ASSUMING", "linux-w2-strcmp"},
                 {"strcmp_bad", "FAILED", ""},
                 {"strncmp_ok", "PROVED-ASSUMING", "linux-w2-strncmp"},
                 {"strncmp_bad", "FAILED", ""},
                 {"memcmp_ok", "PROVED-ASSUMING", "linux-w2-memcmp"},
                 {"memcmp_bad", "FAILED", ""},
                 {"net_ratelimit_ok", "PROVED-ASSUMING", "linux-w2-net_ratelimit"},
                 {"net_ratelimit_bad", "FAILED", ""},
                 {"up_write_ok", "PROVED-ASSUMING", "linux-w2-up_write"},
                 {"up_write_bad", "FAILED", ""},
                 {"up_read_ok", "PROVED-ASSUMING", "linux-w2-up_read"},
                 {"up_read_bad", "FAILED", ""}});
}

TEST_CASE("pir kernel: Linux W2 group b: timers, PCI config, mutex, platform IRQ, schedule_timeout") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    check_table("kernel_linux_w2_b.c", "linux", false,
                {{"mod_timer_ok", "PROVED-ASSUMING", "linux-w2-mod_timer"},
                 {"mod_timer_bad", "FAILED", ""},
                 {"pci_read_config_dword_ok", "PROVED-ASSUMING", "linux-w2-pci_read_config_dword"},
                 {"pci_read_config_dword_bad", "FAILED", ""},
                 {"mutex_destroy_ok", "PROVED-ASSUMING", "linux-w2-mutex_destroy"},
                 {"mutex_destroy_bad", "FAILED", ""},
                 {"platform_get_irq_ok", "PROVED-ASSUMING", "linux-w2-platform_get_irq"},
                 {"platform_get_irq_bad", "FAILED", ""},
                 {"down_write_ok", "PROVED-ASSUMING", "linux-w2-down_write"},
                 {"down_write_bad", "FAILED", ""},
                 {"schedule_timeout_ok", "PROVED-ASSUMING", "linux-w2-schedule_timeout"},
                 {"schedule_timeout_bad", "FAILED", ""}});
}

TEST_CASE("pir kernel: Linux W2 group c: rwsem read, PCI config write, runtime PM, timer delete") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    check_table("kernel_linux_w2_c.c", "linux", false,
                {{"down_read_ok", "PROVED-ASSUMING", "linux-w2-down_read"},
                 {"down_read_bad", "FAILED", ""},
                 {"pci_write_config_dword_ok", "PROVED-ASSUMING", "linux-w2-pci_write_config_dword"},
                 {"pci_write_config_dword_bad", "FAILED", ""},
                 {"pm_runtime_enable_ok", "PROVED-ASSUMING", "linux-w2-pm_runtime_enable"},
                 {"pm_runtime_enable_bad", "FAILED", ""},
                 {"pci_disable_device_ok", "PROVED-ASSUMING", "linux-w2-pci_disable_device"},
                 {"pci_disable_device_bad", "FAILED", ""},
                 {"timer_delete_sync_ok", "PROVED-ASSUMING", "linux-w2-timer_delete_sync"},
                 {"timer_delete_sync_bad", "FAILED", ""}});
}

TEST_CASE("pir kernel: Linux W2 group d: rtnl lock, reset, jiffies, PCI config word, ACPI node, carrier") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    check_table("kernel_linux_w2_d.c", "linux", false,
                {{"rtnl_lock_ok", "PROVED-ASSUMING", "linux-w2-rtnl_lock"},
                 {"rtnl_lock_bad", "FAILED", ""},
                 {"reset_control_assert_ok", "PROVED-ASSUMING", "linux-w2-reset_control_assert"},
                 {"reset_control_assert_bad", "FAILED", ""},
                 {"jiffies_to_msecs_ok", "PROVED-ASSUMING", "linux-w2-jiffies_to_msecs"},
                 {"jiffies_to_msecs_bad", "FAILED", ""},
                 {"pci_read_config_word_ok", "PROVED-ASSUMING", "linux-w2-pci_read_config_word"},
                 {"pci_read_config_word_bad", "FAILED", ""},
                 {"is_acpi_device_node_ok", "PROVED-ASSUMING", "linux-w2-is_acpi_device_node"},
                 {"is_acpi_device_node_bad", "FAILED", ""},
                 {"netif_carrier_off_ok", "PROVED-ASSUMING", "linux-w2-netif_carrier_off"},
                 {"netif_carrier_off_bad", "FAILED", ""}});
}

TEST_CASE("pir kernel: FreeBSD W2 group a: libkern string functions") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    check_table("kernel_freebsd_w2_a.c", "freebsd", false,
                {{"strlcpy_ok", "PROVED-ASSUMING", "freebsd-w2-strlcpy"},
                 {"strlcpy_bad", "FAILED", ""},
                 {"strlcat_ok", "PROVED-ASSUMING", "freebsd-w2-strlcat"},
                 {"strlcat_bad", "FAILED", ""}});
}

TEST_CASE("pir kernel: FreeBSD W2 group b: strncpy") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    check_table("kernel_freebsd_w2_b.c", "freebsd", false,
                {{"strncpy_ok", "PROVED-ASSUMING", "freebsd-w2-strncpy"},
                 {"strncpy_bad", "FAILED", ""}});
}

TEST_CASE("pir kernel: lock balance, double lock, unlock of an unheld lock, sleeping in atomic context (ghost lock state)") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_lock_ctx.c", "linux", false,
                {{"lock_relock_ok", PA, "linux-lock"},
                 {"lock_double_bad", F, ""},
                 {"spin_double_bad", F, ""},
                 {"spin_nested_ok", PA, "linux-lock"},
                 {"unlock_unheld_bad", F, ""},
                 {"sleep_in_spin_bad", F, ""},
                 {"sleep_after_spin_ok", PA, "linux-lock"},
                 {"might_sleep_in_spin_bad", F, ""},
                 {"might_sleep_ok", PA, "linux-might-sleep"},
                 {"held_on_return_bad", F, ""},
                 {"held_on_return_ok", PA, "linux-lock"}});
    // each bad variant reports its own bug class, not another one
    auto got = run("kernel_lock_ctx.c", "linux", false);
    const std::map<std::string, std::string> want = {
        {"lock_double_bad", "LOCK-DOUBLE"},         {"spin_double_bad", "LOCK-DOUBLE"},
        {"unlock_unheld_bad", "LOCK-UNHELD-UNLOCK"}, {"sleep_in_spin_bad", "SLEEP-IN-ATOMIC"},
        {"held_on_return_bad", "LOCK-HELD-ON-RETURN"},
        {"might_sleep_in_spin_bad", "SLEEP-IN-ATOMIC"}};
    for (auto& [fn, cls] : want) {
        auto it = got.find(fn);
        REQUIRE(it != got.end());
        CHECK(it->second.cls == cls);
    }
}

TEST_CASE("pir kernel: lock contracts (__prism_lock_entry / __prism_lock_exit): a returned-held lock is not reported") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_lock_contract.c", "linux", false,
                {{"take_declared_ok", PA, "linux-lock-contract"},
                 {"take_undeclared_bad", F, ""},
                 {"take_exit_missing_bad", F, ""},
                 {"release_entry_ok", PA, "linux-lock-contract"},
                 {"entry_held_on_return_bad", F, ""},
                 {"pass_through_ok", PA, "linux-lock-contract"},
                 {"entry_spin_sleep_bad", F, ""},
                 {"caller_ok", PA, "linux-lock-overflow"}});
    auto got = run("kernel_lock_contract.c", "linux", false);
    const std::map<std::string, std::string> want = {
        {"take_undeclared_bad", "LOCK-HELD-ON-RETURN"},
        {"take_exit_missing_bad", "LOCK-EXIT-MISSING"},
        {"entry_held_on_return_bad", "LOCK-HELD-ON-RETURN"},
        {"entry_spin_sleep_bad", "SLEEP-IN-ATOMIC"}};
    for (auto& [fn, cls] : want) {
        auto it = got.find(fn);
        REQUIRE(it != got.end());
        CHECK(it->second.cls == cls);
    }
}

TEST_CASE("pir kernel: preempt and bh depth, sleeping in atomic context (SLEEP-IN-ATOMIC), GFP_KERNEL in a spinlock") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_lock_preempt.c", "linux", false,
                {{"preempt_sleep_bad", F, ""},
                 {"preempt_sleep_ok", PA, "linux-preempt"},
                 {"preempt_unbalanced_bad", F, ""},
                 {"preempt_held_on_return_bad", F, ""},
                 {"bh_sleep_bad", F, ""},
                 {"bh_sleep_ok", PA, "linux-preempt"},
                 {"kmalloc_in_spin_bad", F, ""},
                 {"kmalloc_atomic_ok", PA, "linux-alloc"},
                 {"kmalloc_kernel_ok", PA, "linux-alloc"}});
    auto got = run("kernel_lock_preempt.c", "linux", false);
    const std::map<std::string, std::string> want = {
        {"preempt_sleep_bad", "SLEEP-IN-ATOMIC"},
        {"preempt_unbalanced_bad", "PREEMPT-UNBALANCED"},
        {"preempt_held_on_return_bad", "PREEMPT-UNBALANCED"},
        {"bh_sleep_bad", "SLEEP-IN-ATOMIC"},
        {"kmalloc_in_spin_bad", "SLEEP-IN-ATOMIC"}};
    for (auto& [fn, cls] : want) {
        auto it = got.find(fn);
        REQUIRE(it != got.end());
        CHECK(it->second.cls == cls);
    }
}

TEST_CASE("pir kernel: RCU read-side nesting (RCU-UNBALANCED, RCU-HELD-ON-RETURN, RCU-SLEEP)") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const char* PA = "PROVED-ASSUMING";
    const char* F = "FAILED";
    check_table("kernel_lock_rcu.c", "linux", false,
                {{"rcu_balance_ok", PA, "linux-rcu"},
                 {"rcu_unbalanced_bad", F, ""},
                 {"rcu_held_on_return_bad", F, ""},
                 {"rcu_sleep_bad", F, ""},
                 {"rcu_sleep_ok", PA, "linux-rcu"},
                 {"rcu_sync_bad", F, ""},
                 {"rcu_sync_ok", PA, "linux-rcu"}});
    auto got = run("kernel_lock_rcu.c", "linux", false);
    const std::map<std::string, std::string> want = {
        {"rcu_unbalanced_bad", "RCU-UNBALANCED"},
        {"rcu_held_on_return_bad", "RCU-HELD-ON-RETURN"},
        {"rcu_sleep_bad", "RCU-SLEEP"},
        {"rcu_sync_bad", "RCU-SLEEP"}};
    for (auto& [fn, cls] : want) {
        auto it = got.find(fn);
        REQUIRE(it != got.end());
        CHECK(it->second.cls == cls);
    }
}

// --lock-contracts (docs/KERNEL_CONCURRENCY.md section 14): table-driven contracts for code with no markers
namespace {
std::map<std::string, prism::Finding> run_with_table(const std::string& file, const std::string& tsv) {
    auto cfg = prism::default_config();
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    cfg.pir_kernel_api = "linux";
    if (!tsv.empty()) cfg.pir_lock_contracts = (dir / tsv).string();
    std::map<std::string, prism::Finding> m;
    for (auto& f : pp::run_pir({dir / file}, cfg))
        if (f.function) m[*f.function] = f;
    return m;
}
}  // namespace

TEST_CASE("pir kernel: --lock-contracts table: acquire and release halves are contracts, a wrong contract is reported") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    // without the table: the model artefacts (true variant)
    auto none = run_with_table("kernel_lock_table.c", "");
    REQUIRE(none.count("nvm_acquire_ok"));
    REQUIRE(none.count("nvm_release_ok"));
    CHECK(none["nvm_acquire_ok"].cls == "LOCK-HELD-ON-RETURN");
    CHECK(none["nvm_release_ok"].cls == "LOCK-UNHELD-UNLOCK");
    // with the table (false variant): no finding for the two halves, the contracts are assumed
    auto got = run_with_table("kernel_lock_table.c", "kernel_lock_table.tsv");
    REQUIRE(got.count("nvm_acquire_ok"));
    REQUIRE(got.count("nvm_release_ok"));
    CHECK(got["nvm_acquire_ok"].status == prism::laws::PROVED_ASSUMING);
    CHECK(assumptions_of(got["nvm_acquire_ok"]).find("lock-contract-table") != std::string::npos);
    CHECK(got["nvm_release_ok"].status == prism::laws::PROVED_ASSUMING);
    CHECK(assumptions_of(got["nvm_release_ok"]).find("lock-contract-table") != std::string::npos);
    // a wrong table row (acquires, but the function releases first): LOCK-EXIT-MISSING
    REQUIRE(got.count("wrong_contract_bad"));
    CHECK(got["wrong_contract_bad"].status == prism::laws::FAILED);
    CHECK(got["wrong_contract_bad"].cls == "LOCK-EXIT-MISSING");
    // a spin contract (kind spin): the mutex call while the caller's spinlock is held sleeps
    REQUIRE(got.count("spin_entry_sleep_bad"));
    CHECK(got["spin_entry_sleep_bad"].status == prism::laws::FAILED);
    CHECK(got["spin_entry_sleep_bad"].cls == "SLEEP-IN-ATOMIC");
}

TEST_CASE("pir kernel: --lock-contracts TSV parse: header and comments skipped, bad lines are errors") {
    std::string err;
    auto ok = pp::parse_lock_contracts("function\tlock\taction\n# c\nf\tglobal:m\tacquires\tspin\n", &err);
    CHECK(err.empty());
    REQUIRE(ok.size() == 1);
    CHECK(ok[0].function == "f");
    CHECK(ok[0].lock == "global:m");
    CHECK(ok[0].spin);
    auto bad = pp::parse_lock_contracts("f\tm\tacquires\n", &err);
    CHECK(bad.empty());
    CHECK(err.find("line 1") != std::string::npos);
    bad = pp::parse_lock_contracts("f\tglobal:m\ttakes\n", &err);
    CHECK(bad.empty());
    CHECK(err.find("action") != std::string::npos);
    bad = pp::parse_lock_contracts("f\targ:0\trequires-held\tfast\n", &err);
    CHECK(bad.empty());
    CHECK(err.find("kind") != std::string::npos);
}

// LOCK-ORDER-INVERSION (docs/KERNEL_CONCURRENCY.md section 14): a lock-class pair taken in both orders
TEST_CASE("pir kernel: lock order: a pair taken A-then-B and B-then-A is LOCK-ORDER-INVERSION, a consistent order is not") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    auto count_inversions = [&](const std::string& file, std::vector<prism::Finding>& keep) {
        cfg.root = dir;
        cfg.jobs = 2;
        cfg.pir_kernel_api = "linux";
        keep = pp::run_pir({dir / file}, cfg);
        int n = 0;
        for (auto& f : keep)
            if (f.cls == "LOCK-ORDER-INVERSION") ++n;
        return n;
    };
    std::vector<prism::Finding> t, f;
    CHECK(count_inversions("kernel_lock_order_true.c", t) == 1);
    for (auto& x : t)
        if (x.cls == "LOCK-ORDER-INVERSION") {
            CHECK(x.status == prism::laws::FAILED);
            CHECK(x.extra.count("lock_classes"));
            if (x.extra.count("lock_classes")) CHECK(x.extra.at("lock_classes") == "lock_a,lock_b");
            CHECK(x.function.has_value());
        }
    CHECK(count_inversions("kernel_lock_order_false.c", f) == 0);
}
