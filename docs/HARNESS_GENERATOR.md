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
   definition or a declaration (covers function-pointer tables, `module_init` aliases, `llvm.used`).
3. All call sites are known: the direct calls in the unit's IR (including functions from headers).
4. No recursion: `g` does not reach itself in the call graph (a cycle keeps the assumption). PIR does not
   encode recursion today (`UNENCODED: recursive call`), so such a `g` is never a candidate; the check is
   there for when it does.
5. Every call site is PROVED (not BOUNDED) to establish the harness, including the bytes' initialisation.
6. `g`'s own result is `PROVED-ASSUMING` under the automatic harness (a `requires` clause or a draft on a
   parameter is a stated precondition, not an invented one: not touched).
7. The pass has a time budget (`PRISM_DISCHARGE_BUDGET`, 900 s); a call site not reached is not discharged.

**What verdict it permits.** `PROVED-ASSUMING`, never `PROVED` (VERDICTS.md: a rewrite never strengthens a
formal verdict, and a result resting on an invented assumption is not `PROVED`):

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

