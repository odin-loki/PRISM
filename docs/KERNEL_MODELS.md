# Kernel API specs, inline asm and atomics (requirements R2 and R3)

Status: implemented, opt-in, reviewed against the kernel headers only (see "What was checked"). Requirements:
`docs/feedback/DRIVER-REQUIREMENTS.md` R2 and R3, plan in `docs/OS_PROVING_DRIVER_PLAN.md` (Stage 2 and 3).
Nothing here is on by default, nothing here is a proof step: every use is an **assumption** that shows in the verdict.

## Switches

| flag | effect |
|---|---|
| `--kernel-api linux` / `--kernel-api freebsd` | link the specs of that kernel (`src/prism/pir/models/kernel/linux_*.cpp`, `freebsd_*.cpp`) for each function the unit declares but does not define; model inline asm, asm goto, atomics, barriers (R3); do not use the libc `printf` format checks and libm names for kernel calls |
| `--assume-unknown-calls` | a call without a spec havocs its result and the objects its pointer arguments point to (assumption listed). Opt-in, independent of `--kernel-api` |

Without either flag the behaviour is exactly as before: `UNENCODED: call @f`, `UNENCODED: inline assembly`,
`UNENCODED: atomic or barrier instruction`, `UNENCODED: asm goto`, all `NEEDS-HARNESS`.
A proof that used a spec, an asm model, an atomic model or the unknown-call opt-in is **PROVED-ASSUMING**, never PROVED,
and `extra.assumptions` names each one. Functions that contain a kernel-mode construct are outside the Lean-proved
fragment (the exporter writes `L unsupported kernel-mode model ...`; R7).

## How a spec is written

Same mechanism as the libc models (`docs/PIR.md` "Library models"): a C++ file with `extern "C"` definitions, lowered by
clang and linked into the unit for each declared but undefined symbol; the model body is checked like user code, so its
`requires:` preconditions are enforced by the memory model (a spec that reads `n` bytes of a buffer fails the *caller*
when the buffer is smaller). Every spec has a comment block with (1) the contract on a reference semantics, (2) the
failure paths, (3) the assumptions, (4) the source (kernel header or manual page), and calls `ASSUME("<family>: <text>")`
(`__prism_assumption`), which records the text in the verdict. Rules:

- A spec is never stronger than the real function. Where the real result is not known the spec returns an arbitrary
  value of the type (`__VERIFIER_nondet_*`), and where the real function may fail the spec may fail.
- A spec that is stronger on purpose says so: FreeBSD `malloc(M_WAITOK)` never returns NULL (malloc(9) states it), and
  `regmap_read` / `regmap_write` return `<= 0` (regmap.c documents it). Both are in the assumption text.
- Kernel specs take precedence over the libc models of the same name (`malloc`, `free`, `printf`, `snprintf`, `panic`
  differ between libc and FreeBSD's kernel), and a libc model's own internal calls never resolve to a kernel spec.
- ERR_PTR / IS_ERR / PTR_ERR are **not** specified: they are inline `ptrtoint` / `inttoptr` arithmetic, which PIR does not
  encode (`UNENCODED: ptrtoint`). Supporting them needs translator support, not a spec (see "Not done").

## Specs by family (150: Linux 115, FreeBSD 35)

Sources are the Linux 6.18.60 tree used for the OS-Proving clang pass (`include/linux`, `lib`, `mm`, `kernel`) and the
FreeBSD 15.0 (HardenedBSD) tree (`sys/sys`, `sys/kern`) and the manual pages named. Signatures were compared with those
headers; the arity of a call must match the spec's (a mismatch, for example `__kmalloc_node_noprof` with
`CONFIG_SLAB_BUCKETS`, stops at `UNENCODED: call @f arity`, never at a wrong proof).

| family | specs (Linux) | contract | assumption name | source |
|---|---|---|---|---|
| allocation, 22 | `__kmalloc_noprof`, `__kmalloc_large_noprof`, `__kmalloc_cache_noprof`, `__kmalloc_node_noprof`, `__kmalloc_node_track_caller_noprof`, `__kvmalloc_node_noprof`, `vmalloc_noprof`, `vzalloc_noprof`, `vmalloc_user_noprof`, `vmalloc_node_noprof`, `vzalloc_node_noprof`, `__vmalloc_noprof`, `devm_kmalloc`, `kmemdup_noprof`, `kstrdup`, `krealloc_node_align_noprof`, `kmalloc_size_roundup`, free: `kfree`, `kfree_sensitive`, `kvfree`, `vfree`, `devm_kfree` | NULL (any call, any flags) or a fresh object of `size` bytes, zero when `flags & 0x100` (`__GFP_ZERO`), else uninitialised; size 0 gives a size-0 object (ZERO_SIZE_PTR); free of NULL / size-0 is a no-op, else double free, free of a non-allocation and use after free are reported; krealloc keeps `min(old, new)` bytes and always moves; kmemdup reads `len` bytes of src, kstrdup needs a string | `linux-alloc`, `linux-free`, `linux-krealloc`, `linux-devm` | `include/linux/slab.h`, `string.h`, `vmalloc.h`, `gfp_types.h`, `mm/slub.c`, `mm/util.c` |
| locking, 17 | `mutex_lock_nested`, `mutex_unlock`, `__mutex_init`, `mutex_trylock`, `mutex_lock_interruptible_nested`, `_mutex_lock_killable`, `_raw_spin_lock`, `_raw_spin_unlock`, `_raw_spin_lock_irq`, `_raw_spin_unlock_irq`, `_raw_spin_lock_bh`, `_raw_spin_unlock_bh`, `_raw_spin_lock_irqsave`, `_raw_spin_lock_irqsave_nested`, `_raw_spin_unlock_irqrestore`, `_raw_spin_trylock`, `__raw_spin_lock_init` | no effect on data memory; the lock pointer must be a valid object (1-byte read checked: NULL / freed / wild is reported); trylock returns 0 or 1, interruptible and killable return 0 or -EINTR, irqsave returns an arbitrary flags word | `linux-lock` | `include/linux/mutex.h`, `spinlock_api_smp.h`, `spinlock.h`, `kernel/locking/` |
| RCU, lockdep, might_sleep, 10 | `__rcu_read_lock`, `__rcu_read_unlock`, `synchronize_rcu`, `debug_lockdep_rcu_enabled`, `lockdep_rcu_suspicious`, `__might_sleep`, `rcu_is_watching`, `rcu_read_lock_held`, `rcu_read_lock_any_held`, `rcu_read_lock_bh_held` | no effect; the queries return an arbitrary value | `linux-rcu`, `linux-might-sleep`, `linux-lockdep` | `include/linux/rcupdate.h`, `rcutree.h` |
| user access, 7 | `_copy_from_user`, `copy_from_user`, `raw_copy_from_user`, `_copy_to_user`, `copy_to_user`, `raw_copy_to_user`, `strncpy_from_user` | returns the bytes not copied in `[0, n]`; the kernel buffer of a `from_user` is written (n bytes, must be valid) with arbitrary bytes; the buffer of a `to_user` is read (n bytes, must be valid); `strncpy_from_user` returns -EFAULT or `[0, count]`; user memory is not an object | `linux-uaccess` | `include/linux/uaccess.h`, `lib/usercopy.c`, `arch/x86/include/asm/uaccess.h` |
| MMIO, 16 | `ioread8/16/32`, `iowrite8/16/32`, `readb/w/l/q`, `writeb/w/l/q`, `ioremap`, `iounmap` | a read returns an arbitrary value of its width, a write has no effect on program memory; `ioremap` returns NULL or a fresh object of `size` arbitrary bytes | `linux-mmio` (the register space is the environment; no device model) | `include/asm-generic/iomap.h`, `io.h`, `lib/iomap.c`, `arch/x86/include/asm/io.h` |
| regmap, 2 | `regmap_read`, `regmap_write` | return 0 or a negative errno; a successful read writes an arbitrary value to `*val` | `linux-regmap` | `include/linux/regmap.h`, `drivers/base/regmap/regmap.c` |
| delays and time, 9 | `msleep`, `msleep_interruptible`, `usleep_range_state`, `__udelay`, `__const_udelay`, `__ndelay`, `__delay`, `__msecs_to_jiffies`, `ktime_get` | no effect on memory; the time values are arbitrary | `linux-delay`, `linux-lockdep`, `linux-time` | `include/linux/delay.h`, `jiffies.h`, `timekeeping.h` |
| logging, 19 | `_printk`, `_dev_err/warn/info/notice/crit/alert/emerg`, `_dev_printk`, `__dynamic_dev_dbg`, `__dynamic_netdev_dbg`, `__dynamic_pr_debug`, `netdev_err/warn/info`, `__warn_printk`, `warn_slowpath_fmt`, `___ratelimit`, `dev_err_probe` | no effect on memory, arguments evaluated, `dev_err_probe` returns its `err`; `_printk` / `___ratelimit` results arbitrary | `linux-log`, `linux-warn` | `include/linux/printk.h`, `dev_printk.h`, `dynamic_debug.h`, `drivers/base/core.c` |
| formatting, 4 | `scnprintf`, `snprintf`, `sprintf`, `sized_strscpy` | scnprintf / snprintf write at most `size` bytes (buffer checked) and return a length in range; **sprintf stops the path (`__prism_incomplete`)** (the kernel's `%p` extensions make its length unbounded); sized_strscpy copies exactly as `lib/string.c` | `linux-format`, `linux-string` | `include/linux/sprintf.h`, `lib/string.c` |
| BUG / WARN, 5 | `panic`, `____wrong_branch_error`, `__bad_udelay`, `__bad_ndelay`, `__bad_size_call_parameter` | do not return: reaching them is `FUNC-CONTRACT` (BUG on x86 is `ud2` followed by unreachable: reported as `CXX-UNREACHABLE` through the asm model); WARN (`__warn_printk`, `warn_slowpath_fmt`) has no effect | `linux-bug`, `linux-warn` | `include/linux/panic.h`, `asm-generic/bug.h`, `delay.h`, `jump_label.h` |
| list, refcount debug, 3 | `__list_add_valid_or_report`, `__list_del_entry_valid_or_report`, `refcount_warn_saturate` | arbitrary result, no effect | `linux-list-debug`, `linux-warn` | `lib/list_debug.c`, `include/linux/refcount.h` |
| container_of, offsetof, list_*, IS_ERR | (no spec) | plain pointer arithmetic and inline functions: PIR analyses them as written. `container_of` on an object proves (`testdata/pir/kernel_linux.c`); `IS_ERR` / `PTR_ERR` / `ERR_PTR` are `ptrtoint` / `inttoptr` and stay UNENCODED | | |

| family | specs (FreeBSD) | contract | assumption name | source |
|---|---|---|---|---|
| allocation, 4 | `malloc`, `mallocarray`, `free`, `zfree` | `M_NOWAIT` (0x1) may return NULL, `M_WAITOK` (0x2) never returns NULL, `M_ZERO` (0x100) zero-fills; `mallocarray` overflow is `FUNC-CONTRACT` (the kernel panics); free of NULL is a no-op, else double free is reported | `freebsd-alloc`, `freebsd-free` | `sys/sys/malloc.h`, `sys/kern/kern_malloc.c`, malloc(9) |
| locking, 13 | `__mtx_lock_flags`, `__mtx_unlock_flags`, `__mtx_lock_sleep`, `__mtx_unlock_sleep`, `_mtx_init`, `_mtx_destroy`, `_mtx_trylock_flags_`, `sx_init_flags`, `sx_destroy`, `_sx_xlock`, `_sx_slock`, `_sx_xunlock`, `_sx_sunlock` | no effect on data memory; lock word must be valid memory (8-byte read); trylock 0 or 1; `_sx_xlock` / `_sx_slock` return an arbitrary int | `freebsd-lock` | `sys/sys/mutex.h`, `sx.h`, `sys/kern/kern_mutex.c`, `kern_sx.c`, mutex(9), sx(9) |
| user access, 3 | `copyin`, `copyout`, `copyinstr` | 0 or EFAULT (14) (copyinstr also ENAMETOOLONG 63); the kernel buffer of copyin / copyinstr is written with arbitrary bytes, the one of copyout is read (ranges checked); `*lencopied` arbitrary in `[0, len]` | `freebsd-copy` | `sys/sys/systm.h`, copy(9) |
| bus_space, 8 | `bus_space_read_1/2/4/8`, `bus_space_write_1/2/4/8` | a read returns an arbitrary value of its width, a write has no effect on program memory | `freebsd-bus-space` | bus_space(9), `sys/x86/include/bus.h` |
| delay, log, panic, 7 | `DELAY`, `pause_sbt`, `wakeup`, `printf`, `device_printf`, `if_printf`, `panic` | no effect on memory; `panic` does not return (`FUNC-CONTRACT`) | `freebsd-log`, `freebsd-panic` | `sys/sys/systm.h`, `sys/sys/bus.h`, printf(9), panic(9) |

## Inline assembly, asm goto, atomics, barriers (R3)

| construct | model | assumption name |
|---|---|---|
| `call asm` without a `// prism: asm ensures` contract | each direct output is arbitrary (int, pointer, or a two-field pair); indirect outputs (`=*m`, `+*m`) and, for `~{memory}` (a memory clobber), **every object's contents** become arbitrary (`Stmt::MemHavoc`, the PIR `havocmem` statement: values and tags arbitrary, an initialised byte stays initialised, object sizes, liveness and kinds unchanged; const and dead objects included); indirect outputs: the bytes of the object the pointer points into, from the pointer to the end of the object, become arbitrary (never a violation: NULL, freed, read-only and FILE objects are skipped); registers and flags are not modelled; without a memory clobber, memory no indirect output writes is assumed unchanged, and the assumption says so; `volatile` asm is translated where it stands, once. A `// prism: asm ensures` contract (existing) still wins. | `asm-havoc: "<constraint string>" (line N): ...` |
| asm goto (`callbr`) with one label | the asm havoc, then a nondeterministic jump to the fallthrough or to the label (static keys, jump labels). More than one label: `UNENCODED` | `asm-goto: "<constraints>" (line N)` |
| `ud2` (x86 `BUG()`) | an ordinary asm with no output; the `unreachable` that follows is the reported failure (`CXX-UNREACHABLE`) | `asm-havoc` |
| `atomicrmw` (xchg, add, sub, and, nand, or, xor, max, min, umax, umin), `cmpxchg`, `load atomic`, `store atomic` | lowered by the IR parser into the plain load / operation / store they are in a sequential execution; `cmpxchg weak` may also fail when the values are equal; the memory order is recorded in the assumption text (seq_cst and monotonic as before) | `atomics-sequential: <instruction> (<orders>)` |
| `fence` | no effect | `atomics-sequential: fence (<order>)` |
| `volatile` load and store (`READ_ONCE`, `WRITE_ONCE`, MMIO through a pointer) | as before: ordinary access in program order, now named | `volatile-access` |

Not modelled (`UNENCODED`, unchanged): `atomicrmw fadd/fsub/uinc_wrap/udec_wrap`, `callbr` with more than one label,
asm returning a struct of more than two fields, vector or floating-point asm outputs.
x86 `readl` / `writel` / `copy_from_user` are inline asm: they go through `asm-havoc`, not through the MMIO or
user-access specs (a `memory` clobber on pointer operands havocs the buffer a copy writes).

## Policy for a call without a spec

1. Default: `UNENCODED: call @f`, `NEEDS-HARNESS`. Unchanged.
2. `--assume-unknown-calls`: the result is arbitrary (a pointer result is NULL or a fresh object of 1 MiB of arbitrary
   bytes, never an ERR_PTR value), the object each pointer argument points into is havocked from that pointer to its end,
   the callee is assumed to return, to free nothing, to keep nothing, to call nothing back and to change no other
   memory. The assumption `unknown-call: @f has no spec ...` is in the verdict; the result is PROVED-ASSUMING at best.
   A defect found with it is still reported FAILED (the assumed behaviour is a *possible* behaviour: an unknown function
   that returns 0 does reach the division).

## Soundness notes

- *Too strong* would be a spec that excludes behaviours of the real function. The specs above only narrow where the
  kernel documents it (M_WAITOK, regmap return range, `copy_*` return in `[0, n]`, strscpy contents). Everywhere else the
  result is arbitrary or the call may fail.
- *Too weak* only loses findings: lock state is not tracked (no deadlock / balance), `to_user` does not check for
  uninitialised source bytes (an infoleak is missed), kmalloc / vmalloc / devm share one allocation kind (kfree of
  vmalloc memory is not reported), a size-0 allocation is freed without a double-free report, devm memory is never
  released.
- The sequential model of atomics, volatile accesses and MMIO is the assumption all of PRISM's per-function checks
  already make: nothing else runs between two accesses of the function. A result that depends on another thread, an
  interrupt or the device between two accesses is outside what these specs can say.
- An asm without a memory clobber is assumed to touch only what its operands name (a proof with `asm-havoc` says so).
  A memory clobber havocs every object (`havocmem`). A `// prism: asm ensures` contract, when given, replaces the asm
  model entirely (the contract is the user's claim).

## What was checked

Signatures and parameter widths were compared with the headers of the Linux 6.18.60 and FreeBSD 15.0 trees named above
(by grep, not by compiling the kernel). The contracts come from the comments in those files and the manual pages; no
spec was reviewed by a second person. **Every spec table row needs Odin's review before it counts for the proof levels
L2 and up** (plan section 3, Stage 2 gate). Tests: `testdata/pir/kernel_linux.c`, `kernel_freebsd.c`, `kernel_asm.c`,
`kernel_unknown.c` (a true and a false variant of each spec; `tests/cpp/test_pir_kernel.cpp`).

### Needs review (specified by name in the requirement, left unspecified here)

`kmem_cache_alloc*` (object size is the cache's), `kfree_const`, `kzalloc` / `kcalloc` (inline wrappers: analysed as
written), `seq_printf` (writes a seq_file of unknown layout), `sysfs_emit` (PAGE_SIZE-buffer contract),
`refcount_dec_and_mutex_lock`, `platform_driver_*`, `devm_request_*irq`, `complete` / `wait_for_completion`,
`of_node_put` / `put_device` (foreign refcount, may free), workqueue and DMA APIs (`dma_alloc_coherent`, `dma_map_*`),
clocks, `devm_platform_ioremap_resource` (returns an ERR_PTR), `get_user` / `put_user` (x86: `__get_user_N` with a
register calling convention, not a C function), FreeBSD `realloc`, `malloc_domainset`, `uma_zalloc_arg`, rw locks,
`cv_*`, `device_get_softc`, `bus_dma_*`, `callout_*`, `sysctl_*`, `m_freem`. The `__kmalloc_node_noprof` /
`__kvmalloc_node_noprof` prototypes follow the `CONFIG_SLAB_BUCKETS=n` form.

## Evidence on the IR census

`tools/pir/kernel_census_coverage.sh` reads the OS-Proving IR census (`data/clang/census`, `data/clang/pilot`;
approximate classes, a candidate is not a verdict) and counts, per function, whether every unknown external callee has a
spec. `tools/pir/kernel_callee_top.sh` lists the top unresolved callees from `data/clang/*/calls.tsv.gz`.

| sample | encodable candidates before | unknown-call functions fully covered by specs | inline-asm functions encodable (asm model, upper bound) | candidates after specs + asm model | with `--assume-unknown-calls` as well |
|---|---|---|---|---|---|
| Linux, 2,500 functions | 840 (33.6%) | 178 of 1,076 (16.5%) | 158 of 446 (35.4%) | 1,176 (47.0%) | 2,362 (94.5%) |
| pilot e1000e + em, 1,000 functions | 378 (37.8%) | 24 of 491 (4.9%) | 47 of 119 (39.5%) | 449 (44.9%) | 988 (98.8%) |

The specs cover 30.4% (Linux) and 12.8% (pilot) of the distinct unknown callee names, but the unknown calls that block a
function are mostly driver-local or subsystem helpers (`e1000e_*`, `regmap_*`, `platform_driver_*`, `i2c_*`, `snd_soc_*`,
`drm_*`), which are not kernel-API specs; the specs lift the share by about 13 points (Linux) and 7 points (pilot), and
the rest is `--assume-unknown-calls` or per-subsystem spec tables. Those functions that the census counts as encodable
before may also use constructs PRISM refuses (atomics, `ptrtoint` in `IS_ERR`): the numbers are an upper bound.

## Not done

- ERR_PTR / IS_ERR / PTR_ERR and MMIO through an integer handle (FreeBSD x86 `bus_space_read_*` inline) need `ptrtoint` /
  `inttoptr` support in the translator and the Lean mirror; they stay UNENCODED.
- No Lean mirror or proof for the kernel-mode constructs (the exporter marks them unsupported); R7 reports them.
- A device model (R8): reads return arbitrary values; nothing constrains them.
- One real kernel unit was run: Linux `drivers/net/ethernet/intel/e1000e/82571.c` with `--compile-commands` (regenerated
  `modules_prepare` headers): it compiles and all 39 functions are `NEEDS-HARNESS` with and without `--kernel-api` /
  `--assume-unknown-calls`, because every one takes a `struct e1000_hw *` (Law 6, requirement R4: harness generation).
  No count on real kernel functions without pointer parameters yet; the tests use the kernel's declarations by hand. (The
  census numbers above are from the OS-Proving IR census, not from PRISM runs.)
