# Helix

Hybrid code-testing pipeline: **deterministic instruments first**, then
bounded proofs, then fuzzing, then Qwen 3.5 9B. GUI is Qt. Inference is
llama.cpp. SIMD is xsimd. GPU mutation is CUDA.

```
python -m helix testdata --no-llm
python -m helix testdata --gui
python -m helix testdata --resume
python -m unittest discover -s tests -v
```

A missing tool is `NOTRUN`. It is never a clean result. `PROVED` and
`BOUNDED` are never merged. A fuzzer `CLEAN` is not a proof. LLM output
is `HYPOTHESIS`. A Dafny-style proof under `requires` is `PROVED-ASSUMING`.

See [docs/PLAN.md](docs/PLAN.md) and [docs/MINED.md](docs/MINED.md).

## What runs on this machine today

| piece | status |
|---|---|
| Qwen 3.5 9B | Ollama `qwen3.5:9b` (same GGUF llama.cpp would load) |
| llama.cpp | Python talks to Ollama first; llama-cpp-python is CPU-only without MSVC |
| Qt GUI | `python -m helix --gui` (PySide6); Resume; skip fuzz/repair/optional; taxonomy table |
| xsimd | **helix_native.dll** built with MinGW g++ + xsimd 13.2 |
| CUDA havoc | `native/cuda/mutate.cu` — needs MSVC host for nvcc |
| Z3 BMC | incremental k=1,2,4,…,K; `goto` is ERROR; computed `goto *` is NEEDS-HARNESS; TLS/`_Complex`/`typeof`/nested functions, memcpy/mkstemp/tmpnam/chroot/popen/umask/srand/signal/mktemp/fork/exec/mmap/ioctl/wcscpy/dlopen/accept/chmod/setuid/socket/bind/unlink/mkfifo, opendir/setrlimit/getsockopt/stat/mkdir/getpwuid, clock_gettime/shm_open/posix_spawn/glob/fseek/sleep/access, getopt/uname/sendfile/memfd_create/prctl/tcgetattr, sysconf/getrusage/nftw/wordexp/getlogin/inet_pton, mlock/splice/inotify/fsync/getrandom/getline, strlcpy/isatty/ptsname/mount/fmemopen/explicit_bzero, setxattr/sched_setaffinity/aio_read/io_uring/statx/pidfd, fanotify/seccomp/getgrnam/fallocate/close_range/landlock/getpriority/signalfd, bpf/userfaultfd/getpass/initgroups/unshare/openat2/sendmmsg/name_to_handle_at/process_madvise/personality/quotactl, pivot_root/membarrier/pkey_alloc/statfs/syncfs/prlimit/process_vm_readv/perf_event_open, `char t[]=`, call-site `&n`, dynamic_cast/typeid/reinterpret_cast/bit_cast/launder, packed, coroutines, GNU `&&label`/case-range, `__int128`/`_Decimal`, `if constexpr`/fold, C23 nullptr, `__builtin_clz`, `std::thread`/`optional`/`variant`/`span`/`vector`/`any`/`filesystem`/`regex`/`latch`/`from_chars`/`to_chars`/`visit`/`initializer_list`/`source_location`/`stacktrace`/`stop_token`/`flat_map`/`chrono`/`function_ref`/`flat_set`/`views`/`inplace_vector`/`hive`/`bitset`/`indirect`/`stringstream`/`hazard_pointer`/`text_encoding`/`simd`/`rcu`/`linalg`/`#embed`/`std::meta`/`out_ptr`/`flat_multimap`/`spanstream`/`task`/`osyncstream`/`packaged_task`/`flat_multiset`/`syncbuf`/`counted_iterator`/`weak_ptr`/`exception_ptr`/`coroutine_handle`/`valarray`/`to_underlying`/`unexpected<`/`tuple`/`deque`/`forward_list`/`std::list`/`std::map`/`unordered_map`/`std::set`/`queue`/`stack`/`priority_queue`/`std::array`/`unordered_set`, clone3/kcmp/keyctl/fsopen/futex/shmget/mq_open/adjtimex/sethostname/memfd_secret/reboot/swapon/semget/klogctl/wstring/multimap/byteswap, init_module/kexec_load/quotactl_fd/pkey_free/tgkill/add_key/semctl/msgctl/shmctl/timer_settime/setdomainname/io_submit, `std::pmr`/`u8string`/`unordered_multimap`/`unordered_multiset`/`shared_lock`/`atomic_flag`, io_setup/request_key/tkill/timer_delete/mq_unlink/shmat/semop/msgsnd/sync_file_range/msync/socketpair/sysinfo, `condition_variable_any`/`recursive_mutex`/`timed_mutex`/`ifstream`/`this_thread`/`call_once`, clock_settime/gettid/arch_prctl/futex_waitv/mbind, `shared_timed_mutex`/`recursive_timed_mutex`/`system_error`/`tzdb`/`views::zip`/`format_to`, syslog/epoll_create/timerfd_settime/sched_yield/move_pages, `error_category`/`nested_exception`/`atomic_thread_fence`/`wstring_convert`/`std::invoke`, wait4/preadv/sendmsg/epoll_pwait/execveat/mlock2, `std::apply`/`reference_wrapper`/`std::endian`/`bit_ceil`/`uncaught_exceptions`/`views::join`, ustat/vhangup/mseal/futex_wake/listmount/lsm_*/set_mempolicy_home_node/file_getattr/setxattrat/fchmodat2/rt_sigqueueinfo/open_tree_attr, `quick_exit`/`to_array`/`zoned_time`/`kill_dependency`/`std::rotl`/`current_exception`, posix_fadvise/readahead/sigaction/sem_open/pthread_rwlock/pthread_cond/renameat/faccessat/fchmodat/pthread_barrier, `std::bit_width`/`lerp`/`midpoint`/`cmp_less`/`countl_zero`/`std::unreachable`, symlinkat/unlinkat/mkdirat/mknodat/readlinkat/fstatat/pthread_spin/pthread_key/pthread_cancel/pthread_kill/pthread_sigmask/pthread_atfork, `std::gcd`/`lcm`/`clamp`/`exchange`/`to_address`/`is_constant_evaluated`, pledge/unveil/sysctl/kqueue/kevent/pause/ppoll/sigwait/sigqueue/getcontext/sem_timedwait/pthread_attr, `std::addressof`/`assume_aligned`/`as_const`/`exclusive_scan`/`make_exception_ptr`/`set_terminate`, cap_enter/pdfork/closefrom/issetugid/arc4random/chflags/getfsstat/pthread_yield/sem_trywait/adjtime, revoke/ktrace/rfork/jail/setlogin/getresuid/getpeereid/strtonum/reallocarray/timingsafe/getprogname/daemon, kldload/extattr/mac_set_proc/auditon/kvm_open/reallocf/uuidgen/ntp_gettime/crypt_newhash, wait6/cpuset_setaffinity/rtprio/kenv/getfh/getmntinfo/nmount/strmode/getosreldate/cap_sandboxed/getgrouplist/eaccess, login_getclass/getdirentries/_umtx_op/thr_kill/modfind/lpathconf/minherit/cap_getmode, nfssvc/sysarch/sbrk/ksem_open/kldfirstmod/valloc/getdomainname, `inclusive_scan`/`transform_reduce`/`std::reduce`/`uninitialized_copy`/`construct_at`/`forward_like`/`uninitialized_fill`/`destroy_n`/`add_sat`/`transform_inclusive_scan`/`type_identity`/`nontype`/`is_layout_compatible`/`uninitialized_value_construct`/`ranges::to`/`views::enumerate`/`cartesian_product`/`join_with`/`zip_transform`/`from_range`/`is_scoped_enum`/`views::take`/`drop`/`filter`/`transform_view`/`iota`, wide strings are `NEEDS-HARNESS` |
| Static inline | same-file `static` SCALAR callees inlined one level before BMC (unconstrained calls are not a proof of the callee) |
| Empty TU | `.c` with no functions is `ERROR` / `EMPTY-TU`, never `CLEAN` |
| POINTER harness | `// requires:` materialize → `PROVED-ASSUMING` |
| ACSL | Frama-C `/*@ requires` / `ensures \result` → `PROVED-ASSUMING` |
| Interval | path-sensitive ranges; FAILED on planted integer UB; silence is not a proof |
| FuSeBMC loop | BMC cex → seeds → Fuzz4All (when LLM on) → coverage goals; POINTER is NEEDS-HARNESS (missing harness, not ERROR); concrete **CRASH** |
| Concolic (KLEE method) | seed + branch negation; POINTER / heap / alloca / float / recursion / views / `throw` / `setjmp` / `asm` / `_Generic` / `try` / memcpy/mkstemp/popen/signal / opendir/stat / address-of = NEEDS-HARNESS; `strcpy` overflow is CRASH; goto stays ERROR |
| Dafny decreases / invariant | loop ranking + `assert(I)` at heads; proofs stay `PROVED-ASSUMING` |
| Strix synthesis | missing `G (p → X q)` edges are `HYPOTHESIS`, never a proof |
| PBSD lints | 583 classes including API-LOGIN-GETCLASS, API-LPATHCONF, API-CAP-GETMODE, CXX-TAKE, CXX-IOTA |
| Coccinelle | `spatch` adapter + `helix/cocci/`; missing → `NOTRUN` |
| Optional tools | 11 named (incl. spatch, libFuzzer); empty success is UNKNOWN |
| Tests | **1271 passing** |

## Native CPU library

```
cmake -B native/build -DHELIX_CUDA=OFF -DHELIX_LLAMA=OFF -DHELIX_QT=OFF -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=g++
cmake --build native/build
```

MSVC is required for CUDA + llama.cpp + C++ Qt:

```
cmake -B build -DHELIX_CUDA=ON -DHELIX_LLAMA=ON -DHELIX_QT=ON
cmake --build build --config Release
```
