# ltl `extract_fsm`

`extract_ltl_fsm` in `src/prism/stages/ltl.cpp` is the C++ owner of
`prism/ltl.py:extract_fsm`. The four extraction gaps in the
`docs/CPP_PORT_PLAN.md` ltl row are already implemented on this base
(`f67c14a23`, tightened in `5166c3a3e`). This slice does not add LTL
formulas, verdicts, or a second extractor. `docs/CPP_PORT_PLAN.md` is
outside this slice, so that appendix row is unchanged.

## Behaviours

- A machine comes only from the brace-matched body of `switch (state)`,
  `switch (p->state)`, or `switch (obj.state)` (any chain of `->` / `.`
  ending in the identifier `state`). `case` labels elsewhere in the
  function are not states. `switch (ev)` is not a plant.
- `state = (DEST)` and the same write through `p->state` / `obj.state`
  are edges to `DEST`. They are not replaced by a self-loop.
- `default:` destinations are the edges of states that have no named
  `case`. One named `case` plus `default` is a machine when those arms
  name at least two states.

Locked by `tests/cpp/test_ltl.cpp` (plants `fsm_paren` / `fsm_default` /
`fsm_ev_only`, and the inline case
"brace span ignores outside cases; (DEST) on state, p->state, obj.state is an edge").

## Assumptions

- The switch body is the `{...}` that follows the `)`, with no `;`
  between them. `{`, `(`, and `[` share one depth counter, after
  comments and string literals are blanked. An unbalanced bracket
  yields no span for that switch, so the formula stays `NOTRUN`
  rather than a machine read from a truncated body.
- A parenthesised destination is one pair of parentheses around a
  plain identifier or integer, and the assignment ends at `;`, `,`,
  or `)`. A call, a cast, a ternary, nested parentheses, or a
  compound update (`+=`, `++`) refuses the machine.
- A `default:` arm that can leave the switch without writing `state`
  also keeps that state (a self-loop), including `default: break;` and
  `if` without `else`. That is the stricter reading already in the
  stage: the transitions are a superset of the steps the code can take.
- A switch whose only label is `default:`, or a body that names only
  one state, is not a machine (`nullopt`). A write to `state` outside
  the switch braces refuses the machine as well.
