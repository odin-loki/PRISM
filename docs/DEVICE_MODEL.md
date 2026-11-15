# Hardware and device model, and the driver-module layer

Requirement R8 of `docs/feedback/DRIVER-REQUIREMENTS.md` (branch `os-proving-driver-plan`), Stages 5 and 6 of
`docs/OS_PROVING_DRIVER_PLAN.md` (same branch) and the "driver is a state machine, so check the
driver" direction of OS-Proving's `docs/DRIVER_CONTRACTS.md`. Status: **design plus a working prototype
on one real device** (ARM PrimeCell PL031 RTC, Linux `drivers/rtc/rtc-pl031.c`), bounded, with every
assumption listed. Nothing in this document is a proof of a driver. Section 9 says what was run and what
came out; section 11 says what does not work.

| Where | What |
|---|---|
| `include/prism/module.hpp` | The C++ API: device model, module description, harness, run, report |
| `src/prism/module/devmodel.cpp` | Device-model parser and its translation to C |
| `src/prism/module/spec.cpp` | Module-description parser, entry-point discovery from registration tables |
| `src/prism/module/check.cpp` | Harness generation, running the `pir` stage, reading the verdicts back as module properties |
| `src/tools/module/main.cpp` | The `prism-module` tool |
| `src/prism/pir/models/device/` | The preamble, ghost state and kernel stubs (C), and built-in device models (`*.pdm`); embedded in the binary |
| `testdata/module/` | Toy device and driver with true and false variants; the PL031 and virtio-rng module descriptions |
| `tools/module-validate/` | The QEMU cross-check of the PL031 model and its mutants (`run.sh`, `mutants.sh`, section 7), candidate selection (`select_candidates.sh`), the one-entry compile database for a kernel unit (`mkcdb.sh`) |
| `tests/cpp/test_module.cpp` | Doctests: format, harness, end-to-end on the toy |

Nothing else in the engine changed: one environment variable (`PRISM_PIR_INLINE_DEPTH`, section 4) in
`src/prism/pir/stage_mem.cpp` and its entry in the registry. `stage.cpp` and `translate*.cpp` are untouched.

## 1. What the layer is

A driver module is a set of entry points (probe, remove, ops, interrupt handler, suspend, resume) that
a framework calls in some order, interacting with a device through registers and interrupts. Checking
single functions says little about it: the properties that matter (resources released on every path,
no use after remove, no lock held on return, a handler that is safe while remove runs) are about
sequences. And a driver is only correct relative to what the device may do.

The layer has three parts, all of them ordinary C handed to the existing `pir` stage:

```
device model (.pdm) ──┐
module description    │   prism-module   one C unit:  kernel preamble
  (.pmod: entry       ├──────────────▶   + the driver's own source
   points, env)       │                  + the device model as C
driver source (.c) ───┘                  + ghost state and kernel stubs
                                         + a harness `main`
        ──▶ clang -O0 (the driver's own unit flags, --compile-commands) ──▶ PIR ──▶ Z3
        ──▶ FAILED / BOUNDED / UNKNOWN ... read back per module property, with the assumption list
```

What this deliberately is **not**: a new analysis. Every verdict is the `pir` stage's, on a unit that
contains the driver verbatim. The device model, the stubs and the harness are code that PRISM checks
along with the driver and that the report names as assumptions.

## 2. The device-model description format

### 2.1 Why this format

PRISM reads its configuration from the command line and environment; grammars live in `grammars/`
(the C and C++ ones are for the legacy front ends); JSON (nlohmann) is used for reports and the compile
database. A device model needs expressions and effects ("writing 1 to this bit clears that status
flag"), which JSON would make unreadable. The format is therefore a small block language with a C
expression subset, parsed by `src/prism/module/devmodel.cpp` into a `DevModel` (C++ struct). It is
translated to C, not interpreted, so the solver sees the model as ordinary bit-vector code and an
operator means what it means in ISO C on unsigned 64-bit values. Division and modulo are not in the
language (a model must not contain undefined behaviour of its own); every identifier is checked at parse
time against the names the model declares, with the line number in the error.

### 2.2 Syntax and semantics

```
model NAME {
  doc "text";                                  # free text, repeatable
  source "where this model comes from";        # provenance, repeatable: it goes into the assumption list
  validation unvalidated|datasheet|differential "how";   # status of the model, see section 7
  window 0x1000;                               # size of the MMIO window in bytes

  state NAME : u8|u16|u32|u64 [= INT | nondet()];   # hidden device state, not visible except through registers

  register NAME @OFFSET : uN RW|RO|WO|W1C [reset V] [mask M] [{ items }]
  group    NAME @START .. END : uN RO { read = EXPR; }      # an array of identical registers; `index`

  event NAME { STATEMENTS }                    # a spontaneous device step: time passes, a transfer finishes
  irq   NAME = EXPR;                           # level of an interrupt line as a function of the state
  init  { STATEMENTS }                         # run once at power-on (typically: havoc some state)
  policy unmapped_read|unmapped_write|wo_read|ro_write  zero|unconstrained|violation|ignore;
  policy width strict|any;
  dma "rule";                                  # parsed and listed; not yet enforced (section 2.5)
}
```

Register items:

| Item | Meaning |
|---|---|
| `read = EXPR;` | The value a read returns, a function of the device state. |
| `read in EXPR;` | A read returns *any* value for which the constraint on `result` holds (status flags that change nondeterministically within stated bounds). |
| `read unconstrained;` | Any value of the register's width. |
| `on_read { ... }` | Effects of a read (read-to-clear registers). |
| `write { ... }` | Effects of a write (`value` is what was written): assignments, `if`, `assume`, `havoc X [where C];`, `require C : "rule";`. |
| (none) | The access type's default: RW stores `value & mask`, W1C clears the bits that are set in `value`, RO reads the stored value, WO is not readable. |

Statements and expressions use `= |= &= ^= += -= <<= >>=`, `if / else`, and the C operators
`+ - * << >> & | ^ ~ ! && || == != < <= > >= ?:`. Every assignment is truncated to the declared width.
`havoc X where C` gives X an arbitrary value of its width for which C (which mentions X) holds.

Two statements differ in what they ask of the *driver*:

* `assume C;` restricts the device's behaviours: an environment assumption.
* `require C : "rule";` is a **protocol rule**: the driver must not reach this state. A violation is a
  finding (`[module:protocol]`), not a restriction. Example: "DATA written while the device is busy".

`policy` says what an access the register map does not allow means: a read of an unmapped offset, a
write to a read-only register, a read of a write-only register, an access of the wrong width. `violation`
(the default) makes it a finding; `zero`, `unconstrained` and `ignore` say what the device does instead
(QEMU's PL031 logs a guest error and returns 0; the model says `violation` because the driver doing that
is a bug worth reporting).

The shipped example is `src/prism/pir/models/device/pl031.pdm` (a real device, about 100 lines with
comments); a small one is `testdata/module/toy/toy.pdm`:

```
register STATUS @0x04 : u32 RO {
  read in ((result & 0xfffffcfe) == busy) && (((result >> 8) & 3) <= 2);   # busy flag; channel field 0..2, never 3
}
register INTSTAT @0x08 : u32 W1C reset 0 mask 1;       # write 1 to clear
register DATA @0x10 : u32 WO {
  write { require busy == 0 : "DATA written while the device is busy"; DATA = value; }
}
event done { if (busy != 0) { busy = 0; INTSTAT = 1; } }
irq line = INTSTAT & INTEN;
```

### 2.3 What the translation produces

`prism-module device-c FILE.pdm` prints it. For each model: `prism_dev_read(off, width)` and
`prism_dev_write(off, val, width)` (a `switch` on the offset: width check, then the register's
read/write semantics), `prism_dev_irq_level(line)`, `prism_dev_event()` (picks one event
nondeterministically and runs it), `prism_dev_init()`, and the state as `static` variables.
Nondeterminism is `__VERIFIER_nondet_*` plus `__VERIFIER_assume`, the primitives PIR already models.

### 2.4 Interrupt lines

`irq NAME = EXPR` is the line level. The harness may deliver the registered handler when the level is
asserted, or at any time when the line is shared (`IRQF_SHARED`; spurious calls are the rule on shared
lines, and the PL031 handler returns `IRQ_NONE` for them). Raise and clear conditions are the writes and
events of the model: INTSTAT/RIS set by an event, cleared by a W1C write or an ICR write.

### 2.5 DMA ownership (designed, not enforced)

`dma "rule";` is accepted and listed in the report so that models can carry the rule next to the
registers. Enforcing it needs a view of the driver's memory accesses that the PIR memory model has
(every load and store is a checked statement) but that the harness does not yet use. The design: a
`dma` region is a heap object with an owner (`cpu` or `device`); `dma_map`/`dma_sync` stubs flip the owner;
a device `event` may havoc the bytes of a device-owned region; a CPU access to a device-owned region
is `[module:dma-ownership]`. The virtio pilot (section 10) states the virtio form of the rule and
what it would take.

## 3. How accesses resolve to the device model

Every MMIO access of the driver becomes one call of the **resolver interface** (`prism_pre.h`):

```c
unsigned long long prism_mmio_read(const volatile void *addr, unsigned width);   /* width in bytes */
void prism_mmio_write(volatile void *addr, unsigned long long val, unsigned width);
```

These compute the byte offset of `addr` in the device window (a pointer difference against the one MMIO
window object, so a pointer that is not into the window is a pointer-comparison violation) and call
`prism_dev_read/prism_dev_write`. They also check that the window is mapped (`[module:use_after_remove]`)
and that the access lies inside it (`[module:protocol]`).

* **The default is an unconstrained read of the right width** (`prism_default_device.h`): with no
  device model every `readX` returns an arbitrary value of its width, writes have no effect, the interrupt
  line is arbitrary. A report that used it says so and is level L1 at best.
* **Linux.** `src/prism/pir/models/device/linux_pre.h` (included after the driver's own `<...>` headers)
  redefines `readb/w/l/q`, `_relaxed`, `__raw_*`, `ioread8/16/32` and the `write` side to call the
  resolver. This is the narrow hook for R2: R2's kernel API table can define the accessors with the same
  two resolver calls and nothing else changes. It also replaces what the x86 headers implement with inline
  asm (bit operations, `BUG`/`WARN`, `kmalloc`-family size-class machinery, spinlock/mutex entry points,
  `IS_ERR`/`PTR_ERR`), each stated in the file; those are a local stand-in for R2 and are listed in every
  report as "kernel API stubs of the linux flavour".
* **FreeBSD `bus_space`.** `bus_space_read_N(tag, handle, offset)` and `bus_space_write_N` are macros over a
  `(tag, handle)` pair; with a tag whose handle is the window base they become the same two calls with
  `handle + offset`. A `freebsd` flavour (`freebsd_pre.h`, with the `bus_space_*`, `bus_read_*`,
  `bus_write_*` and resource-manager (`bus_alloc_resource`, `bus_release_resource`) stubs) is the same
  size of work as the Linux one; it is not written.
* **virtio ring accesses.** The ring is guest memory the device writes, not a register window; its
  resolution is not through `prism_mmio_*` but through the virtqueue API stubs (section 10): `virtqueue_get_buf`
  returns the device's answer, an arbitrary used length and arbitrary buffer bytes within the protocol.

## 4. The module harness

### 4.1 Entry points

Entry points come from the driver's registration tables: `prism-module entries SPEC` lists every
designated initialiser `.field = function` of the driver whose function is defined in the file, and
whether the module description's entry blocks exercise it. For the PL031 that is `probe`, `remove` and
the five `ops` members of each of the three vendor tables; the ST variants' members are reported as
**not exercised** (the device model is the ARM variant), which is the honest statement of coverage. The
module description (`.pmod`) says how to call them and builds the environment:

```
module rtc_pl031;  kernel linux;  driver "drivers/rtc/rtc-pl031.c";  device builtin:pl031;
stubs amba rtc;  steps 2;  irq_points api steps;  teardown_ops yes;
assume "...";                     # free text, copied into the assumption list
env   { static struct amba_device adev; ... }        # file-scope C, after the driver and the stubs
setup { adev.res.end = 0xfff; adev.irq[0] = prism_nondet_bool() ? 33 : 0; }
probe  { return pl031_probe(&adev, &pl031_ids[0]); }
remove { pl031_remove(&adev); }
op read_time { if (!prism_rtc_registered) return; PRISM_OP_REGISTERED(read_time, pl031_read_time);
               (void)pl031_read_time(&adev.dev, &tm); }
```

The bus and framework glue (`env`, `setup`, the way an op is invoked) is hand-written C: a reviewed
assumption, named in the report, not generated.

### 4.2 Sequence semantics

The generated `main` is the module state machine, unrolled (no loops; `main` so that globals start at
their initialisers):

```
setup;  r = probe;
  r != 0:   devres release; CHECKPOINT("failed probe")                        -- the failing-probe path
  r == 0:   repeat up to `steps` times:  [device event / interrupt point];
                                         choose: stop | one of the operations;  lock check
            [point];  remove;  lock check;
            [teardown window: one more operation can still arrive -- the framework has not
             unregistered the device yet];
            devres release;  CHECKPOINT("remove");
            the handler must be unregistered; if it is not, run it once more (it runs on freed state)
```

Everything that can fail does, nondeterministically (allocation, ioremap, region request, irq request,
device registration), so all failing-probe paths are reached. `devm_*` resources are released in
reverse order when a failed probe returns and after `remove` returns, as the driver core does.
Operations are chosen freely, so the sequence is "any sequence of operations of length at most
`steps`".

**Interrupts and device events.** `prism_async_point()` lets the device act (run its `event`) and then the
registered handler run, if the line is asserted or the line is shared, at: every step boundary, and the
kernel API calls that matter for teardown ordering (just after `request_irq` returns, before `free_irq`,
before an unmap or region release, around lock acquire/release); optionally at every MMIO access
(`irq_points mmio`, larger). At most 2 interrupts and 2 device events per run; no nesting. A handler
therefore runs "between any two steps and in the middle of probe and remove". This is coarser than
instruction-level interleaving (a handler cannot fire between two statements with no API call between
them): it is an assumption, named in the report, and the one a Lean treatment of the module would have to
close (section 8).

**Why `main` and one unit per row.** The `pir` stage reports the functions defined in the unit's own
file and starts mutable globals at their initialisers only in `main`. So each *row* (section 6) is a unit
file that contains the whole harness, and the driver, device model and stubs are `#include`d into it.
The driver's own `#include <...>` lines are hoisted before the macro overrides, so the overrides follow
every header that declares the overridden name. The driver compiles with the flags of the compile-database
entry `--cc-like` selects (for the PL031, an x86_64 allmodconfig entry of a sibling file: the driver is
architecture-neutral C but the tree is x86; `readl` etc. are replaced, so no x86 asm is involved).

**Inlining depth.** `pir` inlines defined callees to depth 4. A probe that calls a stub that calls a ghost
operation that runs a handler needs more. `PRISM_PIR_INLINE_DEPTH` (new, registered in
`prism::env_vars()`, default unchanged) raises it; `prism-module` sets 24 unless it is set. This is the
only change to the `pir` stage's code.

**Block instances.** `pir` gives up (`UNKNOWN: unrolling exceeds 6000 block instances`) when the unrolled
unit is too large. Everything in the ghost layer is written to be small for that reason: two lock slots,
six devres slots, allocation through the PIR allocation intrinsics rather than the `malloc` model,
operations called by name (with a check that the registered ops member is that function) instead of through
an indirect call that would inline every same-signature function.

## 5. Properties

Each is a `PRISM_MOD_CHECK` in the ghost layer or the harness (`[module:<name>]` in the finding), except
the native ones, which are `pir`'s own checks on the driver code.

| Property | Checked how | Message tag |
|---|---|---|
| **Resource pairing** on every path, including error paths and remove | Counters for bus regions, ioremap windows, irq registration, plain allocations, clock enables; release without acquire, double acquire, and a non-zero count at the end of a failed probe or of remove are violations. `devm_*` resources are released by the framework model. | `pairing` |
| **No use after remove or after a failed probe** | The window is unmapped when its last mapping is released: any later MMIO access is a violation. An irq handler still registered after remove is run once more (so a native use-after-free is found, if there is one) and reported. Operations and handlers run on freed state are `pir`'s own MEM-UAF. | `use_after_remove` + native |
| **No UB or memory error in any entry point, for every device behaviour the model allows** | `pir` native checks on the whole unit (UB, bounds, null, use after free, uninitialised reads, double free, invalid free, alignment), over all nondeterministic device values and all failure injections. | native classes (`MEM-OOB-READ`, `MEM-UAF`, `INT-SIGNED-OVF`, ...) |
| **Lock discipline** | A two-slot lock table: lock while held (self-deadlock, including a handler taking a lock the interrupted code holds when interrupts are not disabled), unlock of a lock not held, a lock held when an entry point returns or at a checkpoint, a sleeping lock in interrupt context. | `lock` |
| **Interrupt handler safe while teardown runs (bounded)** | The handler can run at the points of section 4.2, including inside probe and remove, with the interleavings bounded as stated. A violation that appears only with interrupts is attributed by comparing the `all` row with `all-sequential`. | native + module |
| **Device protocol** | The device model's `require` rules, width and register-map policies, and MMIO outside the window. | `protocol` |
| **API contract** | Calls the stubs refuse (request_irq with a NULL handler, BUG reached, register an rtc without ops). | `api` |

## 6. Verdicts, levels, rows

The module runs several **rows**, each one unit, so a result can be attributed:

| Row | What it checks | What it hides |
|---|---|---|
| `all` | every selected module property and the native checks, with events and interrupts | nothing (the headline) |
| `all-sequential` | the same with no events or interrupts | separates failures that need interrupts |
| `memory` | native checks only, module checks compiled out | |
| `pairing`, `lock`, `use_after_remove`, `protocol`, `api` | one module property, and `pir` verifies only the module checks (the stage's own check-property filter (`Config::pir_only_props = {"model"}`)); native checks are **not** verified in this row | native errors, so a counterexample to the property is not masked by an earlier memory error |
| `cover-probe-ok`, `cover-probe-fail`, `cover-irq`, `cover-remove`, `cover-teardown-op`, `cover-op-<name>`, `cover-op-<name>-body` | **Coverage**: the row compiles a check that fails at one point of the harness (probe returned 0, the failing-probe path, the handler running, remove, an operation invoked, an operation past its framework guard to its body). `pir` FAILED means the point is reachable. A row that `pir` PROVES means the point is **unreachable under the model**, and the report prints a `VACUITY WARNING`: the other rows are then proofs about code that never runs. | nothing; this is what makes a clean result mean something |

Verdict wording is exactly [VERDICTS.md](VERDICTS.md)'s. The mapping:

* `pir` says **FAILED** for the harness: the module verdict is **FAILED**, with the class, the line of the
  violated check and the nondeterministic values on the violating path (`extra.nondet`). A FAILED is
  a counterexample *to the module under the model*: it needs triage (real bug, model artefact, or missing
  precondition) before it is a finding about the driver. Section 9 triages each one.
* `pir` says PROVED / PROVED-UNBOUNDED / PROVED-CERTIFIED / PROVED-ASSUMING on the harness: the module
  verdict is **BOUNDED**, never a proof. The harness contains only the sequences of at most `steps` operations,
  at most two interrupts and two device events, and the failure injections it lists; "no violation in all of
  them" is a statement about those. The report keeps `extra.pir_verdict_*` so the stronger statement is not
  lost. **A module result is never PROVED on this layer**: a PROVED would need an inductive invariant over the
  module's states that holds for sequences of any length (section 8), and an unvalidated model would not
  support it anyway.
* UNKNOWN, TIMEOUT, NEEDS-HARNESS (an `UNENCODED:` construct), NOTRUN pass through unchanged.

Levels (`docs/OS_PROVING_DRIVER_PLAN.md` (same branch) section 2): with a device model the result is
**"L4 (bounded)"** and the device model is the first line of the assumption list, with its validation
status (`unvalidated`, `datasheet`, `differential`) and its sources. Without one the result is **"L1
(bounded; no device model: every register read unconstrained)"**: it is a statement about a driver facing
an adversarial device, which is exactly the hostile-device claim of the second pilot. FAILED and
non-answers carry no level. `L4` here is "module properties hold on the bounded sequences of the model",
not the plan's unbounded "on every entry-point sequence".

The assumption list of every module report (printed and in `--json`): the device model and its validation;
the bounded sequence and event/interrupt budgets; the kernel stubs; the devres and lock bounds; each
`assume` line of the module description; the compile flags used; and that the bus/framework glue is
hand-written.

**Nothing is called PROVED on the strength of an unvalidated model**, and nothing at all is called PROVED
by this layer. An unvalidated device model is allowed (a first sketch is useful), but then the report says
`validation: unvalidated` in the assumption list and the level text does not hide it.

## 7. Validating a device model

A device model is code someone wrote; a proof relative to it is only as good as it. Three routes, in
order of strength, recorded in the model's `validation` line:

1. **`datasheet`**: reviewed register by register against the vendor's reference manual, by a person
   who is named in the model's `source` lines. (The PL031 manual, ARM DDI 0224B, was **not** available to
   the author; the PL031 model does not claim this.)
2. **`differential`**: the model and an independent implementation of the same device are driven with the
   same register sequences and must agree. For QEMU-modelled hardware the independent implementation is QEMU's
   device model, run under QEMU. The PL031 model was checked this way (`tools/module-validate/`):
   * a bare-metal AArch64 guest, compiled with clang, runs a generated trace of register reads and
     writes on `qemu-system-aarch64 -M virt` (the PL031 at 0x09010000) and prints every access and result
     on the PL011 UART;
   * `replay` feeds the same writes to the model translated to C and compares every read. The model's
     nondeterminism is resolved from the trace (elapsed time from the DR values QEMU shows, whether a due
     alarm has fired yet from the next RIS read); a trace that needs the alarm to fire before the model can
     fire it is a mismatch;
   * section 9.5 gives the numbers and the mutants the check rejects.
   This validates *the model against QEMU's model*, i.e. against the source it was written from and one
   run-time implementation of it. It does not show QEMU matches the silicon. The `source` lines say what
   was taken from where.
3. **`unvalidated`**: neither. The default for a new model.

The model can be wrong in two ways: too small (it forbids a behaviour the hardware has: unsound for the
driver proof) or too large (it allows what the hardware never does: false counterexamples). The
nondeterministic parts of the PL031 model are written to be over-approximations (the alarm may fire
late) so that the first kind of error is the one to look for.

## 8. The Lean side

The existing Lean development (`proofs/semantics`, `PrismSem`) defines PIR syntax, its bounded semantics
`run k`, the UB predicate, the memory model, and proves the bounded encoder exact: a model of the VC is a
real counterexample and an unsatisfiable VC means the program has no UB within the bound
([PROOFS_SEMANTICS.md](PROOFS_SEMANTICS.md)). The translator from LLVM IR to PIR is mirrored and proved to
create a failed check whenever the LLVM function reaches UB or poison (`pir_sound`,
`proofs/refinement`). The module layer is compatible with that chain as it stands: the harness, device
model and stubs are C that goes through the same translator, so whatever `pir_sound` and the encoder
theorems give for *a function* they give for the harness function. What Lean does not yet say is what the
*harness means* and how it relates to a driver running with a real kernel and a real device. That needs:

1. **A device-model type and its semantics** (`PrismSem/Device.lean`): a record of registers
   (offset, width, access, reset, a read relation `State → Value → Prop`, a write function
   `State → Value → Set State`), events `State → Set State`, irq level `State → Bool`. The PDM translation to
   C (`devmodel.cpp`) is mirrored as a Lean function from that record to a PIR fragment, and a theorem
   *translation preserves semantics*: for every register access, the PIR fragment's outcomes are exactly
   the record's relation (the shape of the existing `instr` exactness theorems).
2. **The module state machine as a transition system**: states = (phase, resources held, device state,
   irq registered, locks held, driver data), transitions = probe (with failure branches), each operation,
   remove, a device event, an interrupt delivery. A theorem *the unrolled harness's executions are the
   transition system's runs of length at most K*, so that PIR's VC for the harness is a statement about
   exactly those runs.
3. **Entry-point summaries** (function contracts, L3, Stage 4 of the plan): for each entry point a
   pre/post relation over module states, proved or checked per function with the existing machinery.
4. **The module invariants and their induction** (what turns BOUNDED into a proof): an invariant `I` over
   module states with: `I` holds after reset; each transition preserves `I` (using the entry-point summaries
   and the device model's relations); and `I` implies the properties of section 5 (every region/irq/map count
   in `{0,1}` and zero after remove, no handler registered when the state is `removed`, no lock held in a quiescent
   state). Because the state is finite (counters in `{0,1}`, a phase, a bounded device state) the induction
   closes by `decide`, and "any sequence" holds, but only *relative to* the entry-point summaries and the
   device model, which is where the remaining assumptions sit. The interrupt interleaving at instruction
   granularity needs a proof that the handler's accesses commute with, or are guarded against, the
   driver's non-atomic updates; for a driver whose handler touches only device registers and state that
   `free_irq` has already quiesced, that is a statement about the handler's read/write set.
5. **Validation of the device model against a reference implementation** as a refinement or simulation
   theorem, where a reference exists in a form Lean can read (QEMU's device functions are C: through the
   same translator they become PIR, and the model-versus-QEMU check becomes an equivalence of two PIR
   fragments for each register access, decidable on the finite register map).

None of this is in the prototype. The prototype's results are PIR verdicts on a harness; they are as
strong as the translator and encoder theorems for those functions and no stronger.

## 9. The prototype: ARM PrimeCell PL031 RTC

### 9.1 Why this device

Selected by `tools/module-validate/select_candidates.sh` (grep over the Linux 6.18.60 tree and the
OS-Proving clang pass's ops tables, `data/clang/linux/ops.tsv.gz`). Criteria: 80 to 420 non-blank lines;
at least 3 MMIO accesses; a `request_irq` call; `.probe` and `.remove` in a registration table; an `ioremap`.
33 files in `drivers/{rtc,watchdog,char,misc,mfd,input,leds,gpio,clk,mailbox,hwspinlock,dma}` pass. Among
them, by hand: a QEMU model of the same hardware exists for `rtc-pl031.c` (QEMU `hw/rtc/pl031.c`, which I
read), for `ambakmi.c` (PL050, from memory, not checked) and `rtc-zynqmp.c` (from memory, not checked);
`rtc-pl031.c` is the smallest of those with an ops table (five `rtc_class_ops` members through three vendor
tables), real error paths (`goto out` / `err_req` with a region release), a request/free pairing across probe
and remove, an interrupt handler, a clean datasheet-free source for the register map (QEMU) and a second
variant (ST v2) with decoding code that a hostile device can steer. The watchdog candidates the plan names
(`sp805_wdt.c`) have no interrupt handler and no QEMU model. 392 lines.

### 9.2 What was set up

```
prism-module run testdata/module/linux/rtc-pl031.pmod --kernel-root /home/odin/pbsd-ref/linux \
    --cc-like cdb.json --cc-like-file drivers/rtc/rtc-cmos.c --steps 2 --row-jobs 3 --mem-mb 4000 --json
```

* **Driver**: `drivers/rtc/rtc-pl031.c`, verbatim, ARM variant (`pl031_ids[0]`). Entry points (from the
  registration tables): `pl031_probe`, `pl031_remove`, `read_time`, `set_time`, `read_alarm`, `set_alarm`,
  `alarm_irq_enable`, and the handler `pl031_interrupt` (registered by `request_irq`). Not exercised: the ST
  variants' ops (`stv2`), which the ARM device model does not describe; the next section runs them.
* **Flags**: the compile-database entry of `drivers/rtc/rtc-cmos.c` from the OS-Proving x86_64 allmodconfig
  pass (`make -n` text, a one-entry compile database made by `tools/module-validate/mkcdb.sh`), cloned for the harness unit. The driver is
  ARM-only hardware code compiled with x86_64 kernel flags: it is architecture-neutral C and every `readl` is
  replaced, so no x86 asm enters. It compiles with the front end's GNU C fallback and the unit's own
  `-nostdinc -include kconfig.h` flags (PR #47).
* **Device model**: `pl031.pdm`, taken from QEMU `hw/rtc/pl031.c`, validated against it (section 9.5).
* **Bus and framework glue**: the `.pmod` file: an AMBA device with a 4 KiB resource and `irq[0]` either 0 or
  nonzero; the RTC core's `rtc_valid_tm` guard modelled by filling `rtc_time` with calendar-valid values; the
  registered `ops` pointer checked to be the function each operation exercises.
* **Bounds**: probe, then at most 2 operations (5 kinds), then remove, then one more operation in the teardown
  window; at most 2 interrupts and 2 device events; events and interrupts at step boundaries and at
  `request_irq`, `free_irq`, unmap, region release.
* **Machine**: WSL2, 64 cores, 48 GB; other agents were building (up to 6 PRISM processes at a time);
  prism built Release with clang 23. `--timeout 300` per verification condition, no per-process memory cap.

### 9.3 Results (ARM variant, device model `pl031`)

Verdicts exactly as VERDICTS.md names them. The module verdict of a PIR PROVED is BOUNDED (section 6).

| Row | What it checks | `pir` verdict | Module verdict | Seconds |
|---|---|---|---|---:|
| `all` | all module properties and all native checks, events and interrupts | PROVED | BOUNDED | 418 |
| `all-sequential` | the same without events and interrupts | PROVED | BOUNDED | 57 |
| `memory` | UB and memory errors in every entry point | PROVED | BOUNDED | 50 |
| `pairing` | resource acquire/release on every path | PROVED | BOUNDED | 37 |
| `lock` | lock discipline | PROVED | BOUNDED | 36 |
| `use_after_remove` | no access after unmap, handler gone after remove | PROVED | BOUNDED | 36 |
| `protocol` | the device model's rules, window bounds, width | PROVED | BOUNDED | 38 |
| `api` | contracts of the stubs (registered ops, rtc registration) | PROVED | BOUNDED | 33 |
| coverage, 15 rows | probe ok, probe fails, handler runs, remove, teardown op, each op invoked, each op past its guard to its body | FAILED = reachable | all 15 reachable | 19 to 37 each |

Level: **L4 (bounded)**, device model `pl031` (validation: differential). One invocation, 23 rows, 3 rows at a time
(`--row-jobs 3`, each row in its own process): 7 minutes of wall time. Peak resident set of a row's process 417 MB
(the `all` row; the others 130 to 280 MB), measured by the process runner, plus the `z3-worker` children of the query being
solved (not in that figure; 130 MB to about 1.5 GB seen with `ps`). The `all` row is 7 to 14 times slower than the single-property rows
because the interrupt and event points multiply the unrolled code. Before the ghost layer was shrunk (an
interrupt point at every API call, 16 devm slots, the `malloc` models) even zero operations ended
`UNKNOWN: unrolling exceeds 6000 block instances` and one run was stopped by hand after 11 minutes; three
operations were not tried after the shrink. The bound that matters is the size of the unrolled unit more than
the solver (section 11).

**What this says**: for every sequence of at most two operations out of the five, with the device
behaving as the model allows (any elapsed time, alarm firing late, shared-line-free interrupts), with each
allocation, mapping, region request, irq request and RTC registration free to fail, and with the handler allowed to run
at the points of section 4.2, the driver (ARM variant) has no undefined behaviour or memory error in any
entry point, releases every region, mapping, irq and allocation on every probe-failure path and in remove,
touches the device only while its window is mapped, leaves no handler registered after remove, takes no
locks and respects the device's register rules. **What it does not say**: anything for three or more operations,
for the ST variants, for a device that deviates from the model, for interrupts at points the harness does not
have, for concurrent entry points, or about a real RTC core (the glue models it) or real hardware
(the model matches QEMU).

### 9.4 Assumptions each row used

| Assumption | Rows that depend on it |
|---|---|
| A1 device model `pl031` (QEMU-derived, differential) | every row (the model is compiled in); `protocol` is about it |
| A2 bounded sequences: 2 operations, 1 teardown operation, probe failures free to occur | every row |
| A3 events and interrupts at the harness's points, at most 2 each, handler only while registered | `all`, `memory`-with-interrupts rows and all single-property rows; **not** `all-sequential` |
| A4 kernel stubs: allocation, mapping, region, irq, registration may fail; `rtc_time64_to_tm` yields any calendar-valid time; `rtc_tm_to_time64` any value; bit operations, logging, BUG/WARN as in `linux_pre.h` | every row |
| A5 at most 6 devm resources and 2 locks | every row |
| A6 the glue of the `.pmod` (AMBA resource, valid `rtc_time`, ops only while registered) | every row |
| A7 the driver compiled with x86_64 kernel flags; front end drops flags that would define UB | every row |
| A8 the verdict of `pir` on the harness (translator, encoder, Z3: the trusted base of TRUSTED_BASE.md, not certified: no `--certified` run) | every row |

### 9.5 Validation of the PL031 model against QEMU

`tools/module-validate/run.sh` (and `mutants.sh` for the table below): bare-metal AArch64 guest on `qemu-system-aarch64 -M virt` (QEMU 8.2 from the
Ubuntu archive of the VM), the PL031 at 0x09010000, output on the PL011.

| Trace set | Shipped model | Mutants rejected |
|---|---|---|
| broad: 40 random traces of 24 operations (reads of every register, writes of MR, LR, IMSC, ICR, CR, up to three waits) | 40 of 40 agree (about 1,900 reads) | `icr_any` (ICR clears everything) 4/20, `mr_noimm` (no immediate alarm when MR equals the counter) 10/20, `cr_zero` 20/20, `imsc_wide` (IMSC keeps all bits) 11/20, `lr_nooff` (LR does not move the counter) 16/20, `never_fire` 3/20 |
| alarm: 30 traces of three arm-wait-observe-clear rounds | 30 of 30 agree | `icr_any` 10/20, `mr_noimm` 12/20, `never_fire` 9/20 |

One mutant survives: `late_fire` (the alarm may fire only when the elapsed time exceeds the remaining time,
not when it equals it). QEMU's own timer lags the counter by up to a second, so the equality boundary is
never observable from the guest: the check cannot tell the two apart, and that boundary of the model is
**not validated**. Also not validated: behaviour at 32-bit wrap, accesses of widths other than 4 bytes (the
model says `violation`, QEMU masks), and everything the traces did not touch. The replay's time
synchronisation hides DR differences unless they exceed 8 seconds, so DR is validated for LR/MR/IMSC
interplay, not for the clock itself.

### 9.6 Counterexamples, triaged

Every counterexample found while building this, and what it was. None is against the shipped ARM-variant
configuration, which came out clean.

| # | Configuration | Verdict, class | Triage |
|---|---|---|---|
| C1 | ST v2 variant, **no device model** (any register value) | FAILED, `MEM-OOB-READ` on `rtc_ydays[2][13]`, 30 s | **Missing precondition (device side), a robustness gap.** `pl031_stv2_time_to_tm` decodes the month field of DR/MR (4 bits) as `tm_mon = field - 1` and passes it to `rtc_year_days`, which indexes `rtc_ydays[leap][month]` with no check. A month field of 0 gives `(unsigned)-1`, 13 to 15 index past the row. For the real ST silicon it is unreachable if DR is always a valid date; it is reachable from a hostile or buggy device, and from MR if the register resets to 0 (the datasheet is not available to the author). It says the driver trusts the hardware's register encoding, a fact worth knowing, not a defect on conforming hardware. |
| C2 | ST v2 variant, model `pl031_stv2` (DR/YDR calendar-valid, MR/YMR reset to 2000-01-01; **unvalidated**), `rtc_time` raw (any `int` fields) | FAILED, `INT-SIGNED-OVF` (signed addition overflows) in `pl031_stv2_tm_to_time`, 210 s | **Missing precondition.** `(tm->tm_mon + 1) << RTC_MON_SHIFT` overflows for a huge `tm_mon`. The RTC core calls `set_time` and `set_alarm` only after `rtc_valid_tm`; the ARM module's glue encodes that (the `valid` fill); here it is removed on purpose (`--define RAW_TM`) to show the check sees it. Not a driver bug. |
| C3 | ST v2 variant, model `pl031_stv2`, valid `rtc_time` | BOUNDED, L4 (bounded) with an unvalidated model, steps 1 (steps 2 is `UNKNOWN: unrolling exceeds 6000 block instances`) | Not a counterexample: shows that the constraint of C1 is what the proof depends on. The model is a guess; the report says `validation: unvalidated`. |
| C4 | Model validation: first `replay` version | 0 mismatches on a `lr_nooff` mutant | **Model-check artefact, fixed**: the time synchronisation absorbed any DR difference. Now a DR jump over 8 s is a mismatch (and `lr_nooff` is rejected 16/20). |
| C5 | First run of virtio-rng (section 10) | memory 15 GB, no answer | **Harness artefact, fixed**: see section 10. |
| C6 | Toy driver variants (`testdata/module/toy`, section 6 rows) | LEAK: `[module:pairing]`; UAF: `MEM-UAF`; LOCKLEAK: `[module:lock]`; NOCHECK: `[module:protocol]`; SHORT_TABLE: `MEM-OOB-READ`; no device model: `MEM-OOB-READ` | Planted; each is a **real bug of the planted variant**, found as the property it was planted against. |

### 9.7 Not run, to be honest

No `--certified` run (the harness VCs are large; the certified path was not tried). No Lean proof. The model
for `pl031` was not checked against the ARM manual. The ARM variant was not run with `irq_points mmio` (every
MMIO access an interrupt point), which is larger.

## 10. Second pilot: a hostile device (virtio-rng)

**Status: attempted, incomplete.** The unit builds and the driver's probe, scan and remove paths against a hostile
transport verify. Any operation on top of them (a read, a virtqueue callback, suspend/resume) ran `pir` out of
memory (6 to 8 GB budgets) and gave `UNKNOWN`. So the claim this pilot exists to make, "no UB or memory error for
any device behaviour", is **not established** for virtio-rng, and neither is the mutation check that was meant to
show it has teeth. What was learned is below.

### 10.1 The setup

`drivers/char/hw_random/virtio-rng.c` (Linux 6.18.60, 278 lines; the version that already clamps the
device-reported length in `copy_data`). `testdata/module/linux/virtio-rng.pmod`, `stubs virtio`
(`linux_virtio.h`): one virtqueue whose device side is untrusted.

The device may, within the ring protocol: complete a posted buffer at any time after the kick, fill it with
arbitrary bytes (a constant-length `__prism_havoc_bytes`), and report an arbitrary 32-bit used length
(`virtqueue_get_buf`; `define PRISM_VIRTIO_CONFORMANT` restricts it to the buffer size: a conforming host); run the
virtqueue callback at any time, spuriously included (the driver says "we can get spurious callbacks"); fail
`find_vqs`. The transport's config ops (`find_vqs`, `del_vqs`, `get/set_status`, `reset`) and the hwrng core
(`hwrng_register`, `hwrng_unregister` calling the driver's `cleanup`) are stubs. Operations: `read_nowait`,
`read_wait`, `suspend_resume` (freeze then restore), `callback`, and `cb_read` (callback then read: the one order
in which the device-reported length reaches `copy_data`). Property of interest: the driver has no UB or memory error
for any behaviour of this device; the module rows also check pairing (`kzalloc`/`kfree`, `ida`, `find_vqs`/`del_vqs`,
`hwrng_register`/`unregister`) and use after `del_vqs`.

The module description carries 5 source substitutions (`patch`, listed in the report as "DRIVER SOURCE PATCHED"):
`hwrng.priv` holds a pointer in an `unsigned long` (`ptrtoint`/`inttoptr`: `UNENCODED`), so the round trip goes
through a variable; and `config->del_vqs` is called directly (section 11, indirect-call recursion). The unhardened
variant `virtio-rng-unhardened.pmod` adds the substitution that removes the length clamp (a mutant of the real code, not
the shipped source), which a hostile host drives into an out-of-bounds read of `memcpy` in `copy_data`.

### 10.2 What ran

| Configuration | Rows | `pir` verdict | Notes |
|---|---|---|---|
| probe, scan, remove, all failure injections (steps 0), hostile transport | `all` | PROVED, module verdict **BOUNDED**, "L4 (bounded)" | 120 s; `find_vqs`, `kzalloc`, `ida_alloc`, `hwrng_register` may fail; probe ok, probe fail and remove reachable. The device model is "the virtqueue device is untrusted within the ring protocol", **unvalidated**. |
| one operation (steps 1): `callback`; `read_nowait`; `suspend_resume`; `cb_read`; `cb_read` with a 32-byte buffer; the unhardened mutant with `cb_read` | `all` | UNKNOWN (memory budget of 6 to 8 GB reached after 1 to 5 minutes) | no answer. Never a proof. |

### 10.3 What it says and does not say

Probe, scan and remove, with every allocation, id, transport and registration step allowed to fail and without any
virtqueue traffic, have no UB or memory error and release everything they take, in the bounded sense of section 6.
Nothing is known about reading, the callback, or suspend and resume. In particular the "hostile hypervisor or
device" result is **not** delivered.

### 10.4 A lead from reading (not a PRISM result)

`probe_common` sets `vdev->priv = vi` before `virtio_find_single_vq`; its failure path frees `vi` and leaves
`vdev->priv` dangling. `virtrng_restore` returns that error to the virtio core, which leaves the device bound; when
the driver is later unbound, `virtrng_remove` reads `vdev->priv` and `remove_common` writes `vi->hwrng_removed`:
a use after free, reachable after a failed resume. The harness has the `suspend_resume` operation to find exactly this,
but it did not complete (above). This is the author's reading of about twenty lines of the driver and of the virtio
core's remove path, **not confirmed** by running it or by PRISM; it is listed for a person to check.

### 10.5 What blocked it, and what would unblock it

* The `pir` memory model cost, not the property: a copy of a symbolic length from a symbolic offset
  (`memcpy(buf, vi->data + idx, size)`), the 360-byte `struct hwrng` copy and the zero-initialised
  `struct virtrng_info` make the encoder's read-over-write chains large; the callback alone, with a direct
  call, reached the budget too. The cheap steps 0 configuration shows the pipeline is not the obstacle. (Not
  profiled: the three causes above are the likely ones, not measured ones.)
* Smaller steps, a smaller object, or summaries of `copy_data` and `virtqueue_get_buf` (Stage 4: a contract for each
  entry point and each stub) would cut it; so would a memory model with a per-object cell limit.
* A `pir` translator that models `ptrtoint`/`inttoptr` round trips (R3 or a translator change, not this layer)
  would remove four of the five substitutions.

## 11. What does not work, and what is assumed

**Bounds.** Everything is bounded: two operations (one for the ST variant's model run), at most two
interrupts and two device events, six devm resources, two locks, one device, one interrupt line, one
virtqueue. A BOUNDED result is not "any sequence"; the unbounded claim needs the induction of section 8.
Interrupts interleave at the harness's points, not between arbitrary instructions; a handler that races with a
non-atomic update between two such points is not found. Concurrent entry points (two operations at once) are
not modelled.

**Scaling.** The cost is the size of the unrolled unit: `pir` inlines every call, and each interrupt point
inlines the handler and a device event. The PL031 harness with two operations takes about five minutes for the
headline row and was UNKNOWN (block instances over 6,000) with three, and the ST variant with its
constrained model was UNKNOWN at two operations. Reaching real drivers of a few thousand lines needs
function summaries (Stage 4 of the plan: each entry point replaced by its proved contract) and the
invariant induction of section 8, not a faster solver.

**Things in kernel C that `pir` could not encode, worked around in the stub layer** (all in
`linux_pre.h`/`linux_early.h`, a local stand-in for R2; each is an assumption in every report):

* inline asm in `BUG`/`WARN` (x86 `ud2` with a bug table), bit operations, barriers, `READ_ONCE` and
  friends (volatile) and atomics: replaced by plain C with the architectural meaning;
* `ptrtoint`/`inttoptr`: `ERR_PTR`/`IS_ERR`/`PTR_ERR` (a marker object; `PTR_ERR` yields any errno), and any driver
  that keeps a pointer in an integer (virtio-rng's `hwrng.priv`, adapted by a patch listed in the report);
  header inlines that use `ERR_PTR` (`virtio_find_single_vq`) are replaced;
* static initialisers with `inttoptr` constant expressions (`DEFINE_IDA`): replaced by a plain object;
* indirect calls are inlined once per address-taken function of the same IR signature; `void(ptr)` includes
  the driver's own `remove`, so a driver that calls `config->del_vqs` from `remove_common` recursed without
  bound (15 GB of memory before the first answer). The stubs and, for virtio-rng, three adaptations call
  such functions directly. This is a `pir` limit (candidate sets by signature only), to be fixed in the
  translator, not worked around per driver.

**Memory.** One run (virtio-rng, before the direct-call adaptation) used 15 GB; `PRISM_MEM_BUDGET_MB` turns
that into `UNKNOWN: memory budget reached`. Runs here were given a budget of 8 to 9 GB per process.

**What is only designed.** DMA ownership (section 2.5), the FreeBSD flavour (`bus_space`), the Lean
embedding (section 8), a replay of counterexamples as an executable trace (the counterexample is the list of
nondeterministic values on the violating path, `extra.nondet`, in call order, hard to read), several
devices per module, threaded irq handlers (the thread function is not run), power management beyond
freeze/restore, and `irq_points mmio` at scale.

**Who this does not help yet.** A driver whose module needs more than a dozen entry points, a real
datasheet-validated model, or an unbounded claim. The prototype answers "is the design sound and does it find
the planted and the hostile-device cases", which it does; it does not answer "can this scale to the 8,367
driver modules of the OS-Proving data", which is the next gate.
