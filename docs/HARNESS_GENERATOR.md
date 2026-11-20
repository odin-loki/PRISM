# Harness generator for kernel and driver code

Status: Phase 1 (measure, design) and Phase 2 a to c (implemented, opt-in flags), Phase 3 (Lean).
Source: `src/prism/pir/harness.cpp` (planner), `translate.cpp` (`bind_auto_harness`,
`call_obligations`), `stage.cpp` (`discharge_pass`, `apply_vacuity`), `proofs/Prism/Discharge.lean`.
Base: `integration-wave1` (contains the automatic pointer harness, PR #50).

## 1. Measurement: why e1000e functions are still NEEDS-HARNESS

Linux `drivers/net/ethernet/intel/e1000e`, the 11 units of
`clang-work/subsets/compile_commands-linux-e1000e.json`, run with
`scripts/pir_batch.sh ... --prism-arg --kernel-api --prism-arg linux --prism-arg --assume-unknown-calls`
(`--pir-harness` is the batch default), then classified by script:
`prism-qa harness-classify OUT_DIR`. The classification reads the verdict message and the
assumptions of each JSONL record; nothing in it decides a verdict.

Run of 18 Nov 2026 on `integration-wave1` (binary identical in `pir` to `e2251b7`), 4 workers, 6000 MB per
worker, unit timeout 3300 s, a machine at load 50. 10 of the 11 units finished; `param.c` timed out (3300 s) with
and without the new flags, so 521 functions are counted.

| verdict | functions |
|---|---|
| NEEDS-HARNESS | 389 |
| UNKNOWN | 66 |
| PROVED-ASSUMING | 55 |
| BOUNDED | 5 |
| PROVED | 3 |
| TIMEOUT | 2 |
| FAILED | 1 |

354 of the results carry an `(auto harness)` assumption. Why the 389 NEEDS-HARNESS are not verdicts
(`prism-qa harness-classify`):

| why | functions | share of the 389 |
|---|---|---|
| function-pointer field called while it is arbitrary (`hw->mac.ops.x()`: "call through a null function pointer") | 121 | 31 % |
| arbitrary pointer field dereferenced or offset (depth bound 2, or no pointee type: MMIO bases such as `hw->hw_addr`) | 104 | 27 % |
| inline depth: `UNENCODED: call depth > 4` (no recursion in this driver) | 54 | 14 % |
| `UNENCODED: ptrtoint` (address values not modelled: `readl` and friends on integer handles, `IS_ERR`) | 48 | 12 % |
| integer UB on an arbitrary field value (overflow, shift, division) | 20 | 5 % |
| container_of (negative offset from a parameter) | 15 | 4 % |
| misaligned access under the harness (`MEM-MISALIGNED`) | 11 | 3 % |
| `inttoptr`, array index or size from a field, unknown call (`llvm.prefetch`), invalid free, one initializer, one extern object of unknown size | 16 | 4 % |

Reading: a third of the blockers are a function pointer that the framework fills in, another quarter a pointer
field with no object behind it. Both are framework facts (section 2.6), not object shapes. container_of, the case
the brief named, is 4 %. Of the 55 PROVED-ASSUMING results, 14 of 18 examined for the discharge (units
`manage`, `nvm`, `ptp`, `mac`) are exported functions, 3 are address-taken (ops-table targets): entry points.



## 2. Design

### 2.1 What the existing harness is

For a pointer parameter `p` of `f` the planner reads the IR of `f` (and of the module functions it
passes `p` to, three calls deep) and builds an object: type from the first `getelementptr`, size from
the layout (or `--pir-harness-elems` elements for an array-like pointee), nested objects for the pointer
fields `f` loads, down to depth 2 and 32 objects, fields arbitrary but initialised, 16-byte aligned,
distinct objects. Every choice is a line of `extra.assumptions` ending `(auto harness)`. The proof of
`f` is a proof under that precondition `P_f`. It is **invented**: nothing says any caller establishes
it. Hence at best `PROVED-ASSUMING`, and a violation under it is only `NEEDS-HARNESS`.

### 2.2 Assume-guarantee: the harness becomes a proof obligation (`--pir-discharge`)

The proof of `g` under `P_g` is a proof for **every** state satisfying `P_g`: it never looked at
anything else. So `P_g` is a precondition in the logical sense, and the standard rule applies:

* `g` is safe under `P_g` (what PRISM proved), and
* every call site of `g` establishes `P_g` (what the discharge checks), and
* no call of `g` can come from anywhere else,

then every invocation of `g` in the unit is safe. The Lean theorem is `Prism.Discharge.assume_guarantee`
(section 4).

**Obligation.** For each pointer parameter of `g`, each object of its harness plan (the root and the
nested ones) becomes, at each call site, the checks of class `HARNESS-OBLIGATION`
(`MemTr::obligation_object`, `obligation_distinct`):

| property | what it requires of the pointer the caller passes |
|---|---|
| `ob-null` | it points to an object (not null, not a wild value) |
| `ob-origin` | the object is a local, a heap allocation, or one of the caller's own automatic-harness objects: never a global and never an object PRISM assumed into existence (global harness, `current`, a field-invariant object, an unknown call's result). The harness object is fresh, distinct from every global, and the callee's proof may rest on that (a global it writes directly and reads back); a caller's harness object is accepted because the caller's own discharge checks the same rule one level up |
| `ob-dead` | the object is live |
| `ob-offset` | the pointer is at the harnessed offset (0, or the member offset for `container_of`) |
| `ob-size` | the object has at least the harnessed number of bytes |
| `ob-align` | the object is aligned to what `g`'s accesses need: the pointee type's ABI alignment, or the larger alignment of an over-aligned load or store (at most the harness's 16) |
| `ob-const` | the object is writable |
| `ob-init` | every byte of the object is initialised (objects over 1024 bytes: not checked, kept as a stated assumption with the `(auto harness)` marker) |
| `ob-distinct` | the objects of one call (the arguments, and the objects their nested fields point to) are pairwise distinct |

The nested objects are found by loading the pointer field from the caller's object and applying the same
checks to the loaded pointer. A pointer field the harness leaves arbitrary (depth bound, no type evidence)
gets no obligation: the proof of `g` did not depend on it.

**How it is checked.** Not by a new engine. `discharge_pass` (after the per-function results of a unit)
translates each caller `f` of `g` once, with `TranslateOptions::obligations = {g: plan}`: at every direct
call of `g` in `f`'s own body it emits the checks above and drops every other check, so the VC is exactly
the obligations, under `f`'s own state (its harness, if it has pointer parameters, as for any result of
`f`). The ordinary check (`check_function`) decides them. The call site is discharged when that is PROVED,
violated when it is FAILED (the message says which caller, which line, which property), undecided otherwise.
Loops that are not closed leave the result BOUNDED: not discharged.

**Soundness conditions** (all checked by `discharge_pass`; any failure keeps the assumption, with the reason
in `extra.harness_discharge`):

1. `g` is `internal`/`private` in the IR (a `static` function): no caller outside the unit.
2. `g`'s address is not taken: no occurrence of `@g` in the IR other than as the callee of a call, a
   definition or a declaration (covers function-pointer tables, `module_init` aliases, `llvm.used`); and `g`
   is not named in a module-level or inline `asm` string (assembly can call a symbol without taking its address).
3. All call sites are known: the direct calls in the unit's IR (including functions from headers).
4. No recursion: `g` does not reach itself in the call graph (a cycle keeps the assumption). PIR does not
   encode recursion today (`UNENCODED: recursive call`), so such a `g` is never a candidate; the check is
   there for when it does.
5. Every call site is PROVED (not BOUNDED) to establish the harness, including the bytes' initialisation.
6. `g`'s own result is `PROVED-ASSUMING` under the automatic harness (a `requires` clause or a draft on a
   parameter is a stated precondition, not an invented one: not touched).
7. The pass has a time budget (`PRISM_DISCHARGE_BUDGET`, 900 s); a call site not reached is not discharged.

**What verdict it permits.** `PROVED-ASSUMING`, never `PROVED`, as long as some caller's context is itself an
invented assumption (with `--pir-discharge-closure` a callee whose callers' contexts are all unconditional is
`PROVED`: section 6):

* the callers' own assumptions remain: a call site is checked under the state `f` runs in, which for an
  entry function (exported, address-taken, or not discharged itself) is its own harness, still invented.
  `g` therefore rests on the **entry assumptions** of the functions that reach it. The result lists them
  (`extra.discharge_entry`, and the assumption line names them);
* the closed-world fact (the unit's functions are all the callers) is an assumption of the claim even when no
  entry function has any harness.

What changes is the *kind* of assumption: the invented object shape of `g` is replaced by "discharged at N
call sites in F1, F2 ...", and the result no longer carries a line ending `(auto harness)` unless an
initialisation was over the cap. `jsonl` `harness.auto` becomes false for it. A reader can see that
what is left to believe is the entry functions' preconditions, which can be stated by a framework
contract (2.4), not an arbitrary shape.

### 2.3 container_of (`--pir-container-of`)

A parameter `p` used at a negative constant offset (`container_of(p, T, m)` compiles to a
`getelementptr i8, p, -k` followed by a `getelementptr %struct.T`) points to a member of a `T`. The planner
takes `T` from the struct-typed `getelementptr` based at the negative offset, allocates one `T`, and binds
`p` to byte `k` of it. It requires every offset used to lie inside the container (negative ones above its
start, positive ones below its end); otherwise it keeps the old refusal. The assumption names the
container type and the member offset. Nested pointer fields of the member (offsets relative to `p`) are
stored at `k + offset`. The call-site obligation is the same, with `ob-offset` = `k`: a caller passing a
standalone struct fails `ob-offset`/`ob-size`.

### 2.4 Entry points: a named entry assumption (designed, partly implemented)

An exported function, an address-taken one (ops-table target) and any function whose discharge failed keep
`(auto harness)`: an invented shape. The better source is the framework contract: PR #56 (`--ops-table`) gives
each ops-table field a role (a PCI `probe` gets a valid `pci_dev` and `pci_device_id`, `ndo_start_xmit` an
`sk_buff` and a `net_device`), and PR #52 (`docs/DEVICE_MODEL.md`) the module harness that calls entries in
the framework's order. The design: a role table `role -> (parameter -> type, size, init, nullability)` replaces
the planner's guess for that parameter, with the assumption line `(entry assumption: PCI probe(struct
pci_dev *), docs: ...)` instead of `(auto harness)`; the callees below it are then discharged against it by
2.2. Not implemented here: it needs #56's TSV and a table of role contracts that nothing in this branch can
verify; the hook is the parameter binding in `translate.cpp bind_auto_harness`.

### 2.5 Non-vacuity (`--pir-vacuity`)

A harness (or a `requires`, or a library model's assumption) that no execution satisfies makes every VC pass
and the function "PROVED". For every harnessed function whose result is `PROVED-ASSUMING` or `BOUNDED`,
`apply_vacuity` translates nothing new: it takes the function's PIR, removes every check, and adds one check
"a return is reachable" at each `Ret`. `FAILED` (a witness) = the assumptions are satisfiable and the end is
reachable: `extra.non_vacuity = "checked: ..."`. `PROVED` = no execution returns: the verdict becomes
`NEEDS-HARNESS`, `extra.non_vacuity = VACUOUS`, the old verdict kept in `verdict_before_vacuity`. Anything
else (BOUNDED loop, timeout): the verdict stays, `extra.non_vacuity = NOT ESTABLISHED`, plus
`extra.warning`. `Stop` terminators are not counted as ends: an `abort()` inside SV-COMP's
`assume_abort_if_not` is an assumption that ends the path there. A function that always ends in
`exit()`/`BUG()` is therefore reported vacuous: conservative, never unsound.

### 2.6 What the counts say is next (not built here)

The largest blocker in the measurement is not an object shape: it is a **function-pointer field** that the
harness leaves arbitrary (`hw->mac.ops.read_reg(hw, ...)`: the call through a null pointer is the "violation"
under the harness). The second is an **arbitrary pointer field** with no pointee type evidence (the MMIO base
`hw->hw_addr`, `hw->flash_address`, used only through `readl`/`writel`). Both are "the framework's
initialisation is not in the function": `e1000e_probe` fills the ops table, `ioremap` the base. Design:

* ops-table fields (PR #56 `--ops-table`, roles in its TSV): a field the module itself assigns in an
  initialiser (`e1000_init_function_pointers`) is bound to the set of functions assigned to it; a call through
  it is a call of each of them (inlined, or an unknown-call havoc under `--assume-unknown-calls`), with the
  assumption "the field was initialised by `<function>`" (a named entry assumption, not `(auto harness)`).
* MMIO bases: an `__iomem` pointer field with no pointee type gets a device-model object (PR #52,
  `docs/DEVICE_MODEL.md`): the kernel specs already model `readl`/`writel` on it, so the object only has to
  be a valid mapping of the device's BAR size.

Neither is in this branch: each is an assumption about the framework that nothing here can check, and it needs
#56/#52 to land first. The discharge (2.2) then applies to the helpers below them unchanged.

Phase 2 (`docs/HARNESS_PHASE2.md`) builds the first of these (`--pir-ops-auto`: the table is derived from the unit's own
stores and initialisers, and a harness function-pointer field is bound to one of its targets as a named assumption),
contract stubs for body-less callees (`--callee-contracts`), constant `ptrtoint` / `inttoptr` (`--pir-addr-values`) and a
deeper harness with the bound recorded (`--pir-harness-deep`). The MMIO-base object is still not built.

## 3. Implementation summary

| flag | effect | tests |
|---|---|---|
| `--pir-discharge` | 2.2 | `pir discharge:` doctests, `testdata/pir/harness_discharge.c` |
| `--pir-container-of` | 2.3 | `testdata/pir/harness_container.c` |
| `--pir-vacuity` | 2.5 | `testdata/pir/harness_vacuity.c` |

All three are opt-in (`scripts/pir_batch.sh --prism-arg ...`); without them the output is unchanged.

Known limits, each a reason a discharge fails (never an unsound discharge):

* `ob-init` is strict: every byte, padding included. A caller's struct with padding bytes that were never
  written fails. The proof of `g` assumed the padding initialised too (it could be `memcmp`ed). Lifting it
  needs a scan showing neither `g` nor its callees touch the object as bytes.
* Scalar-array pointees are harnessed as 16 elements by default; a caller passing `&x` fails `ob-size`. That
  is the harness being stronger than the caller, as intended.
* The check is per caller, not per call context: a call of `g` inside an inlined callee `h` is covered by
  `h`'s own pair, not re-checked inside `f`'s translation.
* Only `PROVED-ASSUMING` callees are rewritten; a callee that is `NEEDS-HARNESS` because of the harness
  (field values) is not improved by this: the caller's real state could show it safe, but that is a different
  rule (inline-based) not built here.

## 4. Lean (`proofs/Prism/Discharge.lean`)

* `Prism.Discharge.assume_guarantee`: for a module given as `own f s` (the function's own checks pass when
  entered in state `s`) and `calls f s` (the call sites reached with their states), a precondition `pre`
  and the set `entry` of functions callable from outside: if every function is safe under `pre`
  (`h_own`) and every call site reached under the caller's `pre` establishes the callee's (`h_site`), then
  every reachable invocation satisfies `pre` and its own checks pass. `internal_pre_holds` is the corollary
  for a non-entry function. `site_check_needed` shows the call-site hypothesis cannot be dropped.
* `Prism.Discharge.obligation_iff`: the set of properties violated at a call site (`violations`, the model
  of `obligation_object`) is empty exactly when the harness's object exists (`Pre`: live, writable, at the
  offset, the size, the alignment, initialised). `obligationProps` lists the property names; the doctest
  "the obligation names in the Lean model are the properties the code emits" compares the list with
  `translate_mem.cpp`.
* Axioms: `proofs/Prism/Axioms.lean` prints them; no `sorry`, no axiom beyond `propext`,
  `Classical.choice`, `Quot.sound` (checked by `proofs/check.sh`).

What the theorem does not say: that the C++ translation of a call site, or the memory model's answers, are
right. The first is the same trusted translation as every PIR result (and the translation validation does
not replay pointer functions); the second is the memory-model correspondence (`proofs/semantics`).
`obligation_iff` is about the shape of the check, matched to the code by name and by the tests.

## 5. Measurement after the three flags

Same command plus `--pir-discharge --pir-container-of --pir-vacuity`, same 521 functions
(`prism-qa harness-classify`, `diff` of the two runs by function):

| | before | after |
|---|---|---|
| NEEDS-HARNESS | 389 | 386 |
| UNKNOWN | 66 | 69 |
| PROVED-ASSUMING | 55 | 56 |
| BOUNDED / PROVED / FAILED | 5 / 3 / 1 | 5 / 3 / 1 |
| TIMEOUT | 2 | 1 |
| results carrying `(auto harness)` | 354 | 358 |
| harness discharged at the call sites (no `(auto harness)` left) | 0 | 1 |
| NEEDS-HARNESS for container_of | 15 | 0 |

* **container_of:** all 15 now get their container. 2 became PROVED-ASSUMING; 8 moved to `UNENCODED: ptrtoint`
  (the next blocker in the same functions), 2 to integer UB on a field value, the others to global state,
  inline depth, an arbitrary pointer field.
* **Discharge:** 1 result (`__e1000e_disable_aspm`, netdev.c) lost its invented assumption. Of the 18
  PROVED-ASSUMING results in `manage`, `nvm`, `ptp`, `mac` (run with the reasons recorded):
  14 exported, 3 address-taken, 1 call site undecided. e1000e's proved functions are its entry points; its static
  helpers are blocked earlier (function-pointer calls, pointer fields). The mechanism is exercised on the
  testdata (`harness_discharge.c`: discharged, NULL, too small, uninitialised, aliased, exported, address-taken).
* **Vacuity:** no e1000e result was vacuous (no PROVED-ASSUMING became NEEDS-HARNESS for it).
* UNKNOWN +3, TIMEOUT -1 and one PROVED-ASSUMING to UNKNOWN are solver time under the load of the machine (the
  vacuity re-check also costs one more check per harnessed result), not verdict changes by the new code.

What this does not show: that the generator "covers far more functions". On e1000e it does not: the 15
container_of functions are now analysed but the 389 are dominated by what 2.6 describes. It shows that the
harness can be made sound where the callers are in the unit, and what is needed to go further.


## 6. Whole-build call-site discharge (`--pir-discharge-closure`, `--pir-discharge-out`, `prism-qa callsite-discharge`)

Section 2.2 replaces the invented harness of `g` by checks at its call sites, but leaves `g` resting on the
assumptions of the contexts those checks ran in. This section closes the argument where it can be closed: when every
context is itself unconditional, nothing is left and `g` is `PROVED`. Source: `src/prism/pir/discharge.cpp`
(call graph, grounding, whole-build merge), `stage.cpp discharge_pass`, `src/tools/qa/callsite.cpp`.

### 6.1 Grounding

A call-site check of `g` in a caller `f` is a translation of `f` (its own harness, if it has pointer parameters, and
every other assumption of `f`'s context: unknown calls, kernel models, ...) in which only the obligations are
properties. Its result is a `site` with the list of assumptions the translation made. Write `P_x` for the harness of
`x`. Then `P_g` holds at every invocation of `g` when:

1. `g` is **closed**: every caller is known (below);
2. every site of `g` is PROVED;
3. for every site whose translation rested on `f`'s own automatic harness (`caller_harness` non-empty), `P_f` holds
   at every invocation of `f`, by the same rule (recursively). A caller without an automatic harness has an
   unconditional context: nothing about how it is entered is assumed.

This is the least fixed point: a cycle (recursion through the callers) is never grounded. `Grounder::ground` computes
it and collects the **inherited** assumptions: every assumption of every site on the way, except the callers' own
harness lines (discharged by the rule). The result of `g`:

* `PROVED` (or `PROVED-UNBOUNDED`, from `extra.verdict_before_assumptions`) when `g` is grounded, its own assumptions
  are only harness lines, no harness part was left unchecked (initialisation over the 1024-byte cap), nothing was
  inherited, and no proof in the composition used an object PRISM assumed into existence (below). The message says
  "discharged at every call site ..., each checked in an unconditional context: nothing is assumed";
  `extra.verdict_before_discharge` keeps the PROVED-ASSUMING it replaced.
* otherwise `PROVED-ASSUMING`, with the harness lines replaced by "discharged and grounded ..." and the inherited
  assumptions listed as "at a call site (caller's context): ...".

Lean: `Prism.Discharge.grounded_safe` (if every entry's precondition holds in every state, every reachable
invocation satisfies its precondition and is safe) and `entry_pre_needed` (an entry with a non-trivial precondition
can be entered where it fails); `obligation_iff` now includes `ob-origin`.

**Aliasing.** A harness object is fresh: the proof of `g` took it to be distinct from every global and from every
other object the model allocates. Two rules keep that true after the discharge:

* `ob-origin` (section 2.2 table): the argument is a local, a heap allocation, or one of the caller's own harness
  objects (accepted because the caller's discharge checks the same rule). A global or an assumed object fails it.
  `testdata/pir/harness_callsite.c` `leaf_global` is the example: with the global as argument the division is by zero,
  and the discharge is refused.
* objects PRISM assumes into existence (pointer results of unknown and indirect calls, asm outputs, the global harness,
  `current`, field-invariant objects, external objects of library models: `Function::invented_objects`) are also fresh
  in the model, while the real object may be an argument. A composition that used any such object states it
  ("objects PRISM assumes into existence ... are distinct from the objects the callers pass") and is never `PROVED`.

**One call, no model of the callee.** When the caller has exactly one call of `g`, no module function it may inline
calls `g`, it makes no indirect call, the call is on no cycle of its control-flow graph and it does not use `setjmp`,
the site check ends the path at the call (`TranslateOptions::obligation_stop`). The call is reached at most once per
execution, so nothing after it is an obligation, and `g` (possibly defined in another unit) is neither inlined nor
havocked. Otherwise `g` is inlined (defined here) or handled as any declared callee (its unknown-call assumption is
then inherited). The obligations are emitted at every call of `g`, defined in the unit or only declared.

### 6.2 Closed world

* **In one unit** (`--pir-discharge-closure`): `g` is `internal`, its address is not taken, it is not named in
  assembly, it is not recursive and it has a call site. External functions are never closed here.
* **Over a whole build** (`prism-qa callsite-discharge --link linux --complete-db`): additionally an external function
  `g` of unit `U` is closed when it is not exported and all of its link unit is analysed. The link unit is the
  database entry's `link_units` (the build's own link map, e.g. from the kbuild `.mod` files:
  OS-Proving `tools/prism_clusters/link_map.py`), else read from the compile command: `-DMODULE` with
  `KBUILD_MODNAME` (several names joined by `:` for an object linked into several modules) is that module, anything
  else is `vmlinux`; a module object whose module is unknown (`module:?`) counts as a member of every module. Linux
  resolves a module's undefined symbols only against exported ones, so a non-exported external symbol is called only
  from its own link unit. The databases (`--db`, repeatable: a C database and one of the assembler objects, say) must
  list every object of the build: `--complete-db` says so, and without it no external function is closed (a database
  filtered to C, like most `compile_commands.json` files, cannot show the assembler and Rust objects of a module).
  Refused (the assumption stays, the reason is recorded) when: `g` is weak or linkonce; it is exported or otherwise
  address-taken in any unit of the link unit (`EXPORT_SYMBOL` takes the address through `__ADDRESSABLE` and names the
  symbol in asm); it is named in any asm string of the link unit; the link unit has an assembler or other non-C unit
  (`.S`, `.s`, `.rs`), which can call any symbol; a unit of the link unit has no call-graph facts (not run, or the front
  end failed: its calls are unknown); a unit is compiled more than once; the name has more than one external definition
  in the link unit; there is no call site.
  `--link none` (the default, and the rule for FreeBSD, whose kernel linker resolves a module against every global
  kernel symbol) closes only static functions.
* **Indirect calls** are recorded in the call graph (`callgraph.tsv`: `indirect-field` with the struct field and its
  ops-table targets when the table names them, `indirect-vtable`, `indirect-pointer`), but an address-taken function
  is never closed: an indirect call through a pointer the analysis cannot bound may reach it.
* What the closed world does not cover: code outside the configuration (other configurations are separate builds,
  each decided separately), out-of-tree modules (they can reach only exported symbols, which stay assumptions) and
  livepatch modules (which may relocate against non-exported symbols; outside the scope of a build's claim).

### 6.3 Running it over a build

Every run needs `--pir-harness --pir-discharge --pir-discharge-out FILE` (the runner's flags otherwise unchanged):

1. **Pass 1**: the units of the build, as for any run. Each unit appends its call graph (`callgraph`), the harness plans
   of the functions in its discharge work list (`plan`), the call-site checks it could do (`site`, including the
   in-unit callers of its external functions) and its PROVED-ASSUMING harnessed functions (`candidate`).
2. **Facts** for units of the link units not run in pass 1: the same command with `PRISM_PIR_ONLY` matching no
   function (front end and call graph only).
3. `prism-qa callsite-discharge --in DIR --db compile_commands.json [--db asm.json] --complete-db --link linux --config NAME --out OUT` writes
   `OUT/decisions.jsonl` (per candidate: `level` `proved` / `grounded` / `discharged` / `none`, the verdict, the
   assumptions, the reason), `OUT/todo.jsonl` (per unit: the plans of other units' functions to check at its call
   sites, `callers`, or `facts: true`), `OUT/callgraph.tsv` and `OUT/summary.json`.
4. **Pass 2** for each `todo` unit: `--pir-obligations PLANS.jsonl` (its `plans`), with `PRISM_PIR_ONLY` matching no
   function. The unit checks the plans at its call sites, and its callers' own harnesses up its call graph (new
   plans, new sites). Repeat 3 and 4 until `todo` is empty or stops changing.

`decisions.jsonl` is a new claim (record `callsite-discharge`), composed of pir results that are each a solver verdict
(the candidate's own proof and every site check), under the theorem above; no recorded row is rewritten. A consumer
that labels functions takes `level: proved` as `PROVED` for that configuration, and keeps the row's own
PROVED-ASSUMING otherwise.

**Mined field invariants are never discharged.** `--pir-field-invariants` rows are value assumptions about every
object of a struct, mined from stores. They are not harness lines: the discharge leaves them in the list, so a result
that used one stays `PROVED-ASSUMING`, and a site check that ran under one passes it on as an inherited assumption.
Whether a mined invariant holds needs a whole-build store analysis that sees every write of the field (memcpy,
`char *` and `void *` stores, casts, unions, uninitialised `kmalloc` memory, other modules and configurations); PRISM
does not have one, and the call-site check of a field value would still rest on the caller's own harness, which the
same rows constrain.

### 6.4 Measured (20 Nov, indicative: pre-merge binary)

The 1,225-row PTRPARAM sample of the r1 clusters (OS-Proving `data/coverage/prism_callsite_r1/`, driver
`tools/prism_clusters/callsite_measure.py`), r2 flags of #65/#69/#70 plus `--pir-discharge-out`, Linux link map from the
build's `make -n` log, two merge rounds: 232 rows are PROVED-ASSUMING under the automatic harness; **0 become PROVED**;
23 have their harness discharged at every call site but are not grounded; 209 are not discharged. What blocks them, in
order: 208 of the 232 rest on other assumptions no call site discharges (unknown calls 121, asm-havoc 77, kernel logging,
MMIO, locking models, kernel-stop); 124 are address-taken (ops tables, IRQ handlers, EXPORT_SYMBOL) and 8 more sit below
an address-taken caller; 51 call-site checks are undecided (UNKNOWN, TIMEOUT, BOUNDED) and 10 not encodable; 11 call sites
really break the harness (ob-offset, ob-size, ob-null, ob-init). The 23 discharged rest on address-taken or undecided
callers. The next step is therefore entry contracts for address-taken roots (2.4), not more discharge.
