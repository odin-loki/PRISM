# Datamined methods (what Helix actually implements)

This is not a bibliography. Each section is an algorithm we run, named
for the project it came from, with the honesty constraints ParanoidBSD
forced on the encoding.

## ESBMC — bounded model checking + k-induction

ESBMC: Clang → GOTO → SSA → SMT (Boolector/Z3/…). Properties are
claims. CBMC turns checks *on* with `--bounds-check`. ESBMC turns them
*off* with `--no-bounds-check`. Passing the CBMC-shaped flag to ESBMC
disables the check and still prints VERIFICATION SUCCESSFUL.

Helix BMC:

1. Parse a SCALAR (or VOID) function into a statement list.
2. Encode each `int` as a Z3 32-bit bitvector (64 for `long long`).
3. Incremental BMC: unwind loops `k = 1, 2, 4, …, K`. First SAT is
   `FAILED` at that k (`extra.incremental_k`). A closed proof at a
   smaller k is `PROVED-UNBOUNDED` without trying larger k.
4. At each k, assert: no overflow, no div0, no shift UB, no OOB, all
   `assert` hold, and if unwind assertions are on, no loop exceeded k.
5. SAT → `FAILED` + counterexample (concrete input).
6. UNSAT + unwind closed → `PROVED` (bounded). Loop-free or closed
   loops are `PROVED-UNBOUNDED` for the encoded UB properties.
7. K-induction (when BMC is `BOUNDED`): extract loop-free `while` /
   `for` / `do` bodies, havoc locals, assume the guard, execute k=1
   then k=2 concatenated iterations. SAT on a havoced step stays
   `BOUNDED` — it is not a counterexample of the original function.
   UNSAT is `PROVED-UNBOUNDED` (`extra.k_induction_k`) and is never
   folded down into `PROVED`/`BOUNDED`. Nested loops are `unencoded`
   and stay `BOUNDED`.
8. C subset includes `do`/`while`, `continue`, comma operator, `sizeof`,
   and ternary `?:`. `goto` is unencoded `ERROR`, never a proof.
   Unsigned parameters use unsigned compares, unsigned index bounds
   (`UGE` not `slt < 0`), and wrap on `+`/`-`/`*` (not `INT-SIGNED-OVF`).
   `long long` / `int64_t` are 64-bit bitvectors; mixing widths sign/zero-extends.
   Nested loops are encoded by the unwind product; the k-induction step
   still refuses nested bodies (stays `BOUNDED`, never a fake unbounded proof).
   A VLA is `NEEDS-HARNESS` (missing bound), never a closed proof.
   OTHER (struct-by-value / unknown typedef) is `NEEDS-HARNESS`, not ERROR.
   A recursive self-call is `NEEDS-HARNESS`: an unconstrained BitVec is
   not a proof of the callee, and overflow on that havoc is not a cex
   of the original. IEEE `float`/`double` is `NEEDS-HARNESS`, not a
   bitvector proof. Libc `strcpy`/`strcat`/`sprintf`/`snprintf`/`gets`
   are `NEEDS-HARNESS` in BMC (unconstrained call is not a buffer
   proof); the concrete oracle crashes `strcpy` overflow as
   `MEM-OOB-WRITE`. C++ `throw` is `NEEDS-HARNESS`, never a parse ERROR
   dressed as a closed proof. `alloca`/`__builtin_alloca` is `NEEDS-HARNESS`
   (unmodeled stack frame). `setjmp`/`longjmp`/`va_list`/`va_start` are
   `NEEDS-HARNESS`, never a parse ERROR; `goto` stays ERROR and is not
   that case. Inline `asm`/`__asm__`, `_Generic`, GNU `({`, C++
   `try`/`catch`, and `offsetof` are `NEEDS-HARNESS` (missing model).
   Vacuous void `printf` with no encoded UB properties is `NEEDS-HARNESS`.
   `volatile`/`_Atomic` decls and `pthread_mutex_t`/`mtx_t` objects are
   `NEEDS-HARNESS` (missing memory/lock model), never a parse ERROR.
   `const int` locals, `struct S s`, unknown typedef locals, function-local
   `static`/`extern`, GNU `__auto_type`, anonymous `enum { } e`, `_Alignas`,
   and compound literals `(int){n}` are `NEEDS-HARNESS`; `const int`
   parameters and named `enum { NAME = val }` constants still prove.
   `memcpy`/`memmove`/`mkstemp`/`tmpnam`/`chroot` and `char t[] = "…"` /
   call-site address-of (`&n`) are `NEEDS-HARNESS` in every engine;
   `strcpy` stays BMC-only so the concrete oracle can still CRASH.
   Postfix `--`/`++` is in the concrete oracle (INT_MIN decrement is
   `INT-SIGNED-OVF`). Static same-file SCALAR callees are inlined one
   level before BMC (unconstrained calls are not a proof of the callee).
9. Cannot parse / POINTER without harness → `ERROR` / `NEEDS-HARNESS`.
10. No Z3 → `NOTRUN`.

When `esbmc` is on PATH the adapter runs it too. Disagreement is the
point. The two verdicts are never merged.

## FuSeBMC — BMC seeds a fuzzer, coverage feeds BMC

1. Only SCALAR functions get a harness. POINTER is `NEEDS-HARNESS` with a reason,
   not an ERROR and not a NULL crash dressed as a finding.
2. Harness: `fread` sizeof(args) bytes from stdin, call F, return.
3. Compile with gcc. ASan/UBSan if the compiler has them, else bare.
4. Seeds: BMC counterexample bytes first, then LLM seeds (Fuzz4All),
   then random.
5. Mutate (CUDA havoc when the kernel built, xsimd / Python otherwise).
6. New coverage or a crash → keep. Crash → `CRASH` (certainty).
7. Coverage edges that BMC has not seen become new goals (assume
   prefix, re-BMC).
8. Budget exhausted, no crash → `CLEAN`. Not a proof.
9. Could not compile → `ERROR`. No seed from BMC → still fuzz, but the
   record says `NOSEED` for the BMC half.

## Fuzz4All — LLM as a universal generator

1. Autoprompt: Qwen reads the source + a user intent (default: "find UB
   and crashes") and distills a *fuzzing prompt* — the generator spec.
2. Generate N candidate inputs in the language of the harness (raw
   bytes described as hex, or C literals, or grammar snippets).
3. Filter: parseable / right length / compiles if it is source.
4. Keep inputs that produce new coverage.
5. Mutate survivors by asking the model to "produce a close but
   different input that still parses", plus deterministic havoc.

## ChatFuzz — LLM mutation when greybox stalls

1. Track coverage-new rate over a window.
2. On stall: send the current interesting seed + the function source +
   "this input no longer finds new edges" to Qwen.
3. Ask for semantically valid mutants (respect types, not just bit
   flips).
4. Resume greybox on those mutants.
5. If the model is down, stall handling is `NOTRUN` for the ChatFuzz
   half; greybox continues.

## Dafny — specs as artefacts, VCs as the proof obligation

We do not translate C into Dafny (the Dafny adapter does, when `dafny`
exists). We steal the *shape*:

```
requires  P(args)
ensures   Q(args, result)
invariant I(state)
decreases D(state)
```

Qwen proposes these as HYPOTHESIS. Helix then:

1. Encodes `requires` as BMC assumptions (the harness).
2. Encodes `ensures` as BMC assertions after the call.
3. Encodes `invariant` + `decreases` as k-induction obligations.
4. A proof of `ensures` under `requires` is `PROVED-ASSUMING`, never
   `PROVED`. The assumption is in the record.

## Strix — LTL safety for protocol-shaped code

Full Strix solves parity games and emits a Mealy machine. Helix takes
the safety fragment that is decidable without that machinery:

1. Extract a finite state machine from `switch (state)` / enum modes.
2. Parse LTL safety: `G p`, `G (p → X q)`, `G (req → F ack)` bounded.
3. Build a deterministic monitor automaton.
4. BMC the product, or model-check the extracted FSM.
5. Synthesis (emit a missing transition table) is HYPOTHESIS until the
   product proves.

When `strix` is on PATH, LTL that is not safety is handed over.

## RLEF — reinforcement from execution feedback

No training. The *loop*:

1. Candidate = source, or a Qwen patch.
2. Reward = compile (0/1) + tests passed + sanitizer-clean + BMC
   status (`FAILED` is negative, `PROVED` is terminal success).
3. Keep the highest-reward candidate.
4. On failure, feed compiler/sanitizer/BMC output back as the next
   observation.
5. Stop on proof, on crash-free tests under budget, or on budget.

## OpenCodeInterpreter — generate, run, refine

1. Qwen writes a harness or a test, not a lecture.
2. Sandbox: tempdir, gcc, timeout, no network, CPU/RAM cap.
3. stdout, stderr, exit, signal go back into the chat *append-only*
   (prefix cache).
4. Repeat until the interpreter says the test passed or the budget
   ends.
5. The artefact is the last program that ran, plus the transcript.

## ParanoidBSD — the instruments that already found real bugs

Helix calls `tools/verify/` when the tree is present, and ships
portable copies of the shapes that do not need FreeBSD headers:

- lock released on some returns and not others
- switch over a bitmask with too few arms and an uninit escape
- pointer used inside the branch that proved it NULL
- `p = realloc(p, n)`
- `1 << 31` into a signed int
- integer `/` or `%` by a value that can be zero
- one-sided index (`i > n` with signed i)
- `return &local` (MEM-STACK-ESCAPE)
- unbounded `strcpy`/`sprintf` into a local array
- param_premise: a BMC `FAILED` cex that names a parameter is `named`
- sibling-guard (CTRL-SIBLING-ASYMMETRY): integer param guarded in one
  same-file sibling, used as a subscript in another without a test.
  Both missing is not a finding (that is the model checker’s job).
- discarded `malloc`/`fopen`/`open` result (API-IGNORED-ERROR)
- `fopen`/`open` closed on some returns only (RES-FD-LEAK)
- local VLA `T a[n]` (MEM-VLA-SIZE); BMC will not pretend a proof
- heap freed on some returns only (MEM-LEAK)
- `strlen`/`strcpy`/`strcmp` on an unchecked pointer param (STR-NULL-ARG)
- `std::move` then use (CXX-USE-AFTER-MOVE)
- local pointer declared then `*p` before assignment (PTR-UNINIT)
- uninitialised scalar used as `if (x)` (UNINIT-BRANCH)
- comment claims increment, body returns the identifier unchanged (INTENT); LLM stays READS and never COVERED by itself
- `delete` then copy from another object without `&self == &o` (CXX-SELF-ASSIGN)
- `malloc` later `delete`, or `new` later `free` (MEM-MISMATCHED-FREE)
- pointer param indexed by int param with no null/bound guard (MEM-PTR-ARITH)
- divide by `0.0` / `0.0f` (FLOAT-UB)
- two functions lock the same pair in opposite order (LOCK-ORDER)
- constructor / `*ctor*` body calls a `virtual` method (CXX-VIRTUAL-IN-CTOR)
- `access`/`stat` then `open`/`unlink` (CONC-TOCTOU)
- local struct copied with `memcpy` without `memset` (INFOLEAK-PAD)
- comment `requires n > 0` with no `if (n` (API-PRECONDITION); LLM stays READS
- `memcpy(p, p, n)` (MEM-OVERLAP); `memmove` of the same pointer is OK
- signed index bounded above only / capacity grown before realloc (MEM-ONESIDED-INDEX / MEM-CAPACITY-FIRST) — already fired, now in the taxonomy so they can be COVERED
- `atoi`/`strtol` result used as index or divisor with no `if (i` (TRUST-UNVALIDATED-INPUT)
- `rand()` filling `key[]` (CRYPTO-MISUSE)
- `string_view` of a local `string` (CXX-DANGLING-REF)
- `.begin()` then `push_back` (CXX-ITERATOR-INVALID)

- discarded `bpf()` / `unshare()` / `openat2()` fd used without `< 0` (API-BPF, API-CLONE, API-OPENAT2)
- `weak_ptr.lock()` without a truth test; discarded `std::unexpected<` (CXX-WEAK-PTR, CXX-UNEXPECTED)
- discarded `sendmmsg()` / `landlock_create_ruleset()` without `< 0` (API-SENDMMSG, API-LANDLOCK)
- `std::get` on tuple / `deque[]` / `map.at` without a size/count guard (CXX-TUPLE-GET, CXX-DEQUE-INDEX, CXX-MAP-AT)
- discarded `name_to_handle_at()` / `statfs()` / `prlimit()` / unused `perf_event_open` fd (API-NAME-TO-HANDLE, API-STATFS, API-PRLIMIT, API-PERF-EVENT)
- `set.find` / `queue.front` / `stack.top` / `std::array[]` without a guard (CXX-SET-FIND, CXX-QUEUE-FRONT, CXX-STACK-TOP, CXX-ARRAY-INDEX)
- discarded `membarrier()` / `syncfs()` / `clone3()` / `futex()`; `pkey_alloc` without `<0` (API-MEMBARRIER, API-SYNCFS, API-CLONE3, API-FUTEX, API-PKEY)
- `wstring_view` of a local `wstring`; `multimap.find` without `end()`; `byteswap` as an index (CXX-WSTRING-VIEW, CXX-MULTIMAP-FIND, CXX-BYTESWAP)
- discarded `keyctl()` / `kcmp()` / `reboot()`; `fsopen`/`mq_open`/`shmget` without `<0` (API-KEYCTL, API-KCMP, API-FSOPEN, API-MQ-OPEN, API-SHMGET, API-REBOOT)
- `pmr::vector[]` without `size()`; `u8string_view` of a local `u8string`; `unordered_multimap`/`unordered_multiset` find without `end()`; `shared_lock` without `owns_lock()`; `atomic_flag.test_and_set` without `clear()` (CXX-PMR, CXX-U8STRING-VIEW, CXX-UNORDERED-MULTIMAP, CXX-UNORDERED-MULTISET, CXX-SHARED-LOCK, CXX-ATOMIC-FLAG)
- discarded `adjtimex()` / `sethostname()` / `swapon()` / `acct()` / `ioperm()` / `mincore()` / `rseq()` / `timer_create()` / `klogctl()` / `mount_setattr()`; `semget`/`msgget` without `<0` (API-ADJTIMEX, API-SETHOSTNAME, API-SWAPON, API-ACCT, API-IOPERM, API-MINCORE, API-RSEQ, API-TIMER-CREATE, API-SEMGET, API-MSGGET, API-KLOGCTL, API-MOUNT-SETATTR)
- `condition_variable_any.wait` without a lock/predicate; `recursive_mutex`/`timed_mutex` lock without unlock; `ifstream` without `is_open()`; `this_thread::sleep_for` as the only sync; `call_once` without `once_flag` (CXX-CONDVAR-ANY, CXX-RECURSIVE-MUTEX, CXX-TIMED-MUTEX, CXX-FSTREAM, CXX-THIS-THREAD, CXX-CALL-ONCE)
- discarded `getcpu()` / `process_mrelease()` / `ioprio_*` / `init_module()` / `kexec_load()` / `quotactl_fd()` / `pkey_free()` / `tgkill()` / `add_key()` / `semctl()` / `msgctl()`; `memfd_secret` without `<0` (API-GETCPU, API-PROCESS-MRELEASE, API-MEMFD-SECRET, API-IOPRIO, API-INIT-MODULE, API-KEXEC, API-QUOTACTL-FD, API-PKEY-FREE, API-TGKILL, API-ADD-KEY, API-SEMCTL, API-MSGCTL)
- `shared_timed_mutex`/`recursive_timed_mutex` lock without unlock; `system_error` without `.code()`; `current_zone`/`tzdb` unchecked; `views::zip` unguarded; `format_to` into a raw buffer (CXX-SHARED-TIMED-MUTEX, CXX-RECURSIVE-TIMED-MUTEX, CXX-SYSTEM-ERROR, CXX-CHRONO-TZDB, CXX-RANGES-ZIP, CXX-FORMAT-TO)
- discarded `io_setup()` / `request_key()` / `tkill()` / `timer_delete()` / `mq_unlink()` / `shmat()` / `semop()` / `msgsnd()` / `sync_file_range()` / `msync()` / `socketpair()` / `sysinfo()` (API-IO-SETUP, API-REQUEST-KEY, API-TKILL, API-TIMER-DELETE, API-MQ-UNLINK, API-SHMAT, API-SEMOP, API-MSGSND, API-SYNC-FILE-RANGE, API-MSYNC, API-SOCKETPAIR, API-SYSINFO)
- `error_category` without a compare/`.message()`; nested throw without catch; `atomic_thread_fence` around a plain store; `notify_all_at_thread_exit` without a waiter; `wstring_convert` unchecked; `std::invoke` on a nullable callable (CXX-ERROR-CATEGORY, CXX-NESTED-EXCEPTION, CXX-ATOMIC-FENCE, CXX-NOTIFY-THREAD-EXIT, CXX-WSTRING-CONVERT, CXX-INVOKE)
- discarded `clock_settime()` / `settimeofday()` / `gettid()` / `sched_setscheduler()` / `setitimer()` / `nice()` / `arch_prctl()` / `getdents()` / `utimensat()` / `linkat()` / `mbind()` / `futex_waitv()` (API-CLOCK-SETTIME, API-SETTIMEOFDAY, API-GETTID, API-SCHED-SETSCHEDULER, API-SETITIMER, API-NICE, API-ARCH-PRCTL, API-GETDENTS, API-UTIMENSAT, API-LINKAT, API-MBIND, API-FUTEX-WAITV)
- `std::apply` on a nullable callable; `std::ref` of a local; `std::endian` as an index; `bit_ceil` as an unguarded index; `uncaught_exceptions()` in a destructor throw; `views::join` unguarded (CXX-APPLY, CXX-REFERENCE-WRAPPER, CXX-ENDIAN, CXX-BIT-CEIL, CXX-UNCAUGHT-EXCEPTIONS, CXX-RANGES-JOIN)
- discarded `syslog()` / `setpgid()` / `setreuid()` / `getgroups()` / `epoll_create()` / `timerfd_settime()` / `remap_file_pages()` / `move_pages()` / `cachestat()` / `map_shadow_stack()` / `sched_yield()` / `setfsuid()` (API-SYSLOG, API-SETPGID, API-SETREUID, API-GETGROUPS, API-EPOLL-CREATE, API-TIMERFD-SETTIME, API-REMAP-FILE-PAGES, API-MOVE-PAGES, API-CACHESTAT, API-MAP-SHADOW-STACK, API-SCHED-YIELD, API-SETFSUID)
- `quick_exit` without `at_quick_exit`; `to_array` indexed without size; `zoned_time` without a zone check; `kill_dependency`/`rotl` as an unguarded index; `current_exception()` rethrown without nullptr (CXX-QUICK-EXIT, CXX-TO-ARRAY, CXX-ZONED-TIME, CXX-KILL-DEPENDENCY, CXX-ROTL, CXX-CURRENT-EXCEPTION)
- discarded `wait4()` / `preadv()` / `sendmsg()` / `getsockname()` / `epoll_pwait()` / `inotify_rm_watch()` / `eventfd_read()` / `sched_setattr()` / `renameat2()` / `execveat()` / `mlock2()` / `faccessat2()` (API-WAIT4, API-PREADV, API-SENDMSG, API-GETSOCKNAME, API-EPOLL-PWAIT, API-INOTIFY-RM-WATCH, API-EVENTFD-READ, API-SCHED-SETATTR, API-RENAMEAT2, API-EXECVEAT, API-MLOCK2, API-FACCESSAT2)
- `bit_width`/`lerp`/`midpoint`/`cmp_less`/`countl_zero` as an unguarded index; `std::unreachable()` as a recoverable path (CXX-BIT-WIDTH, CXX-LERP, CXX-MIDPOINT, CXX-CMP-LESS, CXX-COUNTL-ZERO, CXX-UNREACHABLE)
- discarded `posix_fadvise()` / `readahead()` / `sigaction()` / `sigprocmask()` / `sem_open()` / `pthread_rwlock_*` / `pthread_cond_*` / `sigaltstack()` / `renameat()` / `faccessat()` / `fchmodat()` / `pthread_barrier_*` (API-POSIX-FADVISE, API-READAHEAD, API-SIGACTION, API-SIGPROCMASK, API-SEM-OPEN, API-RWLOCK, API-PTHREAD-COND, API-SIGALTSTACK, API-RENAMEAT, API-FACCESSAT, API-FCHMODAT, API-PTHREAD-BARRIER)
- `gcd`/`lcm`/`clamp` as an unguarded index; `std::exchange` then use; `to_address` dangling; `is_constant_evaluated` runtime path unchecked (CXX-GCD, CXX-LCM, CXX-CLAMP, CXX-EXCHANGE, CXX-TO-ADDRESS, CXX-IS-CONSTANT-EVALUATED)
- discarded `symlinkat()` / `unlinkat()` / `mkdirat()` / `mknodat()` / `readlinkat()` / `fstatat()` / `pthread_spin_*` / `pthread_key_*` / `pthread_cancel()` / `pthread_kill()` / `pthread_sigmask()` / `pthread_atfork()` (API-SYMLINKAT, API-UNLINKAT, API-MKDIRAT, API-MKNODAT, API-READLINKAT, API-FSTATAT, API-PTHREAD-SPIN, API-PTHREAD-KEY, API-PTHREAD-CANCEL, API-PTHREAD-KILL, API-PTHREAD-SIGMASK, API-PTHREAD-ATFORK)
- `addressof`/`assume_aligned` as an unguarded index; `as_const` then const_cast write; `exclusive_scan` OOB dest; `make_exception_ptr` rethrown unchecked; `set_terminate` handler throws (CXX-ADDRESSOF, CXX-ASSUME-ALIGNED, CXX-AS-CONST, CXX-EXCLUSIVE-SCAN, CXX-MAKE-EXCEPTION-PTR, CXX-SET-TERMINATE)
- discarded `pledge()` / `unveil()` / `sysctl()` / `kqueue()` / `kevent()` / `pause()` / `ppoll()` / `sigwait()` / `sigqueue()` / `getcontext()` / `sem_timedwait()` / `pthread_attr_*` (API-PLEDGE, API-UNVEIL, API-SYSCTL, API-KQUEUE, API-KEVENT, API-PAUSE, API-PPOLL, API-SIGWAIT, API-SIGQUEUE, API-UCONTEXT, API-SEM-TIMEDWAIT, API-PTHREAD-ATTR)
- `inclusive_scan` OOB dest; `transform_reduce`/`std::reduce` as an unguarded index; `uninitialized_copy` OOB; `destroy_at` then use; `forward_like` then use-after-move (CXX-INCLUSIVE-SCAN, CXX-TRANSFORM-REDUCE, CXX-REDUCE, CXX-UNINITIALIZED-COPY, CXX-CONSTRUCT-AT, CXX-FORWARD-LIKE)
- discarded `cap_enter()` / `cap_rights_limit()` / `pdfork()` / `procctl()` / `closefrom()` / `issetugid()` / `arc4random()` / `chflags()` / `getfsstat()` / `pthread_yield()` / `sem_trywait()` / `adjtime()` (API-CAP-ENTER, API-CAP-RIGHTS, API-PDFORK, API-PROCCTL, API-CLOSEFROM, API-ISSETUGID, API-ARC4RANDOM, API-CHFLAGS, API-GETFSSTAT, API-PTHREAD-YIELD, API-SEM-TRYWAIT, API-ADJTIME)
- `uninitialized_fill` OOB dest; `destroy_n` then use; `add_sat` as an unguarded index; `transform_inclusive_scan` OOB dest; `type_identity` recast then OOB; `nontype` as an unguarded index (CXX-UNINITIALIZED-FILL, CXX-DESTROY-N, CXX-ADD-SAT, CXX-TRANSFORM-SCAN, CXX-TYPE-IDENTITY, CXX-NONTYPE)
- discarded `revoke()` / `ktrace()` / `rfork()` / `jail()` / `setlogin()` / `getresuid()` / `getpeereid()` / `strtonum()` / `reallocarray()` / `timingsafe_*()` / `getprogname()` / `daemon()` (API-REVOKE, API-KTRACE, API-RFORK, API-JAIL, API-SETLOGIN, API-GETRESUID, API-GETPEEREID, API-STRTONUM, API-REALLOCARRAY, API-TIMINGSAFE, API-GETPROGNAME, API-DAEMON)
- `is_layout_compatible` recast then OOB; `is_pointer_interconvertible_*` recast then OOB; `uninitialized_value_construct` OOB dest; `basic_const_iterator` write-through; `is_corresponding_member` recast then OOB; `ranges::to` unguarded index (CXX-LAYOUT-COMPATIBLE, CXX-PTR-INTERCONVERTIBLE, CXX-UNINITIALIZED-VALUE, CXX-CONST-ITERATOR, CXX-CORRESPONDING-MEMBER, CXX-RANGES-TO)
- discarded `cap_fcntls_limit()` / `pdgetpid()` / `kldload()` / `extattr_*()` / `mac_set_proc()` / `auditon()` / `kvm_open()` / `reallocf()` / `uuidgen()` / `setfib()` / `ntp_gettime()` / `crypt_newhash()` (API-CAP-FCNTLS, API-PDGETPID, API-KLDLOAD, API-EXTATTR, API-MAC, API-AUDIT, API-KVM, API-REALLOCF, API-UUIDGEN, API-SETFIB, API-NTP-GETTIME, API-CRYPT-NEWHASH)
- `views::enumerate` unguarded index; `cartesian_product` unguarded index; `views::chunk`/`slide`/`adjacent` unguarded index; `join_with` unguarded index (CXX-ENUMERATE, CXX-CARTESIAN-PRODUCT, CXX-CHUNK, CXX-SLIDE, CXX-ADJACENT, CXX-JOIN-WITH)
- discarded `wait6()` / `cpuset_*affinity()` / `rtprio()` / `kenv()` / `getfh()` / `getmntinfo()` / `nmount()` / `strmode()` / `getosreldate()` / `cap_sandboxed()` / `getgrouplist()` / `eaccess()` (API-WAIT6, API-CPUSET, API-RTPRIO, API-KENV, API-GETFH, API-GETMNTINFO, API-NMOUNT, API-STRMODE, API-GETOSRELDATE, API-CAP-SANDBOXED, API-GETGROUPLIST, API-EACCESS)
- `zip_transform` unguarded index; `as_rvalue` OOB/dangling; `from_range` unguarded index; `is_scoped_enum` recast then OOB; `views::stride`/`repeat` unguarded index (CXX-ZIP-TRANSFORM, CXX-AS-RVALUE, CXX-FROM-RANGE, CXX-SCOPED-ENUM, CXX-STRIDE, CXX-REPEAT)
- discarded `login_getclass()` / `fflagstostr()` / `getdirentries()` / `kinfo_getproc()` / `_umtx_op()` / `thr_kill()` / `modfind()` / `lpathconf()` / `getloginclass()` / `getfsent()` / `minherit()` / `cap_getmode()` (API-LOGIN-GETCLASS, API-FFLAGS, API-GETDIRENTRIES, API-KINFO, API-UMTX, API-THR, API-MODFIND, API-LPATHCONF, API-LOGINCLASS, API-GETFSENT, API-MINHERIT, API-CAP-GETMODE)
- `views::take`/`drop`/`filter` unguarded index; `views::transform` unguarded index; `views::elements`/`iota` unguarded index (CXX-TAKE, CXX-DROP, CXX-FILTER, CXX-TRANSFORM-VIEW, CXX-ELEMENTS, CXX-IOTA)

Plus the vocabulary, the taxonomy (583 COVERED on testdata), and the confidence product.

## Also in the pipeline

| Instrument | Role |
|---|---|
| cppcheck | value-flow; uninit struct members |
| gcc/clang -Wall | cheap, noisy, union of both compilers |
| ASan/UBSan/TSan | probe then compile; MinGW here is NOTRUN (no libubsan) |
| AFL++/libFuzzer | PATH/clang -fsanitize=fuzzer probe; NOTRUN if absent |
| KLEE | in-process concolic in helix/concolic.py; binary adapter still NOTRUN |
| Semgrep | PATH adapter; real scan when present; UNKNOWN if empty |
| Coccinelle | `spatch` + `helix/cocci/`; missing is NOTRUN |
| Infer/CodeQL | adapters; in-process taint; UNKNOWN without a DB |
| Frama-C | EVA adapter + ACSL in contracts.py |
| Interval | path-sensitive integer ranges; FAILED ≠ proof |
| RapidCheck | `ensures` → property test |
| Differential | two functions, same bytes, disagree |
| Mutation testing | operator mutants vs the same properties |

## Implemented in code

File:function pointers for the methods above. Call these from `helix/pipeline.py`; do not merge CLEAN with a proof.

- `helix/bmc.py:bmc_function` — ESBMC-method BMC; statuses `PROVED` / `PROVED-UNBOUNDED` / `FAILED` / `BOUNDED`
- `helix/bmc.py:k_induction` — base case = BMC; closed loops may be unbounded
- `helix/bmc.py:run_bmc` — `run_bmc(functions, unwind) -> list[Finding]`
- `helix/fuse.py:run_fuse` — FuSeBMC closed loop: `run_fuse(functions, bmc_findings, root, budget, iters) -> list[Finding]`
- `helix/fuse.py:seeds_from_bmc` — BMC cex bytes as fuzzer seeds
- `helix/fuse.py:bmc_toward_goal` — uncovered `if` as a new BMC goal
- `helix/fuzz.py:run_fuzz` — greybox; CLEAN is not a proof
- `helix/fuzz.py:fuzz_function` — compile + run harness
- `helix/contracts.py:parse_comments` — `// requires:` plus Frama-C `/*@` / `//@` ACSL; `\result` → `result`
- `helix/contracts.py:prove_contracts` — `prove_contracts(functions, unwind) -> list[Finding]`; proof rewritten to `PROVED-ASSUMING`
- `helix/contracts.py:bmc_function_with_assume` — `if (!(requires)) return 0;` + `assert(ensures);` then `helix.bmc.bmc_function`
- `helix/ltl.py:run_ltl` — `run_ltl(functions, spec_paths) -> list[Finding]`; safety fragment `G p`, `G (p -> X q)`, `G (req -> F_k ack)` with `k=8`
- `helix/ltl.py:check_safety` — decide the fragment; otherwise NOTRUN (missing Strix)
- `helix/ltl.py:extract_fsm` — `switch(state)` plus transitions
- `helix/diff.py:run_diff` — `run_diff(functions, root) -> list[Finding]`; pair `*_a`/`*_b` or `// diff: othername`; disagree = `FAILED`
- `helix/agent.py:fuzz4all_seeds` — Fuzz4All autoprompt seeds
- `helix/agent.py:chatfuzz_mutants` — stall mutants
- `helix/agent.py:dafny_specs` — Dafny-shaped specs as HYPOTHESIS
- `helix/agent.py:rlef_repair` — execution-feedback repair loop
- `helix/agent.py:sandbox_run` — gcc/clang compile+run in tempdir; missing compiler is honest
- `helix/agent.py:interpreter_loop` — OpenCodeInterpreter generate/run/refine
- `helix/interval.py:run_interval` — path-sensitive integer ranges; FAILED is FINDS, silence is not PROVED
- `helix/checkers.py:_mem_lifetime` — `MEM-UAF` / `MEM-DOUBLE-FREE`
- `helix/checkers.py:_fmt_string` — `FMT-STRING` (printf-family format not a literal)
- `helix/checkers.py:_intent_mismatch` — comment/code increment mismatch is INTENT FINDS
- `helix/checkers.py:_cxx_self_assign` / `_mismatched_free` / `_ptr_arith` / `_float_ub`
- `helix/checkers.py:_lock_order` / `_cxx_virtual_in_ctor` / `_conc_toctou`
- `helix/checkers.py:_infoleak_pad` / `_api_precondition` / `_mem_overlap`
- `helix/checkers.py:_trust_unvalidated_input` / `_crypto_misuse` / `_cxx_dangling_ref` / `_cxx_iter_invalid`
- `helix/checkers.py:_str_missing_nul` / `_cxx_delete_this` / `_api_mkstemp`
- `helix/checkers.py:_api_tmpnam` / `_fmt_percent_n` / `_api_system`
- `helix/checkers.py:_api_chroot` / `_int_atoi` / `_str_sprintf`
- `helix/checkers.py:_api_getenv_null` / `_mem_sizeof_ptr` / `_popen_leak`
- `helix/checkers.py:_cxx_catch_by_value` / `_cxx_throw_noexcept` / `_cxx_missing_virtual_dtor`
- `helix/checkers.py:_api_umask` / `_crypto_srand` / `_mem_realloc_zero`
- `helix/checkers.py:_cxx_lambda_dangle` / `_cxx_unique_reset` / `_cxx_const_cast`
- `helix/checkers.py:_api_mktemp` / `_api_signal` / `_api_strdup_null`
- `helix/checkers.py:_cxx_dynamic_cast_null` / `_cxx_reinterpret` / `_cxx_throw_copy`
- `helix/checkers.py:_api_fork` / `_api_exec` / `_api_mmap` / `_wcs_unbounded` / `_api_getcwd` / `_api_ioctl`
- `helix/checkers.py:_cxx_bit_cast` / `_cxx_placement_new` / `_cxx_std_thread` / `_cxx_optional_null` / `_cxx_variant_get` / `_cxx_span_dangle`
- `helix/checkers.py:_api_dlopen` / `_api_accept` / `_api_realpath` / `_api_chmod_world` / `_int_clz_zero` / `_mem_bcopy`
- `helix/checkers.py:_cxx_vector_index` / `_cxx_catch_all` / `_cxx_throw_new` / `_cxx_uninit_member` / `_cxx_copy_assign_ptr` / `_cxx_volatile_cast`
- `helix/checkers.py:_api_setuid` / `_api_socket` / `_api_bind` / `_str_snprintf` / `_api_unlink` / `_api_mkfifo`
- `helix/checkers.py:_cxx_shared_ptr_get` / `_cxx_auto_ptr` / `_cxx_string_data` / `_cxx_enable_shared` / `_cxx_forwarding_ref` / `_cxx_explicit_ctor`
- `helix/checkers.py:_api_listen` / `_api_connect` / `_api_pipe` / `_api_dup` / `_api_fcntl` / `_api_wait`
- `helix/checkers.py:_cxx_move_const` / `_cxx_bind_tmp` / `_cxx_expected_null` / `_cxx_std_format` / `_cxx_this_capture` / `_cxx_spaceship_default`
- `helix/checkers.py:_api_select` / `_api_send` / `_api_shutdown` / `_api_kill` / `_api_getaddrinfo` / `_str_strncpy_nul`
- `helix/checkers.py:_cxx_std_async` / `_cxx_future_get` / `_cxx_function_null` / `_cxx_nodiscard` / `_cxx_std_jthread` / `_cxx_mdspan_dangle`
- `helix/checkers.py:_api_pthread_join` / `_api_sem_wait` / `_api_openat` / `_api_flock` / `_api_chown` / `_api_symlink`
- `helix/checkers.py:_cxx_atomic_ref` / `_cxx_condition_wait` / `_cxx_std_bind` / `_cxx_assume` / `_cxx_generator` / `_cxx_shared_mutex`
- `helix/checkers.py:_api_opendir` / `_api_setrlimit` / `_api_getsockopt` / `_api_stat` / `_api_mkdir` / `_api_getpwuid`
- `helix/checkers.py:_cxx_any_cast` / `_cxx_filesystem` / `_cxx_regex` / `_cxx_latch` / `_cxx_from_chars` / `_cxx_init_list_dangle`
- `helix/checkers.py:_api_clock_gettime` / `_api_shm_open` / `_api_posix_spawn` / `_api_glob` / `_api_fseek` / `_api_access`
- `helix/checkers.py:_cxx_stop_token` / `_cxx_flat_map` / `_cxx_semaphore` / `_cxx_stacktrace` / `_cxx_unique_release` / `_cxx_pack_pragma`
- `helix/checkers.py:_api_getopt` / `_api_uname` / `_api_sendfile` / `_api_memfd` / `_api_prctl` / `_api_tcgetattr`
- `helix/checkers.py:_cxx_function_ref` / `_cxx_move_only_function` / `_cxx_ranges_dangle` / `_cxx_chrono_seed` / `_cxx_inplace_vector` / `_cxx_flat_set`
- `helix/checkers.py:_api_sysconf` / `_api_getrusage` / `_api_nftw` / `_api_wordexp` / `_api_getlogin` / `_api_inet_pton`
- `helix/checkers.py:_cxx_copyable_function` / `_cxx_hive` / `_cxx_bitset_index` / `_cxx_sstream_view` / `_cxx_optional_value` / `_cxx_indirect`
- `helix/checkers.py:_api_mlock` / `_api_splice` / `_api_inotify` / `_api_fsync` / `_api_getrandom` / `_api_getline`
- `helix/checkers.py:_cxx_to_chars` / `_cxx_hazard_pointer` / `_cxx_text_encoding` / `_cxx_expected_error` / `_cxx_variant_valueless` / `_cxx_simd_index`
- `helix/checkers.py:_api_asprintf` / `_api_strlcpy` / `_api_isatty` / `_api_ptsname` / `_api_mount` / `_api_fmemopen`
- `helix/checkers.py:_cxx_rcu` / `_cxx_linalg` / `_cxx_sync_wait` / `_cxx_embed` / `_cxx_contracts` / `_cxx_reflection`
- `helix/checkers.py:_api_scandir` / `_api_setxattr` / `_api_sched_affinity` / `_api_aio` / `_api_statx` / `_api_pidfd`
- `helix/checkers.py:_cxx_out_ptr` / `_cxx_flat_multimap` / `_cxx_spanstream` / `_cxx_barrier` / `_cxx_task` / `_cxx_generator_discard`
- `helix/checkers.py:_cxx_osyncstream` / `_cxx_packaged_task` / `_cxx_flat_multiset` / `_cxx_syncbuf` / `_cxx_counted_iterator` / `_cxx_promise`
- `helix/bmc.py:unencoded_syntax_reason` — memcpy/mkstemp/tmpnam/chroot/popen/umask/srand/signal/mktemp/fork/exec/mmap/ioctl/wcscpy/dlopen/accept/chmod/setuid/socket/bind/unlink/mkfifo/listen/connect/pipe/dup/fcntl/wait/select/send/shutdown/kill/getaddrinfo/pthread_join/sem_wait/openat/flock/chown/symlink/opendir/setrlimit/getsockopt/stat/mkdir/getpwuid/clock_gettime/gettimeofday/shm_open/posix_spawn/glob/fseek/sleep/access/getopt/uname/sendfile/memfd_create/prctl/tcgetattr/sysconf/getrusage/nftw/wordexp/getlogin/inet_pton/mlock/splice/inotify/fsync/getrandom/getline/strlcpy/isatty/ptsname/mount/fmemopen/setxattr/aio_read/statx/pidfd, designated init, alignof, va_arg, range-for, lambda, const_cast, dynamic_cast/typeid/reinterpret_cast/bit_cast/launder/start_lifetime_as, packed/#pragma pack, coroutines, GNU &&label/case-range/cleanup/vector_size, wide strings, __int128/_Decimal/_Float16, if constexpr/fold/requires/constexpr, C23 nullptr, restrict, __builtin_clz/choose_expr/atomic, typeof_unqual, std::thread/jthread/async/future/function/optional/variant/span/mdspan/vector/expected/format/mutex, condition_variable/shared_mutex/atomic_ref/generator/[[assume(/std::bind(/any/filesystem/regex/latch/from_chars/visit/initializer_list/source_location/stacktrace/stop_token/flat_map/chrono/function_ref/flat_set/views/inplace_vector/hive/bitset/indirect/polymorphic/stringstream/import-module, <=>; computed goto (plain `goto` stays ERROR)
- `helix/concolic.py:concolic_function` — VLA/float/recursion/C++ view/alloca/setjmp/va_list are NEEDS-HARNESS; goto stays ERROR
- `helix/bmc.py:_has_self_call` — recursive unconstrained call is NEEDS-HARNESS, never PROVED
- `helix/bmc.py:k_induction` — havoced step k=1 then k=2; SAT stays BOUNDED
- `helix/adapters_extra.py:run_optional_tools` — present tools run cheap analysis; empty success is UNKNOWN, never CLEAN/PROVED
- `helix/adapters_extra.py:_run_spatch` — Coccinelle; missing `spatch` is NOTRUN
- `helix/ltl.py:synthesize_missing` — Strix-shaped missing `G (p -> X q)` edges are HYPOTHESIS
- `helix/contracts.py:_instrument_invariant` — Dafny `invariant:` around loops; PROVED-ASSUMING
