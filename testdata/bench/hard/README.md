# Hard pir benchmark

Slow, kernel-shaped functions for measuring the pir stage (not a conformance
suite: the `_ok` / `_bad` suffix says whether a violation was planted, but
the verdict a function should get also depends on bounds, e.g. BOUNDED for a
loop PRISM cannot close).

| file | what |
|---|---|
| `proto.c` | protocol state machines driven by an input array (framer, TCP-like events, lexer) |
| `ring.c` | ring buffers with head/tail indices, bounded and unbounded simulations |
| `tlv.c` | TLV / varint / header parsers over byte buffers with `\valid_read` contracts |
| `arith.c` | 64-bit multiply, shift, hash and modular arithmetic, nested loops |

Run: `build/prism testdata/bench/hard --preset prove -j N --out OUT --solver-cache FRESH`
(fresh cache, and `PRISM_PIR_FN_CACHE=0`, to measure solving rather than the caches).
Measured 15 November 2026, 4 cores shared, `-j 2`: 552 s wall; Houdini 513 s over
28 runs and VC solving 489 s over 287 queries dominate; encoding 10 s.

Tried and rejected (15 November 2026): Houdini's solvers created with Z3's
`QF_BV` logic instead of the default solver. On `ring.c` + `proto.c` Houdini
took 872 s instead of 496 s and `tcp_events_ok` fell from PROVED-ASSUMING to
BOUNDED: slower and a lost verdict.
