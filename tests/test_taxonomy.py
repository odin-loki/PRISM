"""Honest taxonomy coverage. python -m unittest tests.test_taxonomy -v"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from prism import laws
from prism.confidence import score
from prism.config import Config
from prism.models import Finding, RunReport, StageResult
from prism.pipeline import run_pipeline
from prism.taxonomy import CLASSES, coverage_from_report


def _rep(*findings: Finding, stage="bmc", status="ok") -> RunReport:
    rec = RunReport(root="x")
    rec.stages.append(StageResult(
        name=stage, status=status, findings=list(findings), records=len(findings),
    ))
    return rec


class TestCoverageFromReport(unittest.TestCase):
    def test_crash_cls_is_covered(self):
        f = Finding(
            stage="fuzz", status=laws.CRASH, file="a.c", function="add",
            line=1, cls="INT-SIGNED-OVF", message="ovf",
            strength=laws.STRENGTH_FINDS,
        )
        rows = {r["id"]: r for r in coverage_from_report(_rep(f, stage="fuzz"))}
        self.assertEqual(rows["INT-SIGNED-OVF"]["verdict"], "COVERED")

    def test_hypothesis_never_covered(self):
        f = Finding(
            stage="llm", status=laws.HYPOTHESIS, file="a.c", function="add",
            line=1, cls="INTENT", message="maybe",
            strength=laws.STRENGTH_READS,
        )
        rows = {r["id"]: r for r in coverage_from_report(_rep(f, stage="llm"))}
        self.assertEqual(rows["INTENT"]["verdict"], "PARTIAL")
        self.assertNotEqual(rows["INTENT"]["verdict"], "COVERED")

    def test_failed_reads_strength_is_not_covered(self):
        """READS cannot COVER, even on FAILED. C++ must not rewrite READS→FINDS."""
        f = Finding(
            stage="ltl", status=laws.FAILED, file="fsm.c", function="step",
            line=1, cls="LTL-SAFETY", message="monitor miss",
            strength=laws.STRENGTH_READS,
        )
        rows = {r["id"]: r for r in coverage_from_report(_rep(f, stage="ltl"))}
        self.assertEqual(rows["LTL-SAFETY"]["best"], laws.STRENGTH_READS)
        self.assertEqual(rows["LTL-SAFETY"]["verdict"], "PARTIAL")
        self.assertNotEqual(rows["LTL-SAFETY"]["verdict"], "COVERED")

    def test_wp_empty_cls_still_covers_func_contract(self):
        f = Finding(
            stage="wp", status=laws.PROVED_ASSUMING, file="a.c", function="inc",
            line=1, cls="", message="WP holds; never PROVED",
            strength=laws.STRENGTH_PROVES,
        )
        rows = {r["id"]: r for r in coverage_from_report(_rep(f, stage="wp"))}
        self.assertEqual(rows["FUNC-CONTRACT"]["verdict"], "COVERED")
        self.assertNotEqual(rows["INT-SIGNED-OVF"]["verdict"], "COVERED")

    def test_cpp_taxonomy_does_not_promote_reads_to_finds(self):
        root = Path(__file__).resolve().parents[1]
        cpp = (root / "src" / "prism" / "taxonomy.cpp").read_text(encoding="utf-8")
        gen = (root / "tools" / "gen_prism.py").read_text(encoding="utf-8")
        self.assertNotIn("if (st == READS) st = FINDS", cpp)
        self.assertNotIn("if (st == READS) st = FINDS", gen)
        self.assertIn('cls.empty() && (s.name == "wp" || s.name == "contracts")', cpp)

    def test_wp_assuming_covers_contract(self):
        f = Finding(
            stage="wp", status=laws.PROVED_ASSUMING, file="a.c", function="inc",
            line=1, cls="FUNC-CONTRACT", message="WP holds; never PROVED",
            strength=laws.STRENGTH_PROVES,
        )
        rows = {r["id"]: r for r in coverage_from_report(_rep(f, stage="wp"))}
        self.assertEqual(rows["FUNC-CONTRACT"]["verdict"], "COVERED")
        self.assertNotEqual(rows["FUNC-CONTRACT"]["verdict"], "GAP")

    def test_lints_intent_finds_is_covered(self):
        f = Finding(
            stage="lints", status=laws.FAILED, file="intent.c",
            function="intent_bad", line=3, cls="INTENT",
            message="comment claims increment but intent_bad() returns x unchanged",
            strength=laws.STRENGTH_FINDS,
        )
        rows = {r["id"]: r for r in coverage_from_report(_rep(f, stage="lints"))}
        self.assertEqual(rows["INTENT"]["verdict"], "COVERED")
        self.assertEqual(rows["INTENT"]["best"], laws.STRENGTH_FINDS)

    def test_skipped_stage_is_gap_not_covered(self):
        rec = RunReport(root="x")
        rec.stages.append(StageResult(name="taint", status="skipped"))
        rows = {r["id"]: r for r in coverage_from_report(rec)}
        self.assertEqual(rows["TAINT-SINK"]["verdict"], "GAP")

    def test_new_breadth_classes_exist(self):
        rows = {r["id"]: r for r in coverage_from_report(RunReport(root="x"))}
        for cid in ("MEM-MEMSET-SWAP", "INT-TAUTOLOGY", "INT-WRAP-ALLOC",
                    "INT-TRUNC", "INT-SIGN-CONV", "MEM-NEW-DELETE", "EMPTY-TU",
                    "MEM-STACK-ESCAPE", "STR-UNBOUNDED-COPY",
                    "CTRL-MISSING-RETURN", "CTRL-FALLTHROUGH", "CTRL-DEAD-GUARD",
                    "UNINIT-RETURN", "PTR-UNINIT", "UNINIT-BRANCH",
                    "STR-OFF-BY-ONE",
                    "CTRL-SIBLING-ASYMMETRY", "API-IGNORED-ERROR",
                    "RES-FD-LEAK", "MEM-VLA-SIZE", "MEM-LEAK", "STR-NULL-ARG", "STR-NULL-MEMBER",
                    "PTR-CHAIN-NULL",
                    "CXX-USE-AFTER-MOVE", "CXX-DANGLING-REF", "CXX-ITERATOR-INVALID",
                    "CXX-SELF-ASSIGN", "MEM-MISMATCHED-FREE",
                    "MEM-PTR-ARITH", "FLOAT-UB",
                    "MEM-ONESIDED-INDEX", "MEM-CAPACITY-FIRST", "MEM-OVERLAP",
                    "LOCK-ORDER", "LOCK-DOUBLE-UNLOCK", "CXX-VIRTUAL-IN-CTOR",
                    "CONC-TOCTOU",
                    "CONC-ATOMICITY", "CXX-EXCEPTION-LEAK", "CXX-THROW-DESTRUCTOR",
                    "INFOLEAK-PAD", "API-PRECONDITION",
                    "TRUST-UNVALIDATED-INPUT", "CRYPTO-MISUSE",
                    "LOCK-DOUBLE-LOCK", "MEM-ALLOCA", "API-SCANF-UNCHECKED",
                    "API-GETS", "CXX-THROW-SPEC", "INT-ENUM-HOLE",
                    "LOCK-MISSING-INIT", "CXX-SLICING", "MEM-FLEX-ARRAY",
                    "INT-BOOL-AS-BIT", "API-STRTOK-REENTRANT",
                    "CTRL-EMPTY-INFINITE",
                    "STR-MISSING-NUL", "CXX-DELETE-THIS", "API-MKSTEMP",
                    "API-TMPNAM", "API-MKTEMP", "API-SIGNAL", "FMT-PERCENT-N",
                    "API-SYSTEM",
                    "API-CHROOT", "INT-ATOI", "STR-SPRINTF",
                    "CXX-CATCH-BY-VALUE", "CXX-THROW-NOEXCEPT",
                    "CXX-MISSING-VIRTUAL-DTOR",
                    "CXX-LAMBDA-DANGLE", "CXX-UNIQUE-RESET", "CXX-CONST-CAST",
                    "CXX-DYNAMIC-CAST-NULL", "CXX-REINTERPRET", "CXX-THROW-COPY",
                    "CXX-BIT-CAST", "CXX-PLACEMENT-NEW", "CXX-STD-THREAD",
                    "CXX-OPTIONAL-NULL", "CXX-VARIANT-GET", "CXX-SPAN-DANGLE",
                    "CXX-VECTOR-INDEX", "CXX-CATCH-ALL", "CXX-THROW-NEW",
                    "CXX-UNINIT-MEMBER", "CXX-COPY-ASSIGN-PTR",
                    "CXX-VOLATILE-CAST",
                    "CXX-SHARED-PTR-GET", "CXX-AUTO-PTR", "CXX-STRING-DATA",
                    "CXX-ENABLE-SHARED", "CXX-FORWARDING-REF",
                    "CXX-EXPLICIT-CTOR",
                    "CXX-MOVE-CONST", "CXX-BIND-TMP", "CXX-EXPECTED-NULL",
                    "CXX-STD-FORMAT", "CXX-THIS-CAPTURE",
                    "CXX-SPACESHIP-DEFAULT",
                    "CXX-STD-ASYNC", "CXX-FUTURE-GET",
                    "CXX-STD-FUNCTION-NULL", "CXX-NODISCARD",
                    "CXX-STD-JTHREAD", "CXX-MDSPAN-DANGLE",
                    "CXX-ATOMIC-REF", "CXX-CONDITION-WAIT", "CXX-STD-BIND",
                    "CXX-ASSUME", "CXX-GENERATOR", "CXX-SHARED-MUTEX",
                    "CXX-ANY-CAST", "CXX-FILESYSTEM", "CXX-REGEX",
                    "CXX-LATCH", "CXX-FROM-CHARS", "CXX-INIT-LIST-DANGLE",
                    "CXX-STOP-TOKEN", "CXX-FLAT-MAP", "CXX-SEMAPHORE",
                    "CXX-STACKTRACE", "CXX-UNIQUE-RELEASE", "CXX-PACK-PRAGMA",
                    "CXX-FUNCTION-REF", "CXX-MOVE-ONLY-FUNCTION",
                    "CXX-RANGES-DANGLE", "CXX-CHRONO-SEED",
                    "CXX-INPLACE-VECTOR", "CXX-FLAT-SET",
                    "CXX-COPYABLE-FUNCTION", "CXX-HIVE",
                    "CXX-BITSET-INDEX", "CXX-SSTREAM-VIEW",
                    "CXX-OPTIONAL-VALUE", "CXX-INDIRECT",
                    "CXX-TO-CHARS", "CXX-HAZARD-POINTER",
                    "CXX-TEXT-ENCODING", "CXX-EXPECTED-ERROR",
                    "CXX-VARIANT-VALUELSS", "CXX-SIMD-INDEX",
                    "CXX-RCU", "CXX-LINALG", "CXX-SYNC-WAIT",
                    "CXX-EMBED", "CXX-CONTRACTS", "CXX-REFLECTION",
                    "CXX-OUT-PTR", "CXX-FLAT-MULTIMAP", "CXX-SPANSTREAM",
                    "CXX-BARRIER", "CXX-TASK", "CXX-GENERATOR-DISCARD",
                    "CXX-OSYNCSTREAM", "CXX-PACKAGED-TASK",
                    "CXX-FLAT-MULTISET", "CXX-SYNCBUF",
                    "CXX-COUNTED-ITERATOR", "CXX-PROMISE",
                    "CXX-WEAK-PTR", "CXX-EXCEPTION-PTR", "CXX-CORO-HANDLE",
                    "CXX-VALARRAY", "CXX-TO-UNDERLYING", "CXX-UNEXPECTED",
                    "CXX-TUPLE-GET", "CXX-DEQUE-INDEX", "CXX-FORWARD-LIST",
                    "CXX-LIST-FRONT", "CXX-MAP-AT", "CXX-UNORDERED-AT",
                    "CXX-SET-FIND", "CXX-QUEUE-FRONT", "CXX-STACK-TOP",
                    "CXX-PRIORITY-QUEUE", "CXX-ARRAY-INDEX",
                    "CXX-UNORDERED-SET",
                    "CXX-WSTRING-VIEW", "CXX-MULTIMAP-FIND",
                    "CXX-MULTISET-FIND", "CXX-BINARY-SEMAPHORE",
                    "CXX-ERROR-CODE", "CXX-BYTESWAP",
                    "CXX-PMR", "CXX-U8STRING-VIEW",
                    "CXX-UNORDERED-MULTIMAP", "CXX-UNORDERED-MULTISET",
                    "CXX-SHARED-LOCK", "CXX-ATOMIC-FLAG",
                    "CXX-CONDVAR-ANY", "CXX-RECURSIVE-MUTEX",
                    "CXX-TIMED-MUTEX", "CXX-FSTREAM",
                    "CXX-THIS-THREAD", "CXX-CALL-ONCE",
                    "CXX-SHARED-TIMED-MUTEX", "CXX-RECURSIVE-TIMED-MUTEX",
                    "CXX-SYSTEM-ERROR", "CXX-CHRONO-TZDB",
                    "CXX-RANGES-ZIP", "CXX-FORMAT-TO",
                    "CXX-ERROR-CATEGORY", "CXX-NESTED-EXCEPTION",
                    "CXX-ATOMIC-FENCE", "CXX-NOTIFY-THREAD-EXIT",
                    "CXX-WSTRING-CONVERT", "CXX-INVOKE",
                    "CXX-APPLY", "CXX-REFERENCE-WRAPPER", "CXX-ENDIAN",
                    "CXX-BIT-CEIL", "CXX-UNCAUGHT-EXCEPTIONS",
                    "CXX-RANGES-JOIN",
                    "CXX-QUICK-EXIT", "CXX-TO-ARRAY", "CXX-ZONED-TIME",
                    "CXX-KILL-DEPENDENCY", "CXX-ROTL",
                    "CXX-CURRENT-EXCEPTION",
                    "CXX-BIT-WIDTH", "CXX-LERP", "CXX-MIDPOINT",
                    "CXX-CMP-LESS", "CXX-COUNTL-ZERO", "CXX-UNREACHABLE",
                    "CXX-GCD", "CXX-LCM", "CXX-CLAMP", "CXX-EXCHANGE",
                    "CXX-TO-ADDRESS", "CXX-IS-CONSTANT-EVALUATED",
                    "CXX-ADDRESSOF", "CXX-ASSUME-ALIGNED", "CXX-AS-CONST",
                    "CXX-EXCLUSIVE-SCAN", "CXX-MAKE-EXCEPTION-PTR",
                    "CXX-SET-TERMINATE",
                    "CXX-INCLUSIVE-SCAN", "CXX-TRANSFORM-REDUCE",
                    "CXX-REDUCE", "CXX-UNINITIALIZED-COPY",
                    "CXX-CONSTRUCT-AT", "CXX-FORWARD-LIKE",
                    "CXX-UNINITIALIZED-FILL", "CXX-DESTROY-N",
                    "CXX-ADD-SAT", "CXX-TRANSFORM-SCAN",
                    "CXX-TYPE-IDENTITY", "CXX-NONTYPE",
                    "CXX-LAYOUT-COMPATIBLE", "CXX-PTR-INTERCONVERTIBLE",
                    "CXX-UNINITIALIZED-VALUE", "CXX-CONST-ITERATOR",
                    "CXX-CORRESPONDING-MEMBER", "CXX-RANGES-TO",
                    "CXX-ENUMERATE", "CXX-CARTESIAN-PRODUCT", "CXX-CHUNK",
                    "CXX-SLIDE", "CXX-ADJACENT", "CXX-JOIN-WITH",
                    "CXX-ZIP-TRANSFORM", "CXX-AS-RVALUE", "CXX-FROM-RANGE",
                    "CXX-SCOPED-ENUM", "CXX-STRIDE", "CXX-REPEAT",
                    "CXX-TAKE", "CXX-DROP", "CXX-FILTER",
                    "CXX-TRANSFORM-VIEW", "CXX-ELEMENTS", "CXX-IOTA",
                    "CXX-TAKE-WHILE", "CXX-DROP-WHILE", "CXX-KEYS",
                    "CXX-VALUES", "CXX-REVERSE-VIEW", "CXX-COUNTED",
                    "API-GETENV-NULL", "API-STRDUP-NULL", "MEM-SIZEOF-PTR",
                    "API-POPEN",
                    "API-UMASK", "CRYPTO-SRAND", "MEM-REALLOC-ZERO",
                    "API-FORK", "API-EXEC", "API-MMAP", "STR-WCSCPY",
                    "API-GETCWD", "API-IOCTL",
                    "API-DLOPEN", "API-ACCEPT", "API-REALPATH",
                    "API-CHMOD-WORLD", "INT-CLZ-ZERO", "MEM-BCOPY",
                    "API-SETUID", "API-SOCKET", "API-BIND",
                    "STR-SNPRINTF", "API-UNLINK", "API-MKFIFO",
                    "API-LISTEN", "API-CONNECT", "API-PIPE",
                    "API-DUP", "API-FCNTL", "API-WAIT",
                    "API-SELECT", "API-SEND", "API-SHUTDOWN",
                    "API-KILL", "API-GETADDRINFO",
                    "API-PTHREAD-JOIN", "API-THRD-JOIN", "API-SEM-WAIT", "API-OPENAT",
                    "API-FLOCK", "API-CHOWN", "API-SYMLINK",
                    "API-OPENDIR", "API-SETRLIMIT", "API-GETSOCKOPT",
                    "API-STAT", "API-MKDIR", "API-GETPWUID",
                    "API-CLOCK-GETTIME", "API-SHM-OPEN", "API-POSIX-SPAWN",
                    "API-GLOB", "API-FSEEK", "API-ACCESS",
                    "API-GETOPT", "API-UNAME", "API-SENDFILE",
                    "API-MEMFD", "API-PRCTL", "API-TCGETATTR",
                    "API-SYSCONF", "API-GETRUSAGE", "API-NFTW",
                    "API-WORDEXP", "API-GETLOGIN", "API-INET-PTON",
                    "API-MLOCK", "API-SPLICE", "API-INOTIFY",
                    "API-FSYNC", "API-GETRANDOM", "API-GETLINE",
                    "API-ASPRINTF", "API-STRLCPY", "API-ISATTY",
                    "API-PTSNAME", "API-MOUNT", "API-FMEMOPEN",
                    "API-SCANDIR", "API-SETXATTR", "API-SCHED-AFFINITY",
                    "API-AIO", "API-STATX", "API-PIDFD",
                    "API-CAPSET", "API-FANOTIFY", "API-SECCOMP",
                    "API-GETGRNAM", "API-FALLOCATE", "API-CLOSE-RANGE",
                    "API-BPF", "API-USERFAULTFD", "API-GETPASS",
                    "API-INITGROUPS", "API-CLONE", "API-OPENAT2",
                    "API-LANDLOCK", "API-GETPRIORITY", "API-SIGNALFD",
                    "API-SENDMMSG", "API-PERSONALITY", "API-QUOTACTL",
                    "API-NAME-TO-HANDLE", "API-PROCESS-MADVISE",
                    "API-PIVOT-ROOT", "API-STATFS", "API-PRLIMIT",
                    "API-PERF-EVENT",
                    "API-MEMBARRIER", "API-PKEY", "API-SYNCFS",
                    "API-PROCESS-VM", "API-CLONE3", "API-FUTEX",
                    "API-KEYCTL", "API-KCMP", "API-FSOPEN",
                    "API-MQ-OPEN", "API-SHMGET", "API-REBOOT",
                    "API-ADJTIMEX", "API-SETHOSTNAME", "API-SWAPON",
                    "API-ACCT", "API-IOPERM", "API-MINCORE",
                    "API-RSEQ", "API-TIMER-CREATE", "API-SEMGET",
                    "API-MSGGET", "API-KLOGCTL", "API-MOUNT-SETATTR",
                    "API-GETCPU", "API-PROCESS-MRELEASE", "API-MEMFD-SECRET",
                    "API-IOPRIO", "API-INIT-MODULE", "API-KEXEC",
                    "API-QUOTACTL-FD", "API-PKEY-FREE", "API-TGKILL",
                    "API-ADD-KEY", "API-SEMCTL", "API-MSGCTL",
                    "API-IO-SETUP", "API-REQUEST-KEY", "API-TKILL",
                    "API-TIMER-DELETE", "API-MQ-UNLINK", "API-SHMAT",
                    "API-SEMOP", "API-MSGSND", "API-SYNC-FILE-RANGE",
                    "API-MSYNC", "API-SOCKETPAIR", "API-SYSINFO",
                    "API-CLOCK-SETTIME", "API-SETTIMEOFDAY", "API-GETTID",
                    "API-SCHED-SETSCHEDULER", "API-SETITIMER", "API-NICE",
                    "API-ARCH-PRCTL", "API-GETDENTS", "API-UTIMENSAT",
                    "API-LINKAT", "API-MBIND", "API-FUTEX-WAITV",
                    "API-SYSLOG", "API-SETPGID", "API-SETREUID",
                    "API-GETGROUPS", "API-EPOLL-CREATE", "API-TIMERFD-SETTIME",
                    "API-REMAP-FILE-PAGES", "API-MOVE-PAGES", "API-CACHESTAT",
                    "API-MAP-SHADOW-STACK", "API-SCHED-YIELD", "API-SETFSUID",
                    "API-WAIT4", "API-PREADV", "API-SENDMSG",
                    "API-GETSOCKNAME", "API-EPOLL-PWAIT",
                    "API-INOTIFY-RM-WATCH", "API-EVENTFD-READ",
                    "API-SCHED-SETATTR", "API-RENAMEAT2", "API-EXECVEAT",
                    "API-MLOCK2", "API-FACCESSAT2",
                    "API-POSIX-FADVISE", "API-READAHEAD", "API-SIGACTION",
                    "API-SIGPROCMASK", "API-SEM-OPEN", "API-RWLOCK",
                    "API-PTHREAD-COND", "API-SIGALTSTACK", "API-RENAMEAT",
                    "API-FACCESSAT", "API-FCHMODAT", "API-PTHREAD-BARRIER",
                    "API-SYMLINKAT", "API-UNLINKAT", "API-MKDIRAT",
                    "API-MKNODAT", "API-READLINKAT", "API-FSTATAT",
                    "API-PTHREAD-SPIN", "API-PTHREAD-KEY",
                    "API-PTHREAD-CANCEL", "API-PTHREAD-KILL",
                    "API-PTHREAD-SIGMASK", "API-PTHREAD-ATFORK",
                    "API-PLEDGE", "API-UNVEIL", "API-SYSCTL",
                    "API-KQUEUE", "API-KEVENT", "API-PAUSE",
                    "API-PPOLL", "API-SIGWAIT", "API-SIGQUEUE",
                    "API-UCONTEXT", "API-SEM-TIMEDWAIT", "API-PTHREAD-ATTR",
                    "API-CAP-ENTER", "API-CAP-RIGHTS", "API-PDFORK",
                    "API-PROCCTL", "API-CLOSEFROM", "API-ISSETUGID",
                    "API-ARC4RANDOM", "API-CHFLAGS", "API-GETFSSTAT",
                    "API-PTHREAD-YIELD", "API-SEM-TRYWAIT", "API-ADJTIME",
                    "API-REVOKE", "API-KTRACE", "API-RFORK", "API-JAIL",
                    "API-SETLOGIN", "API-GETRESUID", "API-GETPEEREID",
                    "API-STRTONUM", "API-REALLOCARRAY", "API-TIMINGSAFE",
                    "API-GETPROGNAME", "API-DAEMON",
                    "API-CAP-FCNTLS", "API-PDGETPID", "API-KLDLOAD",
                    "API-EXTATTR", "API-MAC", "API-AUDIT", "API-KVM",
                    "API-REALLOCF", "API-UUIDGEN", "API-SETFIB",
                    "API-NTP-GETTIME", "API-CRYPT-NEWHASH",
                    "API-WAIT6", "API-CPUSET", "API-RTPRIO", "API-KENV",
                    "API-GETFH", "API-GETMNTINFO", "API-NMOUNT",
                    "API-STRMODE", "API-GETOSRELDATE", "API-CAP-SANDBOXED",
                    "API-GETGROUPLIST", "API-EACCESS",
                    "API-LOGIN-GETCLASS", "API-FFLAGS", "API-GETDIRENTRIES",
                    "API-KINFO", "API-UMTX", "API-THR", "API-MODFIND",
                    "API-LPATHCONF", "API-LOGINCLASS", "API-GETFSENT",
                    "API-MINHERIT", "API-CAP-GETMODE",
                    "API-NFSSVC", "API-SYSARCH", "API-GETPAGESIZES",
                    "API-SBRK", "API-KSEM", "API-CAP-GETRIGHTS",
                    "API-DEVNAME", "API-GETBOOTFILE", "API-KLDFIRSTMOD",
                    "API-FHLINK", "API-VALLOC", "API-GETDOMAINNAME",
                    "STR-STRNCPY-NUL"):
            self.assertIn(cid, rows)
            self.assertEqual(rows[cid]["verdict"], "GAP")

    def test_inventory_error_empty_tu_is_covered(self):
        f = Finding(
            stage="inventory", status=laws.ERROR, file="empty_tu.c",
            function=None, line=None, cls="EMPTY-TU",
            message="no functions parsed (not a clean unit)",
            strength=laws.STRENGTH_FINDS,
        )
        rows = {r["id"]: r for r in coverage_from_report(_rep(f, stage="inventory"))}
        self.assertEqual(rows["EMPTY-TU"]["verdict"], "COVERED")

    def test_ltl_hypothesis_is_not_covered(self):
        f = Finding(
            stage="ltl", status=laws.HYPOTHESIS, file="fsm.c", function="step",
            line=1, cls="LTL-SAFETY", message="synthesis",
            strength=laws.STRENGTH_READS,
        )
        rows = {r["id"]: r for r in coverage_from_report(_rep(f, stage="ltl"))}
        self.assertNotEqual(rows["LTL-SAFETY"]["verdict"], "COVERED")

    def test_llm_only_hypothesis_report_is_not_covered(self):
        f = Finding(
            stage="llm", status=laws.HYPOTHESIS, file="a.c", function="add",
            line=1, cls="FUNC-CONTRACT", message="maybe ensures",
            strength=laws.STRENGTH_READS,
        )
        rows = coverage_from_report(_rep(f, stage="llm"))
        by_id = {r["id"]: r for r in rows}
        self.assertIn(by_id["FUNC-CONTRACT"]["verdict"], {"GAP", "PARTIAL"})
        self.assertNotEqual(by_id["FUNC-CONTRACT"]["verdict"], "COVERED")
        self.assertIn(by_id["INTENT"]["verdict"], {"GAP", "PARTIAL"})
        covered = [r["id"] for r in rows if r["verdict"] == "COVERED"]
        self.assertEqual(covered, [])

    def test_wp_proved_assuming_covers_func_contract(self):
        fc = next(c for c in CLASSES if c["id"] == "FUNC-CONTRACT")
        self.assertEqual(fc["seen"].get("wp"), laws.STRENGTH_PROVES)
        f = Finding(
            stage="wp", status=laws.PROVED_ASSUMING, file="a.c", function="abs",
            line=1, cls="FUNC-CONTRACT",
            message="WP of ensures holds assuming requires; never PROVED",
            strength=laws.STRENGTH_PROVES,
        )
        rows = {r["id"]: r for r in coverage_from_report(_rep(f, stage="wp"))}
        self.assertEqual(rows["FUNC-CONTRACT"]["verdict"], "COVERED")
        self.assertEqual(rows["FUNC-CONTRACT"]["best"], laws.STRENGTH_PROVES)
        for cid in ("INT-SIGNED-OVF", "INT-DIV-ZERO", "INT-SHIFT-UB",
                    "MEM-OOB-READ", "MEM-OOB-WRITE"):
            self.assertNotEqual(rows[cid]["verdict"], "COVERED")

    def test_empty_functions_confidence_is_zero(self):
        vis, ans, res, conf = score(RunReport(root="x"))
        self.assertEqual((vis, ans, res, conf), (0.0, 0.0, 0.0, 0.0))

    def test_unify_clean_coverage_is_not_a_proof(self):
        prism = (Path(__file__).resolve().parents[1] / "prism" / "pipeline.py").read_text(
            encoding="utf-8"
        )
        start = prism.find("def unify()")
        self.assertGreater(start, 0)
        block = prism[start:prism.find("self._stage(", start)]
        self.assertIn("not a proof", block)
        with tempfile.TemporaryDirectory(prefix="prism_unify_") as td:
            out = Path(td)
            report = run_pipeline(Config(
                root=out, out=out / "out", llm=False, stages=["unify"], skip=[],
            ))
        rec = next(s for s in report.stages if s.name == "unify")
        self.assertTrue(rec.findings)
        f = rec.findings[0]
        self.assertIn("not a proof", f.message)
        self.assertEqual(f.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(f.status))
        self.assertEqual((f.extra or {}).get("not_a_proof"), "true")
        cpp = (Path(__file__).resolve().parents[1] / "src" / "prism" / "pipeline.cpp").read_text(
            encoding="utf-8"
        )
        unify = cpp.split('stage("unify"', 1)[1]
        unify = unify.split("apply_confidence", 1)[0]
        self.assertIn("not a proof", unify)
        self.assertIn('f.extra["not_a_proof"] = "true"', unify)

    def test_unify_clean_spoofed_cls_is_not_covered(self):
        f = Finding(
            stage="unify", status=laws.CLEAN, file="", function=None,
            line=None, cls="INT-SIGNED-OVF",
            message="taxonomy 1/1 COVERED, 0 GAP (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"not_a_proof": "true"},
        )
        rows = {r["id"]: r for r in coverage_from_report(_rep(f, stage="unify"))}
        self.assertNotEqual(rows["INT-SIGNED-OVF"]["verdict"], "COVERED")
        self.assertFalse(laws.is_proof(f.status))


if __name__ == "__main__":
    unittest.main()
