# Driver environment models (`--pir-callback-models FILE`)

A driver callback (a `probe`, a `net_device_ops` method, an interrupt handler, a sysfs `show`) is called by the kernel
core, and the core guarantees things about its arguments that nothing in the driver's own code states: a PCI `probe`
gets the `pci_device_id` entry of the driver's own table that matched, a `write` count never exceeds `MAX_RW_COUNT`, the
`net_device` a driver's `ndo_*` methods get is the one the driver allocated with its private area behind it, a sysfs
`show` writes into a `PAGE_SIZE` buffer. Analysed one function at a time, PRISM sees none of this.

`--pir-callback-models FILE` gives these preconditions to the `pir` stage. Each one is a **named assumption**
(`driver environment model: <interface> callback <name> (@fn, parameter i): <condition> (from <source>; status S; not
checked here)`), so a verdict that uses one is at best `PROVED-ASSUMING`, and it lists the model it rests on. Off by
default; nothing changes without the option.

The models are adapted from the environment model specifications of **Klever** (the Linux Driver Verification
toolset of ISP RAS, <https://forge.ispras.ru/projects/klever>, paper <https://arxiv.org/abs/2309.16427>), which are
licensed under the Apache License 2.0. The rows of a models file name the Klever specification each one comes from;
rows that name a kernel document instead are PRISM's own, derived from that document. PRISM itself ships no Klever
specification text: the models file of a kernel build (OS-Proving `data/coverage/driver_env_models.tsv`) carries the
attribution and the licence notice in its header.

## Which functions a model applies to

A row names a callback kind by where the unit puts the function:

* `container` `%struct.T`, `field_off` N: the function's address is stored at byte N of a `T`, in a constant initialiser
  (`static const struct pci_driver drv = { .probe = foo_probe }`, nested initialisers too) or by a store in the unit
  (`INIT_WORK` stores `work->func`). `field_off` `*`: any field of `T`.
* `container` `call:FN`, `field_off` N: the function is argument N (0-based) of a direct call of `@FN` in the unit
  (`request_irq(irq, handler, ...)`: `call:request_irq`, 1).

A function the unit does not register that way gets no model, whatever its name or signature. The byte offsets are
those of the kernel build being analysed (randstruct, config options): OS-Proving `tools/driver_env_models/resolve_models.py`
computes them from the build's own compile command.

## Kinds

| kind | a, b | assumption |
|---|---|---|
| `range` | lo, hi | the integer parameter is in [lo, hi] (signed, at its width) |
| `id-entry` | byte offset of the id table field in the container | the pointer parameter points to entry 0 .. n-2 of the array that the container instance's field points to (the last entry is the terminator). Applied only when the unit has the container instance as a constant and every instance naming the function names the same constant table in the unit |
| `trailing-alloc` | `fn:arg,...`, struct type | the parameter's object is a `b` allocated by this driver with its private area: its size is exactly `sizeof(b)` plus the constant argument `arg` of the unit's calls of `fn` (all of them must agree). Applied only when the automatic harness types the parameter as one object of `b` (not an indexed array of it); the harness's own size (which it may have grown to cover the accesses it saw, `2 x %struct.net_device`) is replaced by the allocation's, so an access past the private area is still out of bounds |
| `buffer` | size | the pointer parameter points to the start of a fresh buffer of `size` bytes, arbitrary initialised contents |

## Soundness

* A model only narrows the inputs the function is checked for. A violation that does not depend on it is still found:
  a scalar callback with a division by zero for an input inside the range stays `FAILED`
  (`tests/cpp/test_pir_callback_models.cpp`, `testdata/pir/callback_models.c`).
* A pointer bound by a model (`id-entry`, `trailing-alloc`, `buffer`) is an object the model invents, like the automatic
  harness: a violation found under it is `NEEDS-HARNESS` ("violation only under a drafted or automatic harness"), never
  `FAILED`, and never hidden.
* A row that matches but cannot be applied (no single constant id table, allocations that disagree on the private
  size, another struct type) adds nothing: the function is checked as without the option.
* The rows are not checked by PRISM; their status column (`UNVALIDATED` for all rows today) is shown in every assumption.

## File format

TSV, 10 columns; `#` lines and a header line starting with `interface` are skipped:

```
interface  container  field_off  callback  param  kind  a  b  status  source
pci  %struct.pci_driver  16  probe  1  id-entry  8  -  UNVALIDATED  Klever spec drivers/pci/pci event spec.json (...)
```
