# What PRISM needs to take on kernel and driver C (requirements from OS-Proving)

Date: 2026-11-16. Source: the OS-Proving project (odin-loki/OS-Proving): 1,135,593 mined Linux, FreeBSD and KDE functions, a clang pass over the Linux and FreeBSD build, and the plan in docs/OS_PROVING_DRIVER_PLAN.md. This is a requirements list, ordered by what blocks first. Figures marked (pilot) come from a 1,000-function sample of two drivers (Linux e1000e, FreeBSD em) and are not population rates.

## 1. Measured on kernel C so far

- Running `prism pir` on a Linux kernel translation unit fails in PRISM's own clang front end (`rwonce.h: expected ')'`), also on a preprocessed copy. Likely cause: the front end compiles with `-std=c17` while the kernel uses GNU C (`typeof`, `_Generic`-based `__unqual_scalar_typeof`, statement expressions, `asm goto`). A fix is in progress (branch with a regression test); this is requirement R1.
- IR construct census (pilot, 1,000 functions): 378 encodable candidates (37.8%), 491 `UNENCODED: unknown external call` (49.1%), 119 inline asm (11.9%), 12 not present in the IR. A candidate is a function with no blocking construct, not a PRISM verdict.
- The clang pass compiles about 99% of Linux x86_64 allmodconfig translation units (run still in progress) and 99.5% of the FreeBSD GENERIC kernel units with clang 18 (`-fsyntax-only`); about 41% of FreeBSD module units still fail on missing generated `opt_*.h` headers.

## 2. Requirements, in order

**R1. Front end takes the unit's real flags.** Accept a compile-commands entry (or `--cc-flags`) per unit: `-std=gnu17`, `-D__KERNEL__`, `-include kconfig.h`, `-nostdinc`, include paths, `-fno-strict-aliasing`, `-fshort-wchar`. Keep the strict default for existing testdata. Same argv-only process handling. Needs a regression fixture (typeof, `_Generic`, statement expressions, `__auto_type`).

**R2. Kernel API specs.** About half of the sampled functions stop at an unknown external call. PRISM needs a spec table for kernel calls, in the style of `libc_models`: allocation (`kmalloc`, `kzalloc`, `kfree`, `devm_*`), locking (mutex, spinlock, RCU), user access (`copy_from_user`, `get_user`), MMIO (`readl`, `writel`, `ioread*`), DMA, IRQ, clocks, workqueues, refcounts, `container_of`, `list_*`, `printk`. Each spec states its contract and its assumptions; a policy for calls without a spec (assume nothing and report `UNENCODED`, or opt-in havoc of the result with a stated effect, recorded as a PROVED-ASSUMING assumption). The list of calls to specify, ordered by frequency, comes from the OS-Proving clang call graph (data/clang in that repo, being produced).

**R3. Inline assembly and atomics.** Conservative models: havoc of the declared outputs and clobbers with the stated memory effect, labelled as an assumption. Atomics and barriers with the memory orders kept (PRISM already keeps seq_cst and monotonic from the stdatomic shim); `READ_ONCE`/`WRITE_ONCE` patterns. Report each use as a named assumption on the result.

**R4. Per-function analysis of a function from a large unit.** Analyse one function without a whole-program entry: harness generation from the signature, pointer parameters with sizes from annotations or from the mined contract, struct layouts from the IR, bitfields (the Frama-C side fails on 175 bitfield-width cases; check PRISM does not). Resumable per function.

**R5. Levels and assumptions in the output.** Emit the proof level (L0 compiles, L1 bounded, L2 unbounded runtime-error freedom, L3 contract, L4 module property; see the plan) and the full assumption list per function, as machine-readable output, one record per function with the OS-Proving key (`path:start-end`, `i`). Verdict names stay exactly as defined in docs/VERDICTS.md.

**R6. Throughput and limits for a million-function corpus.** A batch mode over a compile database, 2 GB memory cap per worker with a reported `memory-limit` result (never a retry with more), up to 32 workers on a 48 GB VM, resumable, no files on the system disk (scratch in RAM or on a data disk), results in append-only shards. Solver: speed is the bottleneck for bit-vector verification conditions (see section 3).

**R7. Lean export coverage.** Per function: inside the Lean-proved fragment or not, and the `unsupported` reason when not. Needed to say how many results rest on the proved translator and how many on the C++ translator alone.

**R8. Module and device layer (research step).** A way to express a platform or device model PRISM can consume: typed register and MMIO operations, a device model that constrains what a register read may return and which write sequences are legal, interrupt entry and exit, DMA ownership. Start with one small device. Verdicts that use a device model carry it in their assumption list. Existing `abi_model.cpp`, `platform.cpp` and `models` are the starting point; this is design work for Odin and the PRISM maintainers, not a generated change.

**R9. Rust drivers (separate track).** The Linux tree has 64 Rust files under `drivers/` (about 17,900 lines: Android Binder, nova-core, tyr, nova, PHY). PRISM does not need to handle them: a Charon and Aeneas pipeline into Lean is the route, with models of the kernel Rust bindings. Listed here so the two tracks stay consistent.

## 3. Solver speed

Z3 time on bit-vector queries is the limiting cost. FastSMT (ETH SRI, Apache-2.0) learns a Z3 tactic strategy for a dataset of formulas; its paper abstract claims up to 100x runtime improvement over a state-of-the-art SMT solver and 17% more formulas solved on a range of benchmarks (theories and baseline not named in the abstract). The code was tested only with Z3 4.6.2 and Python 3.5. Odin is porting the FastSMT code into PRISM directly (no Z3 port). What the OS-Proving side can supply: a dump of PRISM's own QF_BV queries split into train, validation and test by file, and a held-out evaluation that checks identical sat/unsat answers wherever two configurations both solve a query. The learned strategy never touches the certified path: PROVED-CERTIFIED results still go through the Lean bit-blast certificate.

## 4. Integration points with OS-Proving

- Function identity: `key` = `path:start-end` and `i` as in the OS-Proving final tags and master file (data/master/COLUMNS.md there).
- PRISM results become new master columns: level, engine, verdict, assumptions, replay result, time and memory.
- The module tree used to organise results is the clang-confirmed one from data/clang once complete; the mechanical one is data/modules-all.
- Human review gate: a random sample of each level read by a person (the S5 audit and the calibration there were read by models).

## 5. Not claimed

None of this is implemented except what R1's branch and the clang pass already do. The pilot figures are small and from two drivers. No kernel function has been verified by PRISM yet.
