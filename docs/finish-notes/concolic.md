# concolic args and tried

`docs/CPP_PORT_PLAN.md` (`prism/concolic.py`) asked ERROR rows and
`skip-pointer` NEEDS-HARNESS rows to carry `extra.args` and `extra.tried`.

`put_args_tried` writes that pair in the crash path's spelling of
`extra.args`: `name=value`, comma-space, map order. `extra.tried` is the
decimal count of seeds offered. ERROR and skip-pointer keep the pair
alone (no `generated`, no `oracle`).

- ERROR (`goto unencoded` on `with_goto`): `tried=1`, `args=x=0`. Status
  stays ERROR.
- Pointer parameters stay NEEDS-HARNESS (Law 6). The kind gate returns
  before the body runs. It records the zero seed the execute
  `skip-pointer` arm attaches (`null_branch`: `tried=1`, `args=p=0`).
  The in-loop `skip-pointer` arm uses the same helper.
- CLEAN keeps the run stats. CRASH keeps the counterexample and those
  stats. PROVED and BOUNDED stay separate statuses; this stage's rows are
  CLEAN, CRASH, ERROR, and NEEDS-HARNESS.
