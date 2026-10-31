# bmc retirement record (gate g9)

`pvb9.txt` is the final `prism-qa pir-vs-bmc` report that cleared bmc for
retirement. bmc and pir-vs-bmc were deleted right after this record.

- Date: 2026-10-31. Directories ran 09:42-12:51 AEDT; merged 12:51 AEDT.
- Commit: f8410b376 on prism-pir-parity. That tree was built at 09:31 AEDT
  and committed unchanged at 09:48. The prism-run memory child budget
  (0720ed316) came later and was not in this binary.
- Budget: PRISM_MEM_BUDGET_MB=2000, PRISM_RUN_RESERVE_MB=512, --jobs 1,
  stages inventory,classify,bmc,pir. libc-models also ran with
  PRISM_Z3_CHILD_NODES=0.
- Superset gaps (bmc PROVED*, pir absent or NEEDS-HARNESS): **0**.
- Hard conflicts (PROVED* vs FAILED): **0**. Gate: PASS.
- These are not gaps under the gate's definition. Where bmc reported
  PROVED*, pir reported BOUNDED for 7 functions and UNKNOWN for 3. Neither
  is a contradiction.

Peak tree RSS per directory (prism-run):

| directory | peak |
|---|---|
| fp | 202 MB |
| pir | 159 MB |
| tp | 30 MB |
| conformance/concurrency | 16 MB |
| conformance/esbmc-cpp | 412 MB |
| conformance/libc-models | 2385 MB (over budget; verdicts counted by decision, overshoot tracked as a bug) |
| conformance/prism | 1900 MB |
| conformance/sv-comp | 538 MB |
| planted | 199 MB |
