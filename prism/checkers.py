"""Portable ParanoidBSD-shape checkers. Line-preserving comment strip."""

from __future__ import annotations

from collections.abc import Callable, Iterator
import dataclasses
from pathlib import Path
import re
from typing import Any, NamedTuple

from prism import laws
from prism.cparse import (
    _match_brace,
    extract_functions_from_text,
    strip_comments_keep_lines,
)
from prism.models import Finding, FunctionInfo

SHIFT31 = re.compile(
    r"(?<![A-Za-z0-9_])1\s*<<\s*(?:31|0x1f|0x1F)\b"
)
DIVZERO = re.compile(
    r"(?P<op>/|%)\s*(?P<rhs>0\b|[A-Za-z_]\w*)"
)
REALLOC_SELF = re.compile(
    r"(?P<p>[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)*)\s*=\s*"
    r"realloc\s*\(\s*(?P=p)\s*,"
)
ONESIDED = re.compile(
    r"\bif\s*\(\s*(?P<i>[A-Za-z_]\w*)\s*(?:>|>=)\s*(?P<n>[A-Za-z_]\w*)\s*\)"
)
# Persistent struct-field capacity grown in place, then used as an
# allocation or memcpy size. Essence of ParanoidBSD capacity_first.py.
_CAP_FIELD = r"[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)+"
CAP_GROW = re.compile(
    r"^\s*(?:(?P<a>" + _CAP_FIELD + r")\s*(?:\+=|\*=|<<=)\s*[^=]"
    r"|\+\+\s*(?P<b>" + _CAP_FIELD + r")"
    r"|(?P<c>" + _CAP_FIELD + r")\s*\+\+)"
)
CAP_ALLOC = re.compile(
    r"\b(?:realloc|reallocarray|malloc|calloc|mallocarray|memcpy|memmove)\s*\("
)
CAP_NULLT = re.compile(r"==\s*(?:NULL|nullptr|0)\b|(?:NULL|nullptr)\s*==|!\s*[A-Za-z_]")
CAP_LEAVE = re.compile(r"\b(?:return|goto|break|continue)\b")
CAP_DEAD = re.compile(
    r"\b(?:err|errx|xo_err|xo_errx|abort|exit|panic|_errx|"
    r"out_of_mem|AbortProgram|fatal|bsdar_errc)\s*\("
)
_UNSIGNED_TY = re.compile(r"unsigned|size_t|uint|u_int|u_char|u_short|u_long")
_SIGNED_WORDS = {
    "int", "short", "long", "char", "signed", "ssize_t", "ptrdiff_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "intmax_t", "intptr_t",
    "off_t", "pid_t",
}
_KW = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "else", "do", "case", "default",
}
NULL_TEST = re.compile(
    r"\bif\s*\(\s*(?:(?P<p1>[A-Za-z_]\w*)\s*==\s*(?:NULL|0|nullptr)"
    r"|!\s*(?P<p2>[A-Za-z_]\w*))\s*\)"
)
MASKED_SWITCH = re.compile(
    r"switch\s*\(\s*([^)]*?&\s*(?:\([^)]+\)|0x[0-9a-fA-F]+|\d+))\s*\)",
    re.S,
)
# `clock()` / `read_block()` are not locks (tinyexpr benchmark.c: clock()
# "acquired twice without a release").
LOCK_CALL = re.compile(
    r"\b(?![cC]lock\w*\s*\(|\w*[bB]lock\w*\s*\()([A-Za-z_]\w*lock[A-Za-z0-9_]*)\s*\(([^)]*)\)\s*;")
_ALLOC_VAR = (
    r"(?P<var>[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*|\s*\[[^\]]*\])*)"
)
UNCHECKED_ALLOC = re.compile(
    _ALLOC_VAR + r"\s*=\s*(?:\([^)]*\)\s*)?"
    r"(?P<fn>malloc|calloc|realloc)\s*\("
)
NOWAIT_ALLOC = re.compile(
    _ALLOC_VAR + r"\s*=\s*(?:\([^)]*\)\s*)?"
    r"(?P<fn>malloc|realloc)\s*\([^)]*M_NOWAIT"
)
NORETURN_ATTR = re.compile(
    r"__dead2|__dead\b|_Noreturn|\bnoreturn\b|"
    r"__attribute__\s*\(\s*\(\s*noreturn\s*\)\s*\)"
)
FATAL_TAIL = re.compile(
    r"^\s*(?:\(\s*void\s*\)\s*)?(?P<callee>exit|_exit|abort|err|errx)\s*\("
)
FREE_CALL = re.compile(
    r"\b(?:free|kfree|ck_free)\s*\(\s*(?:\([^)]*\)\s*)*"
    r"(?P<var>[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)*)\s*\)"
)
_FMT_FN: dict[str, int] = {
    "printf": 0, "fprintf": 1, "sprintf": 1, "snprintf": 2,
    "warn": 0, "err": 0, "syslog": 0,
}


def _align_bodies(funcs: list[FunctionInfo], lines: list[str]) -> list[FunctionInfo]:
    """Bodies padded so body line i is file line span[0] + i.

    A body starts after its `{`. With the brace on a line below the head
    (`int f(void)\\n{`), body-walking checkers reported one line early.
    """
    out: list[FunctionInfo] = []
    for f in funcs:
        head = f.span[0] - 1
        k = 0
        for j in range(head, min(len(lines), f.span[1])):
            if "{" in lines[j]:
                k = j - head
                break
        if k > 0:
            f = dataclasses.replace(f, body="\n" * k + f.body)
        out.append(f)
    return out


def _code_bodies(funcs: list[FunctionInfo]) -> list[FunctionInfo]:
    """Copies of `funcs` whose bodies have string-literal contents blanked.

    A body is comment-free and starts outside any literal, so stripping
    it again blanks exactly its literals (quotes, lengths, lines kept).
    """
    out: list[FunctionInfo] = []
    for f in funcs:
        body = f.body
        if '"' in body:
            f = dataclasses.replace(f, body=strip_comments_keep_lines(body))
        out.append(f)
    return out


def _findings_for_file(path: Path, rel: str) -> list[Finding]:
    text = path.read_text(encoding="utf-8", errors="replace")
    stripped = strip_comments_keep_lines(text)
    lines = stripped.splitlines()
    out: list[Finding] = []

    def add(cls: str, line: int, msg: str, fn: str | None = None) -> None:
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel, function=fn,
            line=line, cls=cls, message=msg, strength=laws.STRENGTH_FINDS,
            evidence=lines[line - 1].strip() if 0 < line <= len(lines) else "",
        ))

    # cparse bodies keep string literals. Pattern checks read code, so
    # they get bodies with literal contents blanked (quotes, lengths and
    # lines kept): `puts("never call gets(b)")` is not a gets() call.
    # Only checkers that read literal bytes (format specifiers, literal
    # lengths) get `raw_funcs`.
    raw_funcs = _align_bodies(
        extract_functions_from_text(text, rel or str(path), stripped), lines,
    )
    funcs = _code_bodies(raw_funcs)
    fn_at = []
    for f in funcs:
        fn_at.append((f.span[0], f.span[1], f.name))

    def func_of(ln: int) -> str | None:
        for a, b, n in fn_at:
            if a <= ln <= b:
                return n
        return None

    for i, ln in enumerate(lines, 1):
        if "<<" in ln and SHIFT31.search(ln) and not _rx(r"\bunsigned\b").search(ln):
            # 1<<31 into a signed int is UB. A cast on the 1 is the fix.
            if not _rx(r"\(\s*uint").search(ln) and not _rx(r"1u\s*<<", re.I).search(ln):
                add("INT-SHIFT-UB", i, "signed 1<<31 is undefined", func_of(i))
        m = REALLOC_SELF.search(ln) if "realloc" in ln else None
        if m:
            add("MEM-REALLOC-SELF", i, f"{m.group('p')} = realloc({m.group('p')}, …) leaks on failure", func_of(i))

    _masked_switch(lines, rel, func_of, out)
    _null_branch(lines, rel, func_of, out)
    _lock_balance(lines, rel, funcs, out)
    _lock_double_unlock(lines, rel, funcs, out)
    _lock_double_lock(lines, rel, funcs, out)
    _lock_missing_init(lines, rel, funcs, out)
    _div_zero_const(lines, rel, func_of, out)
    _onesided_index(lines, rel, funcs, out)
    _capacity_first(lines, rel, func_of, out)
    _mem_overlap(lines, rel, funcs, out)
    _unchecked_alloc(lines, rel, funcs, out)
    _nowait_alloc(lines, rel, funcs, out)
    _noreturn_fatal(lines, rel, funcs, out)
    _mem_lifetime(lines, rel, funcs, out)
    _fmt_string(lines, rel, funcs, out)
    _memset_swap(lines, rel, funcs, out)
    _taut_bound(lines, rel, funcs, out)
    _bool_as_bit(lines, rel, funcs, out)
    _wrap_alloc(lines, rel, funcs, out)
    _int_trunc(lines, rel, funcs, out)
    _int_sign_conv(lines, rel, funcs, out)
    _stack_escape(lines, rel, funcs, out)
    _unbounded_copy(lines, rel, funcs, out)
    _str_sprintf(lines, rel, funcs, out)
    _missing_return(lines, rel, funcs, out)
    _fallthrough(lines, text.splitlines(), rel, funcs, out)
    _dead_guard(lines, rel, funcs, out)
    _empty_infinite(lines, rel, funcs, out)
    _uninit_return(lines, rel, funcs, out)
    _ptr_uninit(lines, rel, funcs, out)
    _uninit_branch(lines, rel, funcs, out)
    _off_by_one(lines, rel, funcs, out, text)
    _str_missing_nul(lines, rel, raw_funcs, out)
    _sibling_asymmetry(lines, rel, funcs, out)
    _ignored_error(lines, rel, func_of, out)
    _scanf_unchecked(lines, rel, func_of, out)
    _api_gets(lines, rel, funcs, out)
    _api_strtok(lines, rel, funcs, out)
    _api_mkstemp(lines, rel, funcs, out)
    _api_tmpnam(lines, rel, funcs, out)
    _api_mktemp(lines, rel, funcs, out)
    _api_signal(lines, rel, funcs, out)
    _fmt_percent_n(lines, rel, raw_funcs, out)
    _api_system(lines, rel, funcs, out)
    _api_getenv_null(lines, rel, funcs, out)
    _api_strdup_null(lines, rel, funcs, out)
    _api_chroot(lines, rel, funcs, out)
    _api_umask(lines, rel, funcs, out)
    if path.suffix.lower() not in {".cpp", ".cc", ".cxx", ".ii"}:
        _api_fork(lines, rel, funcs, out)
        _api_exec(lines, rel, funcs, out)
        _api_mmap(lines, rel, funcs, out)
        _wcs_unbounded(lines, rel, funcs, out)
        _api_getcwd(lines, rel, funcs, out)
        _api_ioctl(lines, rel, funcs, out)
        _api_dlopen(lines, rel, funcs, out)
        _api_accept(lines, rel, funcs, out)
        _api_realpath(lines, rel, funcs, out)
        _api_chmod_world(lines, rel, funcs, out)
        _int_clz_zero(lines, rel, funcs, out)
        _mem_bcopy(lines, rel, funcs, out)
        _api_setuid(lines, rel, funcs, out)
        _api_socket(lines, rel, funcs, out)
        _api_bind(lines, rel, funcs, out)
        _api_listen(lines, rel, funcs, out)
        _api_connect(lines, rel, funcs, out)
        _api_pipe(lines, rel, funcs, out)
        _api_dup(lines, rel, funcs, out)
        _api_fcntl(lines, rel, funcs, out)
        _api_wait(lines, rel, funcs, out)
        _api_select(lines, rel, funcs, out)
        _api_send(lines, rel, funcs, out)
        _api_shutdown(lines, rel, funcs, out)
        _api_kill(lines, rel, funcs, out)
        _api_getaddrinfo(lines, rel, funcs, out)
        _api_pthread_join(lines, rel, funcs, out)
        _api_thrd_join(lines, rel, funcs, out)
        _api_sem_wait(lines, rel, funcs, out)
        _api_openat(lines, rel, funcs, out)
        _api_flock(lines, rel, funcs, out)
        _api_chown(lines, rel, funcs, out)
        _api_symlink(lines, rel, funcs, out)
        _api_opendir(lines, rel, funcs, out)
        _api_setrlimit(lines, rel, funcs, out)
        _api_getsockopt(lines, rel, funcs, out)
        _api_stat(lines, rel, funcs, out)
        _api_mkdir(lines, rel, funcs, out)
        _api_getpwuid(lines, rel, funcs, out)
        _api_clock_gettime(lines, rel, funcs, out)
        _api_shm_open(lines, rel, funcs, out)
        _api_posix_spawn(lines, rel, funcs, out)
        _api_glob(lines, rel, funcs, out)
        _api_fseek(lines, rel, funcs, out)
        _api_access(lines, rel, funcs, out)
        _api_getopt(lines, rel, funcs, out)
        _api_uname(lines, rel, funcs, out)
        _api_sendfile(lines, rel, funcs, out)
        _api_memfd(lines, rel, funcs, out)
        _api_prctl(lines, rel, funcs, out)
        _api_tcgetattr(lines, rel, funcs, out)
        _api_sysconf(lines, rel, funcs, out)
        _api_getrusage(lines, rel, funcs, out)
        _api_nftw(lines, rel, funcs, out)
        _api_wordexp(lines, rel, funcs, out)
        _api_getlogin(lines, rel, funcs, out)
        _api_inet_pton(lines, rel, funcs, out)
        _api_mlock(lines, rel, funcs, out)
        _api_splice(lines, rel, funcs, out)
        _api_inotify(lines, rel, funcs, out)
        _api_fsync(lines, rel, funcs, out)
        _api_getrandom(lines, rel, funcs, out)
        _api_getline(lines, rel, funcs, out)
        _api_asprintf(lines, rel, funcs, out)
        _api_strlcpy(lines, rel, funcs, out)
        _api_isatty(lines, rel, funcs, out)
        _api_ptsname(lines, rel, funcs, out)
        _api_mount(lines, rel, funcs, out)
        _api_fmemopen(lines, rel, funcs, out)
        _api_scandir(lines, rel, funcs, out)
        _api_setxattr(lines, rel, funcs, out)
        _api_sched_affinity(lines, rel, funcs, out)
        _api_aio(lines, rel, funcs, out)
        _api_statx(lines, rel, funcs, out)
        _api_pidfd(lines, rel, funcs, out)
        _api_capset(lines, rel, funcs, out)
        _api_fanotify(lines, rel, funcs, out)
        _api_seccomp(lines, rel, funcs, out)
        _api_getgrnam(lines, rel, funcs, out)
        _api_fallocate(lines, rel, funcs, out)
        _api_close_range(lines, rel, funcs, out)
        _api_bpf(lines, rel, funcs, out)
        _api_userfaultfd(lines, rel, funcs, out)
        _api_getpass(lines, rel, funcs, out)
        _api_initgroups(lines, rel, funcs, out)
        _api_clone(lines, rel, funcs, out)
        _api_openat2(lines, rel, funcs, out)
        _api_landlock(lines, rel, funcs, out)
        _api_getpriority(lines, rel, funcs, out)
        _api_signalfd(lines, rel, funcs, out)
        _api_sendmmsg(lines, rel, funcs, out)
        _api_personality(lines, rel, funcs, out)
        _api_quotactl(lines, rel, funcs, out)
        _api_name_to_handle(lines, rel, funcs, out)
        _api_process_madvise(lines, rel, funcs, out)
        _api_pivot_root(lines, rel, funcs, out)
        _api_statfs(lines, rel, funcs, out)
        _api_prlimit(lines, rel, funcs, out)
        _api_perf_event(lines, rel, funcs, out)
        _api_membarrier(lines, rel, funcs, out)
        _api_pkey(lines, rel, funcs, out)
        _api_syncfs(lines, rel, funcs, out)
        _api_process_vm(lines, rel, funcs, out)
        _api_clone3(lines, rel, funcs, out)
        _api_futex(lines, rel, funcs, out)
        _api_keyctl(lines, rel, funcs, out)
        _api_kcmp(lines, rel, funcs, out)
        _api_fsopen(lines, rel, funcs, out)
        _api_mq_open(lines, rel, funcs, out)
        _api_shmget(lines, rel, funcs, out)
        _api_reboot(lines, rel, funcs, out)
        _api_adjtimex(lines, rel, funcs, out)
        _api_sethostname(lines, rel, funcs, out)
        _api_swapon(lines, rel, funcs, out)
        _api_acct(lines, rel, funcs, out)
        _api_ioperm(lines, rel, funcs, out)
        _api_mincore(lines, rel, funcs, out)
        _api_rseq(lines, rel, funcs, out)
        _api_timer_create(lines, rel, funcs, out)
        _api_semget(lines, rel, funcs, out)
        _api_msgget(lines, rel, funcs, out)
        _api_klogctl(lines, rel, funcs, out)
        _api_mount_setattr(lines, rel, funcs, out)
        _api_getcpu(lines, rel, funcs, out)
        _api_process_mrelease(lines, rel, funcs, out)
        _api_memfd_secret(lines, rel, funcs, out)
        _api_ioprio(lines, rel, funcs, out)
        _api_init_module(lines, rel, funcs, out)
        _api_kexec(lines, rel, funcs, out)
        _api_quotactl_fd(lines, rel, funcs, out)
        _api_pkey_free(lines, rel, funcs, out)
        _api_tgkill(lines, rel, funcs, out)
        _api_add_key(lines, rel, funcs, out)
        _api_semctl(lines, rel, funcs, out)
        _api_msgctl(lines, rel, funcs, out)
        _api_io_setup(lines, rel, funcs, out)
        _api_request_key(lines, rel, funcs, out)
        _api_tkill(lines, rel, funcs, out)
        _api_timer_delete(lines, rel, funcs, out)
        _api_mq_unlink(lines, rel, funcs, out)
        _api_shmat(lines, rel, funcs, out)
        _api_semop(lines, rel, funcs, out)
        _api_msgsnd(lines, rel, funcs, out)
        _api_sync_file_range(lines, rel, funcs, out)
        _api_msync(lines, rel, funcs, out)
        _api_socketpair(lines, rel, funcs, out)
        _api_sysinfo(lines, rel, funcs, out)
        _api_clock_settime(lines, rel, funcs, out)
        _api_settimeofday(lines, rel, funcs, out)
        _api_gettid(lines, rel, funcs, out)
        _api_sched_setscheduler(lines, rel, funcs, out)
        _api_setitimer(lines, rel, funcs, out)
        _api_nice(lines, rel, funcs, out)
        _api_arch_prctl(lines, rel, funcs, out)
        _api_getdents(lines, rel, funcs, out)
        _api_utimensat(lines, rel, funcs, out)
        _api_linkat(lines, rel, funcs, out)
        _api_mbind(lines, rel, funcs, out)
        _api_futex_waitv(lines, rel, funcs, out)
        _api_syslog(lines, rel, funcs, out)
        _api_setpgid(lines, rel, funcs, out)
        _api_setreuid(lines, rel, funcs, out)
        _api_getgroups(lines, rel, funcs, out)
        _api_epoll_create(lines, rel, funcs, out)
        _api_timerfd_settime(lines, rel, funcs, out)
        _api_remap_file_pages(lines, rel, funcs, out)
        _api_move_pages(lines, rel, funcs, out)
        _api_cachestat(lines, rel, funcs, out)
        _api_map_shadow_stack(lines, rel, funcs, out)
        _api_sched_yield(lines, rel, funcs, out)
        _api_setfsuid(lines, rel, funcs, out)
        _api_wait4(lines, rel, funcs, out)
        _api_preadv(lines, rel, funcs, out)
        _api_sendmsg(lines, rel, funcs, out)
        _api_getsockname(lines, rel, funcs, out)
        _api_epoll_pwait(lines, rel, funcs, out)
        _api_inotify_rm_watch(lines, rel, funcs, out)
        _api_eventfd_read(lines, rel, funcs, out)
        _api_sched_setattr(lines, rel, funcs, out)
        _api_renameat2(lines, rel, funcs, out)
        _api_execveat(lines, rel, funcs, out)
        _api_mlock2(lines, rel, funcs, out)
        _api_faccessat2(lines, rel, funcs, out)
        _api_posix_fadvise(lines, rel, funcs, out)
        _api_readahead(lines, rel, funcs, out)
        _api_sigaction(lines, rel, funcs, out)
        _api_sigprocmask(lines, rel, funcs, out)
        _api_sem_open(lines, rel, funcs, out)
        _api_rwlock(lines, rel, funcs, out)
        _api_pthread_cond(lines, rel, funcs, out)
        _api_sigaltstack(lines, rel, funcs, out)
        _api_renameat(lines, rel, funcs, out)
        _api_faccessat(lines, rel, funcs, out)
        _api_fchmodat(lines, rel, funcs, out)
        _api_pthread_barrier(lines, rel, funcs, out)
        _api_symlinkat(lines, rel, funcs, out)
        _api_unlinkat(lines, rel, funcs, out)
        _api_mkdirat(lines, rel, funcs, out)
        _api_mknodat(lines, rel, funcs, out)
        _api_readlinkat(lines, rel, funcs, out)
        _api_fstatat(lines, rel, funcs, out)
        _api_pthread_spin(lines, rel, funcs, out)
        _api_pthread_key(lines, rel, funcs, out)
        _api_pthread_cancel(lines, rel, funcs, out)
        _api_pthread_kill(lines, rel, funcs, out)
        _api_pthread_sigmask(lines, rel, funcs, out)
        _api_pthread_atfork(lines, rel, funcs, out)
        _api_pledge(lines, rel, funcs, out)
        _api_unveil(lines, rel, funcs, out)
        _api_sysctl(lines, rel, funcs, out)
        _api_kqueue(lines, rel, funcs, out)
        _api_kevent(lines, rel, funcs, out)
        _api_pause(lines, rel, funcs, out)
        _api_ppoll(lines, rel, funcs, out)
        _api_sigwait(lines, rel, funcs, out)
        _api_sigqueue(lines, rel, funcs, out)
        _api_ucontext(lines, rel, funcs, out)
        _api_sem_timedwait(lines, rel, funcs, out)
        _api_pthread_attr(lines, rel, funcs, out)
        _api_cap_enter(lines, rel, funcs, out)
        _api_cap_rights(lines, rel, funcs, out)
        _api_pdfork(lines, rel, funcs, out)
        _api_procctl(lines, rel, funcs, out)
        _api_closefrom(lines, rel, funcs, out)
        _api_issetugid(lines, rel, funcs, out)
        _api_arc4random(lines, rel, funcs, out)
        _api_chflags(lines, rel, funcs, out)
        _api_getfsstat(lines, rel, funcs, out)
        _api_pthread_yield(lines, rel, funcs, out)
        _api_sem_trywait(lines, rel, funcs, out)
        _api_adjtime(lines, rel, funcs, out)
        _api_revoke(lines, rel, funcs, out)
        _api_ktrace(lines, rel, funcs, out)
        _api_rfork(lines, rel, funcs, out)
        _api_jail(lines, rel, funcs, out)
        _api_setlogin(lines, rel, funcs, out)
        _api_getresuid(lines, rel, funcs, out)
        _api_getpeereid(lines, rel, funcs, out)
        _api_strtonum(lines, rel, funcs, out)
        _api_reallocarray(lines, rel, funcs, out)
        _api_timingsafe(lines, rel, funcs, out)
        _api_getprogname(lines, rel, funcs, out)
        _api_daemon(lines, rel, funcs, out)
        _api_cap_fcntls(lines, rel, funcs, out)
        _api_pdgetpid(lines, rel, funcs, out)
        _api_kldload(lines, rel, funcs, out)
        _api_extattr(lines, rel, funcs, out)
        _api_mac(lines, rel, funcs, out)
        _api_audit(lines, rel, funcs, out)
        _api_kvm(lines, rel, funcs, out)
        _api_reallocf(lines, rel, funcs, out)
        _api_uuidgen(lines, rel, funcs, out)
        _api_setfib(lines, rel, funcs, out)
        _api_ntp_gettime(lines, rel, funcs, out)
        _api_crypt_newhash(lines, rel, funcs, out)
        _api_wait6(lines, rel, funcs, out)
        _api_cpuset(lines, rel, funcs, out)
        _api_rtprio(lines, rel, funcs, out)
        _api_kenv(lines, rel, funcs, out)
        _api_getfh(lines, rel, funcs, out)
        _api_getmntinfo(lines, rel, funcs, out)
        _api_nmount(lines, rel, funcs, out)
        _api_strmode(lines, rel, funcs, out)
        _api_getosreldate(lines, rel, funcs, out)
        _api_cap_sandboxed(lines, rel, funcs, out)
        _api_getgrouplist(lines, rel, funcs, out)
        _api_eaccess(lines, rel, funcs, out)
        _api_login_getclass(lines, rel, funcs, out)
        _api_fflags(lines, rel, funcs, out)
        _api_getdirentries(lines, rel, funcs, out)
        _api_kinfo(lines, rel, funcs, out)
        _api_umtx(lines, rel, funcs, out)
        _api_thr(lines, rel, funcs, out)
        _api_modfind(lines, rel, funcs, out)
        _api_lpathconf(lines, rel, funcs, out)
        _api_loginclass(lines, rel, funcs, out)
        _api_getfsent(lines, rel, funcs, out)
        _api_minherit(lines, rel, funcs, out)
        _api_cap_getmode(lines, rel, funcs, out)
        _api_nfssvc(lines, rel, funcs, out)
        _api_sysarch(lines, rel, funcs, out)
        _api_getpagesizes(lines, rel, funcs, out)
        _api_sbrk(lines, rel, funcs, out)
        _api_ksem(lines, rel, funcs, out)
        _api_cap_getrights(lines, rel, funcs, out)
        _api_devname(lines, rel, funcs, out)
        _api_getbootfile(lines, rel, funcs, out)
        _api_kldfirstmod(lines, rel, funcs, out)
        _api_fhlink(lines, rel, funcs, out)
        _api_valloc(lines, rel, funcs, out)
        _api_getdomainname(lines, rel, funcs, out)
        _str_strncpy_nul(lines, rel, funcs, out)
        _str_snprintf(lines, rel, funcs, out)
        _api_unlink(lines, rel, funcs, out)
        _api_mkfifo(lines, rel, funcs, out)
    _int_atoi(lines, rel, funcs, out)
    _enum_hole(lines, rel, funcs, out)
    _fd_leak(lines, rel, funcs, out)
    _popen_leak(lines, rel, funcs, out)
    _mem_leak(lines, rel, funcs, out)
    _mismatched_free(lines, rel, funcs, out)
    _str_null_arg(lines, rel, funcs, out)
    _ptr_arith(lines, rel, funcs, out)
    _float_ub(lines, rel, funcs, out)
    _vla_size(lines, rel, funcs, out)
    _mem_alloca(lines, rel, funcs, out)
    _mem_sizeof_ptr(lines, rel, funcs, out)
    _mem_flex_array(stripped, lines, rel, funcs, out)
    _intent_mismatch(text.splitlines(), rel, funcs, out)
    _infoleak_pad(lines, rel, funcs, out)
    _api_precondition(text.splitlines(), rel, funcs, out)
    _trust_unvalidated_input(lines, rel, funcs, out)
    _crypto_misuse(lines, rel, funcs, out)
    _crypto_srand(lines, rel, funcs, out)
    _mem_realloc_zero(lines, rel, funcs, out)
    _lock_order(lines, rel, funcs, out)
    _conc_toctou(lines, rel, funcs, out)
    _conc_atomicity(lines, rel, funcs, out)
    if path.suffix.lower() in {".cpp", ".cc", ".cxx", ".ii"}:
        _cxx_new_delete(lines, rel, funcs, out)
        _cxx_use_after_move(lines, rel, funcs, out)
        _cxx_self_assign(lines, rel, funcs, out)
        _cxx_dangling_ref(lines, rel, funcs, out)
        _cxx_iter_invalid(lines, rel, funcs, out)
        _cxx_virtual_in_ctor(text, lines, rel, funcs, out)
        _cxx_exception_leak(lines, rel, funcs, out)
        _cxx_throw_destructor(stripped, lines, rel, out)
        _cxx_throw_spec(stripped, lines, rel, out)
        _cxx_slicing(stripped, lines, rel, funcs, out)
        _cxx_delete_this(lines, rel, funcs, out)
        _cxx_catch_by_value(lines, rel, funcs, out)
        _cxx_throw_noexcept(lines, rel, funcs, out)
        _cxx_missing_virtual_dtor(stripped, lines, rel, funcs, out)
        _cxx_lambda_dangle(lines, rel, funcs, out)
        _cxx_unique_reset(lines, rel, funcs, out)
        _cxx_const_cast(lines, rel, funcs, out)
        _cxx_dynamic_cast_null(lines, rel, funcs, out)
        _cxx_reinterpret(lines, rel, funcs, out)
        _cxx_throw_copy(lines, rel, funcs, out)
        _cxx_bit_cast(lines, rel, funcs, out)
        _cxx_placement_new(lines, rel, funcs, out)
        _cxx_std_thread(lines, rel, funcs, out)
        _cxx_optional_null(lines, rel, funcs, out)
        _cxx_variant_get(lines, rel, funcs, out)
        _cxx_span_dangle(lines, rel, funcs, out)
        _cxx_vector_index(lines, rel, funcs, out)
        _cxx_catch_all(lines, rel, funcs, out)
        _cxx_throw_new(lines, rel, funcs, out)
        _cxx_uninit_member(stripped, lines, rel, funcs, out)
        _cxx_copy_assign_ptr(lines, rel, funcs, out)
        _cxx_volatile_cast(lines, rel, funcs, out)
        _cxx_shared_get(lines, rel, funcs, out)
        _cxx_auto_ptr(lines, rel, funcs, out)
        _cxx_string_data(lines, rel, funcs, out)
        _cxx_enable_shared(lines, rel, funcs, out)
        _cxx_fwd_ref(lines, rel, funcs, out)
        _cxx_explicit_ctor(stripped, lines, rel, funcs, out)
        _cxx_move_const(lines, rel, funcs, out)
        _cxx_bind_tmp(stripped, lines, rel, funcs, out)
        _cxx_expected_null(lines, rel, funcs, out)
        _cxx_std_format(lines, rel, funcs, out)
        _cxx_this_capture(stripped, lines, rel, funcs, out)
        _cxx_spaceship_default(stripped, lines, rel, funcs, out)
        _cxx_std_async(lines, rel, funcs, out)
        _cxx_future_get(lines, rel, funcs, out)
        _cxx_function_null(lines, rel, funcs, out)
        _cxx_nodiscard(lines, rel, funcs, out)
        _cxx_std_jthread(lines, rel, funcs, out)
        _cxx_mdspan_dangle(lines, rel, funcs, out)
        _cxx_atomic_ref(lines, rel, funcs, out)
        _cxx_condition_wait(lines, rel, funcs, out)
        _cxx_std_bind(lines, rel, funcs, out)
        _cxx_assume(lines, rel, funcs, out)
        _cxx_generator(lines, rel, funcs, out)
        _cxx_shared_mutex(lines, rel, funcs, out)
        _cxx_any_cast(lines, rel, funcs, out)
        _cxx_filesystem(lines, rel, funcs, out)
        _cxx_regex(lines, rel, funcs, out)
        _cxx_latch(lines, rel, funcs, out)
        _cxx_from_chars(lines, rel, funcs, out)
        _cxx_init_list_dangle(lines, rel, funcs, out)
        _cxx_stop_token(lines, rel, funcs, out)
        _cxx_flat_map(lines, rel, funcs, out)
        _cxx_semaphore(lines, rel, funcs, out)
        _cxx_stacktrace(lines, rel, funcs, out)
        _cxx_unique_release(lines, rel, funcs, out)
        _cxx_pack_pragma(lines, rel, funcs, out)
        _cxx_function_ref(lines, rel, funcs, out)
        _cxx_move_only_function(lines, rel, funcs, out)
        _cxx_ranges_dangle(lines, rel, funcs, out)
        _cxx_chrono_seed(lines, rel, funcs, out)
        _cxx_inplace_vector(lines, rel, funcs, out)
        _cxx_flat_set(lines, rel, funcs, out)
        _cxx_copyable_function(lines, rel, funcs, out)
        _cxx_hive(lines, rel, funcs, out)
        _cxx_bitset_index(lines, rel, funcs, out)
        _cxx_sstream_view(lines, rel, funcs, out)
        _cxx_optional_value(lines, rel, funcs, out)
        _cxx_indirect(lines, rel, funcs, out)
        _cxx_to_chars(lines, rel, funcs, out)
        _cxx_hazard_pointer(lines, rel, funcs, out)
        _cxx_text_encoding(lines, rel, funcs, out)
        _cxx_expected_error(lines, rel, funcs, out)
        _cxx_variant_valueless(lines, rel, funcs, out)
        _cxx_simd_index(lines, rel, funcs, out)
        _cxx_rcu(lines, rel, funcs, out)
        _cxx_linalg(lines, rel, funcs, out)
        _cxx_sync_wait(lines, rel, funcs, out)
        _cxx_embed(lines, rel, funcs, out)
        _cxx_contracts(lines, rel, funcs, out)
        _cxx_reflection(lines, rel, funcs, out)
        _cxx_out_ptr(lines, rel, funcs, out)
        _cxx_flat_multimap(lines, rel, funcs, out)
        _cxx_spanstream(lines, rel, funcs, out)
        _cxx_barrier(lines, rel, funcs, out)
        _cxx_task(lines, rel, funcs, out)
        _cxx_generator_discard(lines, rel, funcs, out)
        _cxx_osyncstream(lines, rel, funcs, out)
        _cxx_packaged_task(lines, rel, funcs, out)
        _cxx_flat_multiset(lines, rel, funcs, out)
        _cxx_syncbuf(lines, rel, funcs, out)
        _cxx_counted_iterator(lines, rel, funcs, out)
        _cxx_promise(lines, rel, funcs, out)
        _cxx_weak_ptr(lines, rel, funcs, out)
        _cxx_exception_ptr(lines, rel, funcs, out)
        _cxx_coro_handle(lines, rel, funcs, out)
        _cxx_valarray(lines, rel, funcs, out)
        _cxx_to_underlying(lines, rel, funcs, out)
        _cxx_unexpected(lines, rel, funcs, out)
        _cxx_tuple_get(lines, rel, funcs, out)
        _cxx_deque_index(lines, rel, funcs, out)
        _cxx_forward_list(lines, rel, funcs, out)
        _cxx_list_front(lines, rel, funcs, out)
        _cxx_map_at(lines, rel, funcs, out)
        _cxx_unordered_at(lines, rel, funcs, out)
        _cxx_set_find(lines, rel, funcs, out)
        _cxx_queue_front(lines, rel, funcs, out)
        _cxx_stack_top(lines, rel, funcs, out)
        _cxx_priority_queue(lines, rel, funcs, out)
        _cxx_array_index(lines, rel, funcs, out)
        _cxx_unordered_set(lines, rel, funcs, out)
        _cxx_wstring_view(lines, rel, funcs, out)
        _cxx_multimap_find(lines, rel, funcs, out)
        _cxx_multiset_find(lines, rel, funcs, out)
        _cxx_binary_semaphore(lines, rel, funcs, out)
        _cxx_error_code(lines, rel, funcs, out)
        _cxx_byteswap(lines, rel, funcs, out)
        _cxx_pmr(lines, rel, funcs, out)
        _cxx_u8string_view(lines, rel, funcs, out)
        _cxx_unordered_multimap(lines, rel, funcs, out)
        _cxx_unordered_multiset(lines, rel, funcs, out)
        _cxx_shared_lock(lines, rel, funcs, out)
        _cxx_atomic_flag(lines, rel, funcs, out)
        _cxx_condvar_any(lines, rel, funcs, out)
        _cxx_recursive_mutex(lines, rel, funcs, out)
        _cxx_timed_mutex(lines, rel, funcs, out)
        _cxx_fstream(lines, rel, funcs, out)
        _cxx_this_thread(lines, rel, funcs, out)
        _cxx_call_once(lines, rel, funcs, out)
        _cxx_shared_timed_mutex(lines, rel, funcs, out)
        _cxx_recursive_timed_mutex(lines, rel, funcs, out)
        _cxx_system_error(lines, rel, funcs, out)
        _cxx_chrono_tzdb(lines, rel, funcs, out)
        _cxx_ranges_zip(lines, rel, funcs, out)
        _cxx_format_to(lines, rel, funcs, out)
        _cxx_error_category(lines, rel, funcs, out)
        _cxx_nested_exception(lines, rel, funcs, out)
        _cxx_atomic_fence(lines, rel, funcs, out)
        _cxx_notify_thread_exit(lines, rel, funcs, out)
        _cxx_wstring_convert(lines, rel, funcs, out)
        _cxx_invoke(lines, rel, funcs, out)
        _cxx_apply(lines, rel, funcs, out)
        _cxx_reference_wrapper(lines, rel, funcs, out)
        _cxx_endian(lines, rel, funcs, out)
        _cxx_bit_ceil(lines, rel, funcs, out)
        _cxx_uncaught_exceptions(lines, rel, funcs, out)
        _cxx_ranges_join(lines, rel, funcs, out)
        _cxx_quick_exit(lines, rel, funcs, out)
        _cxx_to_array(lines, rel, funcs, out)
        _cxx_zoned_time(lines, rel, funcs, out)
        _cxx_kill_dependency(lines, rel, funcs, out)
        _cxx_rotl(lines, rel, funcs, out)
        _cxx_current_exception(lines, rel, funcs, out)
        _cxx_bit_width(lines, rel, funcs, out)
        _cxx_lerp(lines, rel, funcs, out)
        _cxx_midpoint(lines, rel, funcs, out)
        _cxx_cmp_less(lines, rel, funcs, out)
        _cxx_countl_zero(lines, rel, funcs, out)
        _cxx_unreachable(lines, rel, funcs, out)
        _cxx_gcd(lines, rel, funcs, out)
        _cxx_lcm(lines, rel, funcs, out)
        _cxx_clamp(lines, rel, funcs, out)
        _cxx_exchange(lines, rel, funcs, out)
        _cxx_to_address(lines, rel, funcs, out)
        _cxx_is_constant_evaluated(lines, rel, funcs, out)
        _cxx_addressof(lines, rel, funcs, out)
        _cxx_assume_aligned(lines, rel, funcs, out)
        _cxx_as_const(lines, rel, funcs, out)
        _cxx_exclusive_scan(lines, rel, funcs, out)
        _cxx_make_exception_ptr(lines, rel, funcs, out)
        _cxx_set_terminate(lines, rel, funcs, out)
        _cxx_inclusive_scan(lines, rel, funcs, out)
        _cxx_transform_reduce(lines, rel, funcs, out)
        _cxx_reduce(lines, rel, funcs, out)
        _cxx_uninitialized_copy(lines, rel, funcs, out)
        _cxx_construct_at(lines, rel, funcs, out)
        _cxx_forward_like(lines, rel, funcs, out)
        _cxx_uninitialized_fill(lines, rel, funcs, out)
        _cxx_destroy_n(lines, rel, funcs, out)
        _cxx_add_sat(lines, rel, funcs, out)
        _cxx_transform_scan(lines, rel, funcs, out)
        _cxx_type_identity(lines, rel, funcs, out)
        _cxx_nontype(lines, rel, funcs, out)
        _cxx_layout_compatible(lines, rel, funcs, out)
        _cxx_ptr_interconvertible(lines, rel, funcs, out)
        _cxx_uninitialized_value(lines, rel, funcs, out)
        _cxx_const_iterator(lines, rel, funcs, out)
        _cxx_corresponding_member(lines, rel, funcs, out)
        _cxx_ranges_to(lines, rel, funcs, out)
        _cxx_enumerate(lines, rel, funcs, out)
        _cxx_cartesian_product(lines, rel, funcs, out)
        _cxx_chunk(lines, rel, funcs, out)
        _cxx_slide(lines, rel, funcs, out)
        _cxx_adjacent(lines, rel, funcs, out)
        _cxx_join_with(lines, rel, funcs, out)
        _cxx_zip_transform(lines, rel, funcs, out)
        _cxx_as_rvalue(lines, rel, funcs, out)
        _cxx_from_range(lines, rel, funcs, out)
        _cxx_scoped_enum(lines, rel, funcs, out)
        _cxx_stride(lines, rel, funcs, out)
        _cxx_repeat(lines, rel, funcs, out)
        _cxx_take(lines, rel, funcs, out)
        _cxx_drop(lines, rel, funcs, out)
        _cxx_filter(lines, rel, funcs, out)
        _cxx_transform_view(lines, rel, funcs, out)
        _cxx_elements(lines, rel, funcs, out)
        _cxx_iota(lines, rel, funcs, out)
        _cxx_take_while(lines, rel, funcs, out)
        _cxx_drop_while(lines, rel, funcs, out)
        _cxx_keys(lines, rel, funcs, out)
        _cxx_values(lines, rel, funcs, out)
        _cxx_reverse_view(lines, rel, funcs, out)
        _cxx_counted(lines, rel, funcs, out)
    return out


def _cap_norm(s: str) -> str:
    return _rx(r"\s+").sub("", s)


def _norm_var(s: str) -> str:
    return _rx(r"\s+").sub("", s)


def _has_assignment(var: str, line: str) -> bool:
    v = re.escape(var)
    return bool(re.search(rf"\b{v}\s*=(?!=)", line))


def _reassigns_var(var: str, line: str) -> bool:
    """True when `var` is assigned a new value (NULL does not count)."""
    v = re.escape(var)
    m = re.search(rf"\b{v}\s*=(?!=)\s*(.+?)\s*(?:;|,|$)", line)
    if not m:
        return False
    rhs = m.group(1).strip()
    if _rx(r"^(?:NULL|nullptr|0)\b").match(rhs):
        return False
    return True


def _reads_var(var: str, line: str) -> bool:
    if _has_assignment(var, line):
        return False
    v = re.escape(var)
    if re.search(rf"\b{v}\s*->", line):
        return True
    if re.search(rf"\*\s*{v}\b", line):
        return True
    if re.search(rf"\b{v}\s*\[", line):
        return True
    return bool(re.search(rf"\b{v}\b", line))


def _split_call_args(inner: str) -> list[str]:
    args: list[str] = []
    depth = 0
    cur: list[str] = []
    in_str = False
    esc = False
    for ch in inner:
        if in_str:
            cur.append(ch)
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
            continue
        if ch == '"':
            in_str = True
            cur.append(ch)
        elif ch == "(":
            depth += 1
            cur.append(ch)
        elif ch == ")":
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    tail = "".join(cur).strip()
    if tail:
        args.append(tail)
    return args


def _required_literal(pat: re.Pattern[str]) -> str | None:
    """Longest literal every match of `pat` must contain, or None.

    Walks the parsed pattern: literal runs in the mandatory top-level
    sequence (through plain groups, and through repeats with min >= 1)
    are required. Anything unsure (case folding, branches, classes)
    ends a run. None means "no prefilter", which is always safe.
    """
    if pat.flags & re.IGNORECASE:
        return None
    try:
        from re import _constants as sre_c  # type: ignore[attr-defined]
        from re import _parser as sre_p  # type: ignore[attr-defined]
        parsed = sre_p.parse(pat.pattern, pat.flags)
    except Exception:
        return None
    runs: list[str] = []

    def walk(seq, run: list[str]) -> None:
        for op, av in seq:
            if op is sre_c.LITERAL:
                run.append(chr(av))
            elif op is sre_c.AT:
                continue  # zero width: the run stays contiguous
            elif op is sre_c.SUBPATTERN and not av[1] and not av[2]:
                walk(av[3], run)
            else:
                runs.append("".join(run))
                run.clear()
                if op in (sre_c.MAX_REPEAT, sre_c.MIN_REPEAT) and av[0] >= 1:
                    inner: list[str] = []
                    walk(av[2], inner)
                    runs.append("".join(inner))

    tail: list[str] = []
    walk(parsed, tail)
    runs.append("".join(tail))
    best = max(runs, key=len)
    return best if len(best) >= 2 else None


_REQ_LIT: dict[re.Pattern[str], str | None] = {}


def _pattern_literal(pat: re.Pattern[str]) -> str | None:
    try:
        return _REQ_LIT[pat]
    except KeyError:
        lit = _REQ_LIT[pat] = _required_literal(pat)
        return lit


def _lit_search(pat: re.Pattern[str], text: str) -> re.Match[str] | None:
    """pat.search(text), skipped when text lacks the literal it needs."""
    lit = _pattern_literal(pat)
    if lit is not None and lit not in text:
        return None
    return pat.search(text)


def _lit_finditer(pat: re.Pattern[str], text: str) -> Iterator[re.Match[str]]:
    """pat.finditer(text), empty when text lacks the literal it needs."""
    lit = _pattern_literal(pat)
    if lit is not None and lit not in text:
        return iter(())
    return pat.finditer(text)


def _gated_lines(body: str, pat: re.Pattern[str]) -> list[str]:
    """body.splitlines(), or [] when no line can match `pat`.

    For per-line loops whose first step is `if not pat.match(ln): continue`:
    a literal every match needs, absent from the whole body, is absent
    from every line, so the loop would do nothing.
    """
    lit = _pattern_literal(pat)
    if lit is not None and lit not in body:
        return []
    return body.splitlines()


_RX: dict[str, re.Pattern[str]] = {}
_RX_FLAGS: dict[tuple[str, int], re.Pattern[str]] = {}


def _rx(pattern: str, flags: int = 0) -> re.Pattern[str]:
    """re.compile for the inline constant patterns, cached for good.

    The checkers use far more distinct patterns than the re module's
    512-entry cache holds, so `re.search(pattern, s)` kept recompiling.
    """
    if not flags:
        pat = _RX.get(pattern)
        if pat is None:
            pat = _RX[pattern] = re.compile(pattern)
        return pat
    key = (pattern, int(flags))
    pat = _RX_FLAGS.get(key)
    if pat is None:
        pat = _RX_FLAGS[key] = re.compile(pattern, flags)
    return pat


_CALL_OPEN_RE: dict[str, re.Pattern[str]] = {}
_PLAIN_IDENT = re.compile(r"\w+")
_PLAIN_NAME: dict[str, bool] = {}


def _call_open_re(fn: str) -> re.Pattern[str]:
    pat = _CALL_OPEN_RE.get(fn)
    if pat is None:
        pat = re.compile(rf"\b{fn}\s*\(")
        _CALL_OPEN_RE[fn] = pat
    return pat


def _find_call_args(line: str, fn: str) -> list[str] | None:
    # A plain identifier must occur literally; skip the regex when absent.
    if fn not in line:
        plain = _PLAIN_NAME.get(fn)
        if plain is None:
            plain = _PLAIN_NAME[fn] = _PLAIN_IDENT.fullmatch(fn) is not None
        if plain:
            return None
    m = _call_open_re(fn).search(line)
    if not m:
        return None
    start = m.end()
    depth = 1
    i = start
    while i < len(line) and depth > 0:
        if line[i] == "(":
            depth += 1
        elif line[i] == ")":
            depth -= 1
        i += 1
    if depth != 0:
        return None
    return _split_call_args(line[start:i - 1])


def _is_string_literal(s: str) -> bool:
    return bool(_rx(r'^L?"').match(s.strip()))


def _string_literal_inner(s: str) -> str | None:
    s = s.strip()
    if s.startswith('L"') and s.endswith('"') and len(s) >= 3:
        return s[2:-1]
    if s.startswith('"') and s.endswith('"') and len(s) >= 2:
        return s[1:-1]
    return None


def _literal_has_percent_n(s: str) -> bool:
    """True when a string literal format contains %n (not %%n)."""
    inner = _string_literal_inner(s)
    if inner is None:
        return False
    i = 0
    while i < len(inner):
        if inner[i] == "\\":
            i += 2
            continue
        if inner[i] != "%":
            i += 1
            continue
        if i + 1 < len(inner) and inner[i + 1] == "%":
            i += 2
            continue
        j = i + 1
        while j < len(inner) and inner[j] in "-+ #0":
            j += 1
        if j < len(inner) and inner[j] == "*":
            j += 1
        else:
            while j < len(inner) and inner[j].isdigit():
                j += 1
        if j < len(inner) and inner[j] == ".":
            j += 1
            if j < len(inner) and inner[j] == "*":
                j += 1
            else:
                while j < len(inner) and inner[j].isdigit():
                    j += 1
        while j < len(inner) and inner[j] in "hlLjztz":
            j += 1
        if j < len(inner) and inner[j] == "n":
            return True
        i = j + 1 if j < len(inner) else j
    return False


# --- block-structured reachability -------------------------------------
#
# Not a CFG. A braced block whose last statement leaves (return, goto,
# break, continue, throw, exit(), abort(), ...) does not fall through, so
# what it did (a free, an unlock) does not reach the code after it. A
# block that leaves by break/continue rejoins after the loop or switch
# it leaves. Checkers replay this over their per-line walk.

_EXIT_STMT = re.compile(
    r"(?:return|goto|break|continue|throw)\b"
    r"|(?:exit|_exit|_Exit|quick_exit|abort|err|errx|verr|verrx"
    r"|longjmp|siglongjmp|__builtin_trap|__builtin_unreachable)\s*\("
    r"|std\s*::\s*(?:exit|abort|terminate|quick_exit)\s*\("
)
_STMT_LABELS = re.compile(
    r"(?:(?:case\b[^:;]*|default|[A-Za-z_]\w*)\s*:(?!:)\s*)+"
)
_LOOP_HEAD = re.compile(r"(?:for|while|switch|do)\b")
_CHAR_LITERAL = re.compile(r"'(?:\\.|[^'\\\n])*'")
_BLOCK_TOKEN = re.compile(r"[{};()\n]")


class ExitBlock(NamedTuple):
    """A braced block that does not fall through. Lines are 0-based."""

    open_line: int
    close_line: int
    close_first: bool  # `}` is the first non-blank character of close_line
    loop_exit: bool  # left by break/continue
    merge_line: int  # close line of the loop/switch it leaves, else -1


def _strip_labels(stmt: str) -> str:
    m = _STMT_LABELS.match(stmt)
    return stmt[m.end():] if m else stmt


def exit_blocks(lines: list[str]) -> list[ExitBlock]:
    """Blocks in `lines` (comment- and string-blanked) that end by leaving.

    Listed in closing order, so inner blocks come before outer ones.
    """
    text = "\n".join(lines)
    if "{" not in text:
        return []
    if "'" in text:
        text = _CHAR_LITERAL.sub(lambda m: " " * len(m.group()), text)
    out: list[ExitBlock] = []
    # frame: [open_line, is_loop, last statement, saved paren depth, waiting]
    stack: list[list[Any]] = []
    line = 0
    line_start = 0
    paren = 0
    stmt_start = 0
    for m in _BLOCK_TOKEN.finditer(text):
        c = m.group()
        pos = m.start()
        if c == "\n":
            line += 1
            line_start = pos + 1
        elif c == "(":
            paren += 1
        elif c == ")":
            if paren:
                paren -= 1
        elif c == ";":
            if paren == 0:
                stmt = text[stmt_start:pos]
                if stack and stmt.strip():
                    stack[-1][2] = stmt
                stmt_start = pos + 1
        elif c == "{":
            head = _strip_labels(text[stmt_start:pos].strip())
            if head.startswith("else"):
                head = head[4:].lstrip()
            stack.append([line, bool(_LOOP_HEAD.match(head)), None, paren, []])
            paren = 0
            stmt_start = pos + 1
        else:
            if not stack:
                stmt_start = pos + 1
                continue
            tail = text[stmt_start:pos]
            frame = stack.pop()
            last = tail if tail.strip() else frame[2]
            paren = frame[3]
            stmt_start = pos + 1
            for k in frame[4]:
                out[k] = out[k]._replace(merge_line=line)
            if stack:
                stack[-1][2] = ""  # the parent's last statement is a block
            if not last:
                continue
            em = _EXIT_STMT.match(_strip_labels(last.strip()))
            if not em:
                continue
            loop_exit = em.group().startswith(("break", "continue"))
            if loop_exit:
                if frame[1]:
                    continue  # break/continue of this very loop or switch
                target = next((f for f in reversed(stack) if f[1]), None)
                if target is None:
                    loop_exit = False
                else:
                    target[4].append(len(out))
            out.append(ExitBlock(
                frame[0], line, not text[line_start:pos].strip(), loop_exit, -1,
            ))
    return out


class _ExitReplay:
    """Replays exit_blocks over a line walk: call before(i), then after(i).

    A block's effects are undone when it closes; a break/continue block's
    effects are merged back in after the loop or switch it leaves.
    """

    def __init__(
        self,
        blocks: list[ExitBlock],
        snap: Callable[[], Any],
        restore: Callable[[Any], None],
        merge: Callable[[Any], None],
    ) -> None:
        self.blocks = blocks
        self.snap = snap
        self.restore = restore
        self.merge = merge
        self.opens: dict[int, list[int]] = {}
        self.close_first: dict[int, list[int]] = {}
        self.close_after: dict[int, list[int]] = {}
        for k, b in enumerate(blocks):
            self.opens.setdefault(b.open_line, []).append(k)
            where = self.close_first if b.close_first else self.close_after
            where.setdefault(b.close_line, []).append(k)
        self.saved: dict[int, Any] = {}
        self.merges: dict[int, list[Any]] = {}

    def _leave(self, k: int) -> None:
        state = self.saved.pop(k, None)
        if state is None:
            return
        b = self.blocks[k]
        if b.loop_exit and b.merge_line >= 0:
            self.merges.setdefault(b.merge_line, []).append(self.snap())
        self.restore(state)

    def before(self, i: int) -> None:
        for k in self.close_first.get(i, ()):
            self._leave(k)
        for k in self.opens.get(i, ()):
            self.saved[k] = self.snap()

    def after(self, i: int) -> None:
        for k in self.close_after.get(i, ()):
            self._leave(k)
        for state in self.merges.pop(i, ()):
            self.merge(state)


def _frees_only_in_call(var: str, ln: str, fm: re.Match[str] | None) -> bool:
    """True when every read of `var` on `ln` is the free() call itself."""
    if fm is None or _norm_var(fm.group("var")) != var:
        return False
    return not _reads_var(var, ln[: fm.start()] + ln[fm.end():])


_FreedState = tuple[dict[str, int], dict[str, int], set[str]]


def _mem_lifetime(lines, rel, funcs, out) -> None:
    """Use-after-free and double-free within a function body.

    A free inside a block that leaves (see exit_blocks) does not reach
    the code after the block. A second free is MEM-DOUBLE-FREE only, not
    also MEM-UAF on the same line.
    """
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        freed_uaf: dict[str, int] = {}
        freed_df: dict[str, int] = {}
        reported_uaf: set[str] = set()
        reported_df: set[tuple[str, int]] = set()

        def snap() -> _FreedState:
            return dict(freed_uaf), dict(freed_df), set(reported_uaf)

        def restore(st: _FreedState) -> None:
            freed_uaf.clear()
            freed_uaf.update(st[0])
            freed_df.clear()
            freed_df.update(st[1])
            reported_uaf.clear()
            reported_uaf.update(st[2])

        def merge(st: _FreedState) -> None:
            for k, v in st[0].items():
                freed_uaf.setdefault(k, v)
            for k, v in st[1].items():
                freed_df.setdefault(k, v)
            reported_uaf.update(st[2])

        replay: _ExitReplay | None = None
        if any("free" in ln for ln in chunk):
            blocks = exit_blocks(chunk)
            if blocks:
                replay = _ExitReplay(blocks, snap, restore, merge)
        for i, ln in enumerate(chunk):
            if replay is not None:
                replay.before(i)
            for var in list(freed_uaf):
                if _reassigns_var(var, ln):
                    freed_uaf.pop(var, None)
                    freed_df.pop(var, None)
                    reported_uaf.discard(var)
                elif _has_assignment(var, ln):
                    freed_df.pop(var, None)
            fm = FREE_CALL.search(ln) if "free" in ln else None
            for var in list(freed_uaf):
                if var in reported_uaf:
                    continue
                if _reads_var(var, ln) and not _frees_only_in_call(var, ln, fm):
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="MEM-UAF",
                        message=f"{var} used after free without reassignment",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else "",
                    ))
                    reported_uaf.add(var)
            if fm:
                var = _norm_var(fm.group("var"))
                if var in freed_df:
                    key = (var, freed_df[var])
                    if key not in reported_df:
                        line = start + i
                        out.append(Finding(
                            stage="lints", status=laws.FAILED, file=rel,
                            function=fn.name, line=line, cls="MEM-DOUBLE-FREE",
                            message=f"{var} freed again without reassignment",
                            strength=laws.STRENGTH_FINDS,
                            evidence=lines[line - 1].strip()
                            if 0 < line <= len(lines) else "",
                        ))
                        reported_df.add(key)
                freed_uaf[var] = i
                freed_df[var] = i
            if replay is not None:
                replay.after(i)


def _is_zero_literal(s: str) -> bool:
    return bool(_rx(r"^0(?:u|l|ll|ul|ull)?$", re.I).match(s.strip()))


def _is_tiny_literal(s: str) -> bool:
    return bool(_rx(r"^(?:0|1)(?:u|l|ll|ul|ull)?$", re.I).match(s.strip()))


def _is_sizeof_expr(s: str) -> bool:
    return bool(_rx(r"^sizeof\s*\(").match(s.strip()))


def _has_wrap_mul(s: str) -> bool:
    """`n * sizeof(...)` or `sizeof(...) * n`, casts on n do not help."""
    t = s.strip()
    _var = r"[A-Za-z_]\w*"
    return bool(
        re.search(
            rf"(?:\([^)]*\)\s*)*{_var}\s*\*\s*sizeof\s*\(",
            t,
        )
        or re.search(
            rf"sizeof\s*\([^)]+\)\s*\*\s*(?:\([^)]*\)\s*)*{_var}",
            t,
        )
    )


def _memset_swap(lines, rel, funcs, out) -> None:
    """memset with fill byte 0 in the third argument and size in the second."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[int] = set()
        for i, ln in enumerate(chunk):
            args = _find_call_args(ln, "memset")
            if args is None or len(args) < 3:
                continue
            if not _is_zero_literal(args[2]):
                continue
            size_arg = args[1].strip()
            if not (_is_sizeof_expr(size_arg) or (
                _rx(r"^[A-Za-z_]\w*$").match(size_arg) and not _is_tiny_literal(size_arg)
            )):
                continue
            if i in seen:
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="MEM-MEMSET-SWAP",
                message="memset() has size and fill-byte arguments swapped",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))


_LOWER_BOUND = re.compile(
    r"\b([A-Za-z_]\w*)\s*(?:>=\s*0|>\s*-\s*1)\b"
)
_UPPER_BOUND = re.compile(
    r"\b([A-Za-z_]\w*)\s*(?:<|<=)\s*[^|&,)]+"
)
_ALWAYS_FALSE_LOW = re.compile(r"\b([A-Za-z_]\w*)\s*<\s*0\b")
_ALWAYS_FALSE_HIGH = re.compile(r"\b([A-Za-z_]\w*)\s*>\s*[^|&,)]+")


def _taut_bound(lines, rel, funcs, out) -> None:
    """Signed index bounds joined by || (or contradictory &&) instead of &&."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _rx(r"\bif\s*\(([^)]+)\)").search(ln)
            if not m:
                continue
            cond = m.group(1)
            cls = None
            msg = None
            var = None
            if "||" in cond:
                lowers = {x.group(1) for x in _LOWER_BOUND.finditer(cond)}
                uppers = {x.group(1) for x in _UPPER_BOUND.finditer(cond)}
                both = lowers & uppers
                if both:
                    var = sorted(both)[0]
                    cls = "INT-TAUTOLOGY"
                    msg = f"{var} lower/upper bounds use ||; nearly always true"
            elif "&&" in cond:
                lows = {x.group(1) for x in _ALWAYS_FALSE_LOW.finditer(cond)}
                highs = {x.group(1) for x in _ALWAYS_FALSE_HIGH.finditer(cond)}
                both = lows & highs
                if both:
                    var = sorted(both)[0]
                    cls = "INT-TAUTOLOGY"
                    msg = f"{var} contradictory bounds use &&; always false"
            if cls is None or var is None or msg is None:
                continue
            key = (var, i)
            if key in seen:
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls=cls,
                message=msg,
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))


_NARROW_CAST = re.compile(
    r"\((?:unsigned\s+)?(?:char|short)\)\s*([A-Za-z_]\w*)\b"
)
_NARROW_DECL_INIT = re.compile(
    r"\b(?:signed\s+)?(?:char|short)\s+([A-Za-z_]\w*)\s*=\s*(?P<rhs>[^;,]+)"
)
_NARROW_DECL = re.compile(
    r"\b(?:signed\s+)?(?:char|short)\s+([A-Za-z_]\w*)\s*;"
)
_SIGN_CONV = re.compile(
    r"\b(?P<a>[A-Za-z_]\w*)\s*(?P<op><|>|<=|>=)\s*(?P<b>[A-Za-z_]\w*)\b"
)
_NEW_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*new\s+(?P<rest>[^;]+)"
)
_DELETE = re.compile(r"\bdelete\s*(\[\s*\])?\s*(?P<var>[A-Za-z_]\w*)\b")
_C_ALLOC_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?"
    r"(?:malloc|calloc|realloc)\s*\("
)


def _parse_int_literal(s: str) -> int | None:
    s = s.strip()
    m = _rx(r"^(?P<n>-?(?:0x[0-9a-fA-F]+|0[0-7]*|\d+))"
        r"(?:[uUlL]{0,3})?$").match(s)
    if not m:
        return None
    n = m.group("n")
    try:
        if n.lower().startswith("0x"):
            return int(n, 16)
        if n.startswith("0") and len(n) > 1 and n[1].isdigit():
            return int(n, 8)
        return int(n, 10)
    except ValueError:
        return None


def _is_char_literal(s: str) -> bool:
    return bool(_rx(r"^'(?:\\.|[^\\'])'$").match(s.strip()))


def _trunc_rhs_bad(rhs: str, narrow: str) -> bool:
    rhs = rhs.strip()
    if _is_char_literal(rhs):
        return False
    if _rx(r"^[A-Za-z_]\w*$").match(rhs):
        return rhs not in _KW
    val = _parse_int_literal(rhs)
    if val is None:
        return False
    if narrow == "char":
        return not (-128 <= val <= 127)
    return not (-32768 <= val <= 32767)


def _narrow_kind(typ: str) -> str | None:
    if _UNSIGNED_TY.search(typ):
        return None
    if _rx(r"\bchar\b").search(typ):
        return "char"
    if _rx(r"\bshort\b").search(typ):
        return "short"
    return None


def _narrow_names(fn) -> dict[str, str]:
    names: dict[str, str] = {}
    for typ, name in fn.params:
        if not name or "*" in typ or "[" in typ:
            continue
        kind = _narrow_kind(typ)
        if kind:
            names[name] = kind
    for m in _lit_finditer(_NARROW_DECL, fn.body):
        kind = "char" if _rx(r"\bchar\b").search(m.group(0)) else "short"
        names[m.group(1)] = kind
    for m in _lit_finditer(_NARROW_DECL_INIT, fn.body):
        kind = "char" if _rx(r"\bchar\b").search(m.group(0)) else "short"
        names[m.group(1)] = kind
    return names


def _unsigned_names(fn) -> set[str]:
    names: set[str] = set()
    for typ, name in fn.params:
        if not name or "*" in typ or "[" in typ:
            continue
        if _UNSIGNED_TY.search(typ):
            names.add(name)
    for m in _rx(r"\b(?:unsigned\s+(?:int|short|long|char)|size_t|uint\d+_t)\s+"
        r"([A-Za-z_]\w*)\s*;").finditer(fn.body):
        names.add(m.group(1))
    return names


def _int_trunc(lines, rel, funcs, out) -> None:
    """Wider integer assigned or cast into char/short without a fitting literal."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        narrow = _narrow_names(fn)
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            for m in _NARROW_DECL_INIT.finditer(ln):
                name = m.group(1)
                rhs = m.group("rhs").strip()
                narrow_ty = narrow.get(name) or (
                    "char" if _rx(r"\bchar\b").search(m.group(0)) else "short"
                )
                if not _trunc_rhs_bad(rhs, narrow_ty):
                    continue
                key = (name, i)
                if key in seen:
                    continue
                seen.add(key)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="INT-TRUNC",
                    message=f"{name} narrowed from a wider value",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))
            for m in _NARROW_CAST.finditer(ln):
                ident = m.group(1)
                if ident in _KW:
                    continue
                key = (ident, i)
                if key in seen:
                    continue
                seen.add(key)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="INT-TRUNC",
                    message=f"({ident}) narrowed via cast from a wider value",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))
            for name, narrow_ty in narrow.items():
                am = re.search(
                    rf"\b{re.escape(name)}\s*=(?!=)\s*(?P<rhs>[^;,]+)",
                    ln,
                )
                if not am or _NARROW_DECL_INIT.search(ln):
                    continue
                rhs = am.group("rhs").strip()
                if not _trunc_rhs_bad(rhs, narrow_ty):
                    continue
                key = (name, i)
                if key in seen:
                    continue
                seen.add(key)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="INT-TRUNC",
                    message=f"{name} narrowed from a wider value",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))


def _int_sign_conv(lines, rel, funcs, out) -> None:
    """Signed index compared to an unsigned bound (or the reverse)."""
    for fn in funcs:
        signed = _signed_index_names(fn)
        unsigned = _unsigned_names(fn)
        if not signed or not unsigned:
            continue
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            for m in _SIGN_CONV.finditer(ln):
                a, b = m.group("a"), m.group("b")
                pair = None
                if a in signed and b in unsigned:
                    pair = (a, b)
                elif b in signed and a in unsigned:
                    pair = (b, a)
                if pair is None:
                    continue
                key = (pair[0], i)
                if key in seen:
                    continue
                seen.add(key)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="INT-SIGN-CONV",
                    message=f"{pair[0]} compared to unsigned {pair[1]}; "
                    f"negative indices look huge",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))


_LOCAL_DECL = re.compile(
    r"^\s*(?P<static>static\s+)?"
    r"(?:const\s+|volatile\s+)?"
    r"(?:struct\s+\w+|union\s+\w+|enum\s+\w+|"
    r"(?:unsigned\s+)?(?:char|short|int|long|float|double)|"
    r"size_t|ssize_t|ptrdiff_t|int\d+_t|uint\d+_t|wchar_t)\s+"
    r"(?P<name>[A-Za-z_]\w*)"
    r"(?P<array>\s*\[[^\]]+\])?\s*;",
    re.M,
)
_RETURN_ADDR = re.compile(r"\breturn\s*\(*\s*&\s*\(*\s*([A-Za-z_]\w*)\s*\)*\s*;")
_RETURN_VAR = re.compile(r"\breturn\s+\(?\s*(?!\*&)([A-Za-z_]\w*)\s*\)?\s*;")
_LOCAL_CHAR_ARRAY = re.compile(
    r"^\s*(?P<static>static\s+)?"
    r"(?:const\s+)?char\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*\[\s*(?P<size>\d+)\s*\]\s*;",
    re.M,
)
_UNBOUNDED_COPY_FN: dict[str, int] = {
    "strcpy": 0, "gets": 0, "strcat": 0,
}
_LOCAL_WCHAR_ARRAY = re.compile(
    r"^\s*(?P<static>static\s+)?"
    r"(?:const\s+)?wchar_t\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*\[\s*(?P<size>\d+)\s*\]\s*;",
    re.M,
)
_WCS_UNBOUNDED_FN: dict[str, int] = {
    "wcscpy": 0, "wcscat": 0,
}
_SPRINTF_FN: dict[str, int] = {
    "sprintf": 0, "vsprintf": 0,
}


def _locals_in_fn(fn) -> tuple[set[str], set[str]]:
    """Local scalars and array names declared in the function body (not params)."""
    params = {name for typ, name in fn.params if name}
    scalars: set[str] = set()
    arrays: set[str] = set()
    for m in _lit_finditer(_LOCAL_DECL, fn.body):
        if m.group("static"):
            continue
        name = m.group("name")
        if name in params or name in _KW:
            continue
        if m.group("array"):
            arrays.add(name)
        else:
            scalars.add(name)
    return scalars, arrays


def _stack_escape(lines, rel, funcs, out) -> None:
    """Return of address-of-local or local array (decays to dangling pointer)."""
    for fn in funcs:
        scalars, arrays = _locals_in_fn(fn)
        if not scalars and not arrays:
            continue
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _RETURN_ADDR.search(ln)
            if m:
                var = m.group(1)
                if var in scalars:
                    key = (var, i)
                    if key in seen:
                        continue
                    seen.add(key)
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="MEM-STACK-ESCAPE",
                        message=f"return &{var}; escapes address of local {var}",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else "",
                    ))
            m = _RETURN_VAR.search(ln)
            if m:
                var = m.group(1)
                if var in arrays:
                    key = (var, i)
                    if key in seen:
                        continue
                    seen.add(key)
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="MEM-STACK-ESCAPE",
                        message=f"return {var}; local array decays to dangling pointer",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else "",
                    ))


def _local_char_arrays(fn) -> set[str]:
    params = {name for typ, name in fn.params if name}
    names: set[str] = set()
    for m in _lit_finditer(_LOCAL_CHAR_ARRAY, fn.body):
        if m.group("static"):
            continue
        name = m.group("name")
        if name not in params and name not in _KW:
            names.add(name)
    return names


def _local_wchar_arrays(fn) -> set[str]:
    params = {name for typ, name in fn.params if name}
    names: set[str] = set()
    for m in _lit_finditer(_LOCAL_WCHAR_ARRAY, fn.body):
        if m.group("static"):
            continue
        name = m.group("name")
        if name not in params and name not in _KW:
            names.add(name)
    return names


def _str_sprintf(lines, rel, funcs, out) -> None:
    """sprintf/vsprintf into a fixed-size local char buffer (not snprintf)."""
    for fn in funcs:
        locals_arr = _local_char_arrays(fn)
        if not locals_arr:
            continue
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            for fname, dst_idx in _SPRINTF_FN.items():
                args = _find_call_args(ln, fname)
                if args is None or len(args) <= dst_idx:
                    continue
                dst = _rx(r"\s+").sub("", args[dst_idx])
                if dst not in locals_arr:
                    continue
                key = (fname, i)
                if key in seen:
                    continue
                seen.add(key)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="STR-SPRINTF",
                    message=f"{fname}() into local {dst}[…] with no bound",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))


def _unbounded_copy(lines, rel, funcs, out) -> None:
    """strcpy/gets/strcat into a fixed-size local char buffer."""
    for fn in funcs:
        locals_arr = _local_char_arrays(fn)
        if not locals_arr:
            continue
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            for fname, dst_idx in _UNBOUNDED_COPY_FN.items():
                args = _find_call_args(ln, fname)
                if args is None or len(args) <= dst_idx:
                    continue
                dst = _rx(r"\s+").sub("", args[dst_idx])
                if dst not in locals_arr:
                    continue
                key = (fname, i)
                if key in seen:
                    continue
                seen.add(key)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="STR-UNBOUNDED-COPY",
                    message=f"{fname}() into local {dst}[…] with no bound",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))


def _cxx_new_delete(lines, rel, funcs, out) -> None:
    """delete vs delete[] mismatch for the same pointer variable."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        new_kind: dict[str, bool] = {}
        for i, ln in enumerate(chunk):
            m = _NEW_ASSIGN.search(ln)
            if not m:
                continue
            rest = m.group("rest")
            is_array = "[" in rest.split("(", 1)[0]
            new_kind[m.group("var")] = is_array
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _DELETE.search(ln)
            if not m:
                continue
            var = m.group("var")
            if var not in new_kind:
                continue
            is_array = new_kind[var]
            del_array = m.group(1) is not None
            if is_array == del_array:
                continue
            key = (var, i)
            if key in seen:
                continue
            seen.add(key)
            line = start + i
            want = "delete[]" if is_array else "delete"
            got = "delete[]" if del_array else "delete"
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="MEM-NEW-DELETE",
                message=f"{var} allocated with new{'[]' if is_array else ''} "
                f"but freed with {got}; expected {want}",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))


_UNARY_BITOP_PREV = set("=(,?:;{[|&!~^+-*/%<>")


# Same left-to-right scan as a character loop: == != <= >= win, << and >>
# are stepped over whole, then a lone < or > is a comparison.
_CMP_TOKEN = re.compile(r"==|!=|<=|>=|<<|>>|[<>]")


def _has_comparison(s: str) -> bool:
    """True for == != <= >= < > that are not << or >>."""
    for m in _CMP_TOKEN.finditer(s):
        if m.group() not in ("<<", ">>"):
            return True
    return False


def _binary_bitop_indices(s: str) -> list[int]:
    """Indices of binary & / | (not && || &= |=, and not unary &)."""
    out: list[int] = []
    i = 0
    n = len(s)
    while i < n:
        ch = s[i]
        if ch in "&|":
            nxt = s[i + 1] if i + 1 < n else ""
            if nxt == ch or nxt == "=":
                i += 2
                continue
            j = i - 1
            while j >= 0 and s[j] in " \t":
                j -= 1
            if j < 0 or s[j] in _UNARY_BITOP_PREV:
                i += 1
                continue
            out.append(i)
        i += 1
    return out


def _bitop_on_comparison(s: str) -> bool:
    """An operand of a binary & / | on the line is itself a comparison at
    that operand's nesting level: `a == 1 & b == 2` (comparisons bind
    tighter), not `(a->type & 0xFF) != (b->type & 0xFF)` (the & is inside the
    parentheses; `->` is not a comparison)."""
    n = len(s)

    def at(i: int) -> str:
        return s[i] if 0 <= i < n else "\0"

    def is_assign(i: int) -> bool:
        return at(i) == "=" and at(i + 1) != "=" and at(i - 1) not in "=!<>+-*/%&|^"

    def has_cmp(a: int, b: int) -> bool:
        while a < b and at(a).isspace():
            a += 1
        while b > a and at(b - 1).isspace():
            b -= 1
        if a < b and at(a) == "(" and at(b - 1) == ")":
            # `(a < b) | (c < d)`: an operand that is one parenthesised comparison
            d, close = 0, -1
            for i in range(a, b):
                if at(i) == "(":
                    d += 1
                elif at(i) == ")":
                    d -= 1
                    if d == 0:
                        close = i
                        break
            if close == b - 1:
                return has_cmp(a + 1, b - 1)
        depth = 0
        for i in range(a, b):
            c = at(i)
            if c in "([":
                depth += 1
            elif c in ")]":
                depth -= 1
            if depth != 0:
                continue
            if c in "=!" and at(i + 1) == "=":
                return True
            if c in "<>":
                if at(i + 1) == c or at(i - 1) == c:
                    continue
                if c == ">" and at(i - 1) == "-":
                    continue
                return True
        return False

    for k in _binary_bitop_indices(s):
        depth, j = 0, k - 1
        while j >= 0:
            c = s[j]
            if c in ")]":
                depth += 1
            elif c in "([":
                if depth == 0:
                    break
                depth -= 1
            elif depth == 0 and (c in ",;?:{}&|" or is_assign(j)):
                break
            j -= 1
        if has_cmp(j + 1, k):
            return True
        depth, e = 0, k + 1
        while e < n:
            c = s[e]
            if c in "([":
                depth += 1
            elif c in ")]":
                if depth == 0:
                    break
                depth -= 1
            elif depth == 0 and (c in ",;?:{}&|" or is_assign(e)):
                break
            e += 1
        if has_cmp(k + 1, e):
            return True
    return False


def _bool_as_bit(lines, rel, funcs, out) -> None:
    """Bitwise &/| with a comparison operand: a boolean used as a bit.

    `flags & MASK` is not this. `a == 1 & b == 2` is. Unary `&` is not.
    """
    for fn in funcs:
        # No & or | in the body: no binary bit-op on any line.
        if "&" not in fn.body and "|" not in fn.body:
            continue
        start = fn.span[0]
        seen: set[int] = set()
        # `case '&': if (s->next++[0] == '&')` (tinyexpr): a char literal is no operator.
        body = _CHAR_LITERAL.sub(lambda m: " " * len(m.group()), fn.body)
        for i, ln in enumerate(body.splitlines()):
            if "&" not in ln and "|" not in ln:
                continue
            if not _has_comparison(ln):
                continue
            if not _bitop_on_comparison(ln):
                continue
            if i in seen:
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="INT-BOOL-AS-BIT",
                message="bitwise &/| applied to a comparison (boolean used as a bit)",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _wrap_alloc(lines, rel, funcs, out) -> None:
    """malloc/realloc size is n*sizeof without calloc/reallocarray two-arg form."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            for fname in ("malloc", "realloc", "calloc", "reallocarray"):
                args = _find_call_args(ln, fname)
                if args is None:
                    continue
                if fname == "calloc" and len(args) >= 2:
                    if not any(_has_wrap_mul(a) for a in args):
                        continue
                elif fname == "reallocarray" and len(args) >= 3:
                    if not any(_has_wrap_mul(a) for a in args):
                        continue
                size_args: list[str] = []
                if fname == "malloc":
                    size_args = [args[0]]
                elif fname == "realloc" and len(args) >= 2:
                    size_args = [args[1]]
                else:
                    size_args = [a for a in args if _has_wrap_mul(a)]
                for sz in size_args:
                    if not _has_wrap_mul(sz):
                        continue
                    key = (fname, i)
                    if key in seen:
                        continue
                    seen.add(key)
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="INT-WRAP-ALLOC",
                        message=f"{fname}() size uses n*sizeof; may wrap before cast",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else "",
                    ))


_FMT_MACRO_DEF = re.compile(
    r'^\s*#\s*define\s+([A-Z_][A-Z0-9_]*)\s+(?:L|u8|u|U)?"'
)
_FMT_PIECES = re.compile(r'(?:\s*(?:[A-Z_][A-Z0-9_]*|(?:L|u8|u|U)?"[^"]*"))+\s*')
_FMT_MACRO_NAME = re.compile(r"[A-Z_][A-Z0-9_]*")


def _string_macros(lines: list[str]) -> set[str]:
    """ALL_CAPS names this file #defines to a string literal."""
    names: set[str] = set()
    for ln in lines:
        if "define" in ln:
            m = _FMT_MACRO_DEF.match(ln)
            if m:
                names.add(m.group(1))
    return names


def _is_literal_format(fmt: str, macros: set[str]) -> bool:
    """A string literal, or literals and string macros pasted together.

    `#define FMT "%d\\n"` then `printf(FMT, x)` is a literal format.
    """
    if _is_string_literal(fmt):
        return True
    if not macros or not _FMT_PIECES.fullmatch(fmt):
        return False
    names = _FMT_MACRO_NAME.findall(_rx(r'"[^"]*"').sub(" ", fmt))
    return bool(names) and all(n in macros for n in names)


def _fmt_string(lines, rel, funcs, out) -> None:
    """printf-family calls whose format argument is not a string literal.

    An ALL_CAPS macro #defined to a string literal in the same file is a
    literal.
    """
    macros = _string_macros(lines)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            for fname, idx in _FMT_FN.items():
                args = _find_call_args(ln, fname)
                if args is None or len(args) <= idx:
                    continue
                fmt = args[idx].strip()
                if _is_literal_format(fmt, macros):
                    continue
                key = (fname, i)
                if key in seen:
                    continue
                seen.add(key)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="FMT-STRING",
                    message=f"{fname}() format argument is not a string literal",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))


def _signed_index_names(fn) -> set[str]:
    """Parameters and locals that are signed integers, not pointers."""
    names: set[str] = set()
    for typ, name in fn.params:
        if not name or "*" in typ or "[" in typ:
            continue
        if _UNSIGNED_TY.search(typ):
            continue
        words = _rx(r"[A-Za-z_]\w*").findall(typ)
        if any(w in _SIGNED_WORDS for w in words):
            names.add(name)
    for m in _rx(r"\b(?:signed\s+)?(?:int|short|long|char|ssize_t|ptrdiff_t|int\d+_t"
        r"|intmax_t|intptr_t|off_t|pid_t)\s+([A-Za-z_]\w*)\s*;").finditer(fn.body):
        names.add(m.group(1))
    return names


def _has_floor(name: str, text: str) -> bool:
    """A comparison that pins V non-negative, anywhere in the function."""
    v = re.escape(name)
    return bool(re.search(
        rf"\b{v}\s*(?:<\s*0|>=\s*0|>\s*-\s*1|==\s*-\s*1|!=\s*-\s*1|<=\s*-\s*1)"
        rf"|0\s*(?:>|<=|<|>=)\s*\b{v}\b"
        rf"|-\s*1\s*(?:<|>=|==|!=)\s*\b{v}\b"
        rf"|\(\s*(?:unsigned|u_int|u_long|size_t|uint\d+_t)\s*\)\s*\(?\s*\b{v}\b",
        text,
    ))


def _pinned_or_loop(name: str, text: str) -> bool:
    v = re.escape(name)
    return bool(re.search(
        rf"\b{v}\s*=\s*(?:0|[1-9]\d*)\s*[;,)]"
        rf"|\bfor\s*\(\s*(?:[A-Za-z_]\w*\s+)?{v}\s*=",
        text,
    ))


def _onesided_index(lines, rel, funcs, out) -> None:
    """Signed index with an upper bound (`i > n` / `i >= n`) and no `i < 0`.

    Narrow shape from ParanoidBSD onesided_index.py, without KNF or
    FreeBSD headers. A loop counter, an unsigned index, a two-sided
    guard, and an index with no ceiling are all skipped.
    """
    for fn in funcs:
        signed = _signed_index_names(fn)
        if not signed:
            continue
        start, end = fn.span
        chunk = "\n".join(lines[start - 1 : end])
        seen: set[str] = set()
        for m in ONESIDED.finditer(chunk):
            i = m.group("i")
            if i not in signed or i in seen:
                continue
            if _has_floor(i, chunk) or _pinned_or_loop(i, chunk):
                continue
            if not re.search(rf"\[\s*{re.escape(i)}\s*\]", chunk):
                continue
            seen.add(i)
            line = start + chunk[: m.start()].count("\n")
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="MEM-ONESIDED-INDEX",
                message=f"{i} is a signed index bounded above only; no {i} < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip() if 0 < line <= len(lines) else "",
            ))


def _null_test_for(var: str, text: str) -> bool:
    """True if `text` contains a NULL/truth test on `var`."""
    v = re.escape(var)
    if re.search(
        rf"\bif\s*\(\s*(?:{v}\s*==\s*(?:NULL|0|nullptr)"
        rf"|!\s*{v}\b"
        rf"|{v}\s*\))",
        text,
    ):
        return True
    if re.search(
        rf"\(\s*{v}\s*=(?!=)[^;)]*?\)\s*==\s*(?:NULL|nullptr|0)\b",
        text,
    ):
        return True
    return False


def _alloc_in_null_condition(line: str, var: str) -> bool:
    """`if ((p = malloc(n)) == NULL)` — alloc and test on one condition."""
    v = re.escape(var)
    return bool(
        _rx(r"\bif\s*\(").search(line)
        and re.search(rf"\b{v}\s*=(?!=)", line)
        and _rx(r"==\s*(?:NULL|nullptr|0)\b").search(line)
    )


def _ptr_use_in(var: str, text: str) -> bool:
    """`var->`, `*var` or `var[` in text, sizeof operands aside."""
    v = re.escape(var)
    stripped = _rx(r"sizeof\s*\([^)]*\)").sub("", text)
    if re.search(rf"\b{v}\s*->", stripped):
        return True
    if re.search(rf"\*\s*{v}\b", stripped):
        return True
    return bool(re.search(rf"\b{v}\s*\[", stripped))


def _first_ptr_use(var: str, chunk: list[str], start: int) -> int | None:
    for j in range(start + 1, len(chunk)):
        if _ptr_use_in(var, chunk[j]):
            return j
    return None


def _stmt_end(line: str, pos: int) -> int:
    """Index of the `;` ending the statement running through `pos`, or -1."""
    depth = 0
    for k in range(pos, len(line)):
        c = line[k]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == ";" and depth <= 0:
            return k
    return -1


def _unchecked_alloc_site(
    chunk: list[str],
    i: int,
    m: re.Match,
    *,
    require_nowait: bool,
) -> tuple[str, int] | None:
    var = m.group("var")
    var = _rx(r"\s+").sub("", var)
    fn = m.group("fn")
    line = chunk[i]
    if fn == "realloc":
        arg_m = re.search(
            rf"realloc\s*\(\s*(?P<p>{re.escape(var)})\s*,",
            line,
        )
        if arg_m:
            return None
    if _alloc_in_null_condition(line, var):
        return None
    # `int *p = malloc(4); *p = 3;` on one line is the same unchecked use
    # as on two lines.
    semi = _stmt_end(line, m.end())
    rest = line[semi + 1:] if semi >= 0 else ""
    if rest and _ptr_use_in(var, rest):
        use_j = i
        between = line
    else:
        found = _first_ptr_use(var, chunk, i)
        if found is None:
            return None
        use_j = found
        between = "\n".join(chunk[i:use_j + 1])
    if _null_test_for(var, between):
        return None
    stmt = " ".join(x.strip() for x in chunk[i:i + 4])
    if require_nowait and "M_NOWAIT" not in stmt:
        return None
    return var, use_j


def _unchecked_alloc(lines, rel, funcs, out) -> None:
    """malloc/calloc/realloc result used before any NULL test."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            # UNCHECKED_ALLOC names malloc/calloc/realloc.
            if "alloc" not in ln:
                continue
            m = UNCHECKED_ALLOC.search(ln)
            if not m:
                continue
            stmt = " ".join(x.strip() for x in chunk[i:i + 4])
            if "M_NOWAIT" in stmt:
                continue
            hit = _unchecked_alloc_site(chunk, i, m, require_nowait=False)
            if hit is None:
                continue
            var, use_j = hit
            key = (var, i)
            if key in seen:
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="PTR-UNCHECKED-ALLOC",
                message=f"{var} used without a NULL check after {m.group('fn')}()",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip() if 0 < line <= len(lines) else "",
            ))


def _nowait_alloc(lines, rel, funcs, out) -> None:
    """M_NOWAIT malloc/realloc used before any NULL test."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            # NOWAIT_ALLOC needs M_NOWAIT on the line.
            if "M_NOWAIT" not in ln:
                continue
            m = NOWAIT_ALLOC.search(ln)
            if not m:
                continue
            hit = _unchecked_alloc_site(chunk, i, m, require_nowait=True)
            if hit is None:
                continue
            var, _use_j = hit
            key = (var, i)
            if key in seen:
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="MEM-NOWAIT",
                message=f"{var} used without a NULL check after M_NOWAIT allocation",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip() if 0 < line <= len(lines) else "",
            ))


def _unwrap_wrapping_braces(stmt: str) -> str:
    """Strip a one-line `{ ... }` wrapper so the tail is the inner statements."""
    s = stmt.strip()
    if len(s) >= 2 and s.startswith("{") and s.endswith("}"):
        inner = s[1:-1].strip()
        if inner:
            return inner
    return s


def _missing_return_last_chunk(stmt: str) -> str:
    """Last `;`-separated chunk (`g = 1; return 0;` → `return 0`)."""
    last = ""
    for part in stmt.split(";"):
        chunk = part.strip()
        if chunk:
            last = chunk
    return last


def _last_body_stmt(body_lines: list[str]) -> str | None:
    for ln in reversed(body_lines):
        s = ln.strip()
        # `#endif` after `#else return '.';` is not the last statement.
        if not s or s in ("{", "}") or s.startswith("#"):
            continue
        s = _unwrap_wrapping_braces(s)
        last = _missing_return_last_chunk(s)
        return last if last else s
    return None


_VALUE_RET = re.compile(
    r"\b(?:unsigned\s+)?(?:int|long|short|char)\b"
)
_MISSING_RETURN_OK = re.compile(r"^\s*return\b")
_TERMINATING_TAIL = re.compile(
    r"^\s*(?:\(\s*void\s*\)\s*)?"
    r"(?:abort|exit|_exit|__builtin_unreachable)\s*\("
)
_TERMINATING_FN = re.compile(
    r"\b(?:panic|fatal|errx)\w*\s*\("
)
_FALLTHROUGH_ANNOT = re.compile(
    r"\b(?:fallthrough|FALLTHROUGH|\[\[fallthrough\]\]|"
    r"__attribute__\s*\(\s*\(\s*fallthrough\s*\)\s*\))"
)
_CASE_STOP = re.compile(r"\b(?:break|return|goto|continue)\b")
_CASE_LABEL = re.compile(r"\b(?:case\b[^:]*:|default\s*:)")
_DEAD_GUARD = re.compile(r"\bif\s*\(\s*([A-Za-z_]\w*)\s*<\s*0(?:u|U)?\s*\)")
_LOCAL_UNINIT = re.compile(
    r"^\s*(?:const\s+|volatile\s+)?"
    r"(?:struct\s+\w+|union\s+\w+|enum\s+\w+|"
    r"(?:unsigned\s+)?(?:char|short|int|long|float|double)|"
    r"size_t|ssize_t|ptrdiff_t|int\d+_t|uint\d+_t)\s+"
    r"([A-Za-z_]\w*)\s*;",
    re.M,
)
_LOCAL_PTR_UNINIT = re.compile(
    r"^\s*(?:static\s+)?(?:const\s+|volatile\s+)?"
    r"(?:struct\s+\w+|union\s+\w+|enum\s+\w+|"
    r"(?:unsigned\s+)?(?:char|short|int|long|float|double)|void|"
    r"size_t|ssize_t|ptrdiff_t|int\d+_t|uint\d+_t)\s*"
    r"\*\s*"
    r"([A-Za-z_]\w*)\s*;",
    re.M,
)
_BRANCH_SCALAR = re.compile(
    r"\b(?:if|while)\s*\(\s*(?:!\s*)?(?P<var>[A-Za-z_]\w*)\s*\)"
)


def _is_value_returning(ret: str) -> bool:
    r = ret.strip()
    if _rx(r"\bvoid\b").search(r) or "*" in r:
        return False
    return bool(_VALUE_RET.search(r))


def _missing_return_tail_ok(stmt: str) -> bool:
    s = _unwrap_wrapping_braces(stmt)
    last = _missing_return_last_chunk(s) or s
    if _MISSING_RETURN_OK.match(last):
        return True
    if _TERMINATING_TAIL.match(last) or _TERMINATING_TAIL.match(s):
        return True
    return bool(_TERMINATING_FN.search(last) or _TERMINATING_FN.search(s))


def _missing_return(lines, rel, funcs, out) -> None:
    """Value-returning function with no return or bad final statement."""
    for fn in funcs:
        if not _is_value_returning(fn.return_type):
            continue
        body_lines = fn.body.splitlines()
        if not body_lines:
            continue
        has_return = bool(_rx(r"\breturn\b").search(fn.body))
        last = _last_body_stmt(body_lines)
        if last is None:
            continue
        if has_return and _missing_return_tail_ok(last):
            continue
        if not has_return or not _missing_return_tail_ok(last):
            rel_off = len(body_lines) - 1
            for i in range(len(body_lines) - 1, -1, -1):
                s = body_lines[i].strip()
                if s and s not in ("{", "}"):
                    rel_off = i
                    break
            line = fn.span[0] + rel_off
            msg = (
                f"{fn.name}() has no return statement"
                if not has_return
                else f"{fn.name}() does not end in return or fatal exit"
            )
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CTRL-MISSING-RETURN",
                message=msg,
                strength=laws.STRENGTH_FINDS,
                evidence=last.strip(),
            ))


def _switch_bodies(text: str) -> list[tuple[str, int]]:
    """Switch bodies and 0-based line offset of the opening brace line."""
    out: list[tuple[str, int]] = []
    for m in _rx(r"\bswitch\s*\([^)]*\)").finditer(text):
        rest = text[m.end():]
        brace = rest.find("{")
        if brace < 0:
            continue
        depth = 0
        end = None
        for k, ch in enumerate(rest[brace:]):
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    end = brace + k
                    break
        if end is None:
            continue
        body = rest[brace + 1 : end]
        line_off = text[: m.end() + brace].count("\n")
        out.append((body, line_off))
    return out


def _arm_falls_through(arm: str) -> bool:
    if _FALLTHROUGH_ANNOT.search(arm):
        return False
    has_stmt = False
    for ln in arm.splitlines():
        s = ln.strip()
        # A preprocessor line (`#endif` between stacked labels) is not a statement.
        if not s or s.startswith("#"):
            continue
        if _CASE_LABEL.search(s):
            continue
        has_stmt = True
        if _CASE_STOP.search(s):
            return False
    return has_stmt


# GCC -Wimplicit-fallthrough comment forms (`/* Falls through. */`,
# `// FALLTHRU`, `/* no break */`): the author annotated the intent.
_FALLTHROUGH_COMMENT = re.compile(
    r"(?://|/\*).*?(?:\bfalls?[ \t-]*thr(?:ough|u)|\bno\s*break)", re.I)


def _fallthrough(lines, orig_lines, rel, funcs, out) -> None:
    """Switch case reaches the next arm without break or annotation."""
    for fn in funcs:
        start = fn.span[0]
        seen: set[int] = set()
        # `case '}':` / `case ':':` - a char literal is neither a brace nor a label end.
        text = _CHAR_LITERAL.sub(lambda m: " " * len(m.group()), fn.body)
        for body, line_off in _switch_bodies(text):
            cases = list(_lit_finditer(_CASE_LABEL, body))
            # The last arm runs out of the switch, not into a label.
            for idx, cm in enumerate(cases[:-1]):
                arm_end = cases[idx + 1].start()
                arm = body[cm.end() : arm_end]
                if not _arm_falls_through(arm):
                    continue
                rel_off = body[: cm.start()].count("\n")
                next_off = body[:arm_end].count("\n")
                first = start + line_off + rel_off
                last = max(first, start + line_off + next_off - 1)
                if any(0 < ln <= len(orig_lines)
                       and _FALLTHROUGH_COMMENT.search(orig_lines[ln - 1])
                       for ln in range(first, last + 1)):
                    continue
                key = rel_off
                if key in seen:
                    continue
                seen.add(key)
                line = start + line_off + rel_off + 1
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CTRL-FALLTHROUGH",
                    message="case falls through to the next label without annotation",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))


def _dead_guard(lines, rel, funcs, out) -> None:
    """Unsigned value compared below zero."""
    for fn in funcs:
        unsigned = _unsigned_names(fn)
        if not unsigned:
            continue
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _DEAD_GUARD.search(ln)
            if not m or m.group(1) not in unsigned:
                continue
            key = (m.group(1), i)
            if key in seen:
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CTRL-DEAD-GUARD",
                message=f"{m.group(1)} is unsigned; {m.group(1)} < 0 is always false",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))


_EMPTY_INF = re.compile(
    r"(?:"
    r"\bfor\s*\(\s*;\s*;\s*\)\s*(?:;|\{\s*\})"
    r"|\bwhile\s*\(\s*(?:1|true)\s*\)\s*(?:;|\{\s*\})"
    r"|\bdo\s*(?:;|\{\s*\})\s*while\s*\(\s*(?:1|true)\s*\)"
    r")"
)


def _empty_infinite(lines, rel, funcs, out) -> None:
    """Empty `for(;;)` / `while(1)` / `do;while(1)`. A body with work is not this.

    Server loops (`while (1) { accept(); }`) are out of scope.
    """
    for fn in funcs:
        start = fn.span[0]
        seen: set[int] = set()
        for m in _lit_finditer(_EMPTY_INF, fn.body):
            i = fn.body[: m.start()].count("\n")
            if i in seen:
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CTRL-EMPTY-INFINITE",
                message="empty infinite loop (no body, no exit)",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else fn.body[m.start():m.end()].strip(),
            ))


def _uninit_locals(fn) -> set[str]:
    params = {name for typ, name in fn.params if name}
    names: set[str] = set()
    for m in _lit_finditer(_LOCAL_UNINIT, fn.body):
        name = m.group(1)
        if name not in params and name not in _KW:
            names.add(name)
    return names


def _uninit_return(lines, rel, funcs, out) -> None:
    """Return of a local declared without initializer and never assigned."""
    for fn in funcs:
        uninit = _uninit_locals(fn)
        if not uninit:
            continue
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _RETURN_VAR.search(ln)
            if not m:
                continue
            var = m.group(1)
            if var not in uninit:
                continue
            if re.search(rf"\b{re.escape(var)}\s*=(?!=)", fn.body):
                continue
            key = (var, i)
            if key in seen:
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="UNINIT-RETURN",
                message=f"return {var}; {var} is never initialised",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))


def _local_uninit_pointers(fn) -> set[str]:
    params = {name for typ, name in fn.params if name}
    names: set[str] = set()
    for m in _lit_finditer(_LOCAL_PTR_UNINIT, fn.body):
        name = m.group(1)
        if name not in params and name not in _KW:
            names.add(name)
    return names


def _ptr_decl_line(p: str, ln: str) -> bool:
    m = _LOCAL_PTR_UNINIT.match(ln)
    return m is not None and m.group(1) == p


def _ptr_uninit(lines, rel, funcs, out) -> None:
    """Local pointer dereferenced before it is assigned."""
    for fn in funcs:
        if fn.kind == "POINTER":
            continue
        ptrs = _local_uninit_pointers(fn)
        if not ptrs:
            continue
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for p in ptrs:
            v = re.escape(p)
            for i, ln in enumerate(chunk):
                if _ptr_decl_line(p, ln):
                    continue
                if not (re.search(rf"\*{v}\b", ln) or re.search(rf"\b{v}\s*->", ln)):
                    continue
                prior = "\n".join(chunk[:i])
                if re.search(rf"\b{v}\s*=(?!=)", prior):
                    continue
                key = (p, i)
                if key in seen:
                    continue
                seen.add(key)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="PTR-UNINIT",
                    message=f"{p} dereferenced before it is assigned",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))


def _uninit_branch(lines, rel, funcs, out) -> None:
    """Scalar local used in if/while before it is assigned."""
    for fn in funcs:
        uninit = _uninit_locals(fn)
        if not uninit:
            continue
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _BRANCH_SCALAR.search(ln)
            if not m:
                continue
            var = m.group("var")
            if var not in uninit:
                continue
            prior = "\n".join(chunk[:i])
            if re.search(rf"\b{re.escape(var)}\s*=(?!=)", prior):
                continue
            # `te_interp(expr, &err); if (err)`: assigned through its address.
            if re.search(rf"(?<![&\w])&\s*{re.escape(var)}\b", prior):
                continue
            key = (var, i)
            if key in seen:
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="UNINIT-BRANCH",
                message=f"{var} used in branch before it is assigned",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))


def _local_char_array_sizes(fn) -> dict[str, int]:
    params = {name for typ, name in fn.params if name}
    sizes: dict[str, int] = {}
    for m in _lit_finditer(_LOCAL_CHAR_ARRAY, fn.body):
        if m.group("static"):
            continue
        name = m.group("name")
        if name not in params and name not in _KW:
            sizes[name] = int(m.group("size"))
    return sizes


def _sizeof_dst(dst: str, buf: str) -> bool:
    d = _rx(r"\s+").sub("", dst)
    b = _rx(r"\s+").sub("", buf)
    return d in {f"sizeof({b})", f"sizeof{b}"}


def _string_lit_len(s: str) -> int | None:
    s = s.strip()
    if not _rx(r'^L?"').match(s):
        return None
    inner = s[1:-1]
    length = 0
    i = 0
    while i < len(inner):
        if inner[i] == "\\":
            i += 2 if i + 1 < len(inner) else 1
            length += 1
            continue
        length += 1
        i += 1
    return length


def _off_by_one(lines, rel, funcs, out, text: str = "") -> None:
    """strncpy/strcpy into a local buffer with no room for a trailing NUL.

    `lines` blank literal contents (an escape pair is two blanks), so a
    strcpy source literal is measured on the raw line from `text`.
    """
    raw_lines: list[str] = []
    for fn in funcs:
        arrays = _local_char_array_sizes(fn)
        if not arrays:
            continue
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            args = _find_call_args(ln, "strncpy")
            if args is not None and len(args) >= 3:
                dst = _rx(r"\s+").sub("", args[0])
                if dst in arrays:
                    m = arrays[dst]
                    n_arg = _rx(r"\s+").sub("", args[2])
                    if n_arg == str(m) or _sizeof_dst(n_arg, dst):
                        key = ("strncpy", i)
                        if key not in seen:
                            seen.add(key)
                            line = start + i
                            out.append(Finding(
                                stage="lints", status=laws.FAILED, file=rel,
                                function=fn.name, line=line, cls="STR-OFF-BY-ONE",
                                message=f"strncpy({dst}, …, {m}) leaves no room for NUL "
                                f"in {dst}[{m}]",
                                strength=laws.STRENGTH_FINDS,
                                evidence=lines[line - 1].strip()
                                if 0 < line <= len(lines) else "",
                            ))
            args = _find_call_args(ln, "strcpy")
            if args is not None and len(args) >= 2:
                dst = _rx(r"\s+").sub("", args[0])
                if dst in arrays and _string_lit_len(args[1]) is not None:
                    if text and not raw_lines:
                        raw_lines = strip_comments_keep_lines(
                            text, blank_strings=False,
                        ).splitlines()
                    raw_ln = start + i
                    if 0 < raw_ln <= len(raw_lines):
                        raw_args = _find_call_args(raw_lines[raw_ln - 1], "strcpy")
                        if (raw_args is not None and len(raw_args) >= 2
                                and _rx(r"\s+").sub("", raw_args[0]) == dst):
                            args = raw_args
                    lit_len = _string_lit_len(args[1])
                    if lit_len is not None and lit_len + 1 > arrays[dst]:
                        key = ("strcpy", i)
                        if key not in seen:
                            seen.add(key)
                            line = start + i
                            out.append(Finding(
                                stage="lints", status=laws.FAILED, file=rel,
                                function=fn.name, line=line, cls="STR-OFF-BY-ONE",
                                message=f"strcpy({dst}, …) needs {lit_len + 1} bytes "
                                f"in {dst}[{arrays[dst]}]",
                                strength=laws.STRENGTH_FINDS,
                                evidence=lines[line - 1].strip()
                                if 0 < line <= len(lines) else "",
                            ))


def _str_missing_nul(lines, rel, funcs, out) -> None:
    """memcpy of a string literal whose count equals the content length.

    That copies every character and drops the implicit terminator.
    `memcpy(dst, "hi", 3)` / `sizeof("hi")` includes the NUL and is not
    this. strncpy with n == sizeof(buf) is STR-OFF-BY-ONE.
    """
    for fn in funcs:
        start = fn.span[0]
        seen: set[int] = set()
        for i, ln in enumerate(fn.body.splitlines()):
            args = _find_call_args(ln, "memcpy")
            if args is None or len(args) < 3:
                continue
            lit_len = _string_lit_len(args[1])
            if lit_len is None:
                continue
            n = _parse_int_literal(args[2])
            if n is None or n != lit_len:
                continue
            if i in seen:
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="STR-MISSING-NUL",
                message=f"memcpy(…, string, {n}) omits the terminator",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _declared_noreturn(fn, lines) -> bool:
    if NORETURN_ATTR.search(fn.signature):
        return True
    start = max(0, fn.span[0] - 3)
    head = "\n".join(lines[start:fn.span[0]])
    if NORETURN_ATTR.search(head):
        return True
    return False


def _noreturn_fatal(lines, rel, funcs, out) -> None:
    """Function whose last statement is exit/abort/err* but is not noreturn."""
    for fn in funcs:
        if fn.name == "main":
            continue
        if _declared_noreturn(fn, lines):
            continue
        body_lines = fn.body.splitlines()
        if _rx(r"\breturn\b").search(fn.body):
            continue
        last = _last_body_stmt(body_lines)
        if last is None:
            continue
        m = FATAL_TAIL.match(last.strip())
        if not m:
            continue
        rel_off = len(body_lines) - 1
        for i in range(len(body_lines) - 1, -1, -1):
            s = body_lines[i].strip()
            if s and s not in ("{", "}"):
                rel_off = i
                break
        line = fn.span[0] + rel_off
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=fn.name, line=line, cls="FUNC-NORETURN",
            message=f"{fn.name}() ends in {m.group('callee')}(), not declared noreturn",
            strength=laws.STRENGTH_FINDS,
            evidence=last.strip(),
        ))


def _mem_overlap(lines, rel, funcs, out) -> None:
    """memcpy with the same identifier as both destination and source."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[int] = set()
        for i, ln in enumerate(chunk):
            args = _find_call_args(ln, "memcpy")
            if args is None or len(args) < 2:
                continue
            dst = _rx(r"\s+").sub("", args[0])
            src = _rx(r"\s+").sub("", args[1])
            if dst != src or not _rx(r"^[A-Za-z_]\w*$").match(dst):
                continue
            if i in seen:
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="MEM-OVERLAP",
                message=f"memcpy({dst}, {dst}, …) has overlapping source and destination",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))


def _capacity_first(lines, rel, func_of, out) -> None:
    """Struct-field capacity grown, then used as malloc/memcpy size, then
    a failure arm that neither restores the field nor kills the process.

    Port of ParanoidBSD capacity_first.py scan(), plus memcpy/memmove.
    """
    for i, line in enumerate(lines):
        m = CAP_GROW.match(line)
        if not m:
            continue
        var = _cap_norm(m.group("a") or m.group("b") or m.group("c"))
        aidx = None
        for j in range(i + 1, min(i + 6, len(lines))):
            if CAP_ALLOC.search(lines[j]):
                aidx = j
                break
        if aidx is None:
            continue
        if var not in _cap_norm("".join(lines[aidx:aidx + 4])):
            continue
        arm = None
        for j in range(aidx + 1, min(aidx + 7, len(lines))):
            if CAP_NULLT.search(lines[j]) and CAP_LEAVE.search("".join(lines[j:j + 4])):
                arm = "".join(lines[j:j + 5])
                break
        if arm is None:
            continue
        if CAP_DEAD.search(arm):
            continue
        if var in _cap_norm(arm):
            continue
        ln = i + 1
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=func_of(ln), line=ln, cls="MEM-CAPACITY-FIRST",
            message=f"{var} grown before the allocation that is supposed to earn it",
            strength=laws.STRENGTH_FINDS, evidence=line.strip(),
        ))


def _div_zero_const(lines, rel, func_of, out) -> None:
    for i, ln in enumerate(lines, 1):
        if _rx(r"[/%]\s*0\b").search(ln) and not _rx(r"[0-9]\s*[/%]\s*0\.\d").search(ln):
            # integer /0 or %0; float 0.0 is defined IEEE
            if _rx(r"[/%]\s*0\.").search(ln):
                continue
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=func_of(i), line=i, cls="INT-DIV-ZERO",
                message="integer division or modulo by constant 0",
                strength=laws.STRENGTH_FINDS, evidence=ln.strip(),
            ))


def _then_branch(lines: list[str], i: int, cond_end: int) -> tuple[str, int]:
    """The then-branch of the `if (...)` whose condition ends at
    lines[i][cond_end], and the index of the first line after it.

    `if (!n) return;` is a one-line branch (the next line is not in it);
    a braced branch ends at its own `}`, so `} else { n->x }` is not in it.
    """
    rest = lines[i][cond_end:]
    if rest.strip() and not rest.lstrip().startswith("{"):
        return rest, i + 1
    k = i
    while not rest.strip() and k + 1 < len(lines):
        k += 1
        rest = lines[k]
    if not rest.lstrip().startswith("{"):
        return (rest if k > i else ""), k + 1
    depth = 0
    buf: list[str] = []
    line_k, text = k, rest
    while True:
        for c in text:
            if c == "{":
                depth += 1
                if depth == 1:
                    continue
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return "".join(buf), line_k + 1
            buf.append(c)
        buf.append("\n")
        line_k += 1
        if line_k >= len(lines):
            return "".join(buf), line_k
        text = lines[line_k]


def _null_branch(lines, rel, func_of, out) -> None:
    """Pointer used inside the branch that proved it NULL."""
    i = 0
    while i < len(lines):
        m = NULL_TEST.search(lines[i])
        if not m:
            i += 1
            continue
        p = m.group("p1") or m.group("p2")
        body, j = _then_branch(lines, i, m.end())
        if re.search(rf"\b{re.escape(p)}\s*=", body):
            i = j
            continue
        if re.search(rf"\b{re.escape(p)}\s*(->|\.)", body):
            ln = i + 1
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=func_of(ln), line=ln, cls="PTR-NULL-DEREF",
                message=f"{p} dereferenced in the branch that proved it NULL",
                strength=laws.STRENGTH_FINDS, evidence=lines[i].strip(),
            ))
        i = j if j > i + 1 else i + 1


def _popcount(n: int) -> int:
    return bin(n).count("1")


def _masked_switch(lines, rel, func_of, out) -> None:
    text = "\n".join(lines)
    for m in _lit_finditer(MASKED_SWITCH, text):
        expr = m.group(1)
        # locate line
        line = text[: m.start()].count("\n") + 1
        # crude block
        rest = text[m.end():]
        brace = rest.find("{")
        if brace < 0:
            continue
        depth = 0
        end = None
        for k, ch in enumerate(rest[brace:]):
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    end = brace + k
                    break
        if end is None:
            continue
        body = rest[brace : end + 1]
        if _rx(r"\bdefault\s*:").search(body):
            continue
        cases = _rx(r"\bcase\b").findall(body)
        lit = _rx(r"0x([0-9a-fA-F]+)|(\d+)\s*$").search(expr.replace(")", " ").split("&")[-1].strip())
        named = _rx(r"[A-Za-z_]\w*").findall(expr.split("&", 1)[-1])
        named = [n for n in named if n not in {"if", "switch"}]
        if lit:
            mask = int(lit.group(1), 16) if lit.group(1) else int(lit.group(2))
            states = 1 << _popcount(mask)
            nbits = _popcount(mask)
            if nbits > 4:
                continue
        else:
            nbits = max(1, len(set(named)))
            if nbits > 4:
                continue
            states = 1 << nbits
        if len(cases) >= states:
            continue
        # uninit escape: a name assigned in an arm, declared without init, read after
        assigned = set(_rx(r"\b([A-Za-z_]\w*)\s*=").findall(body))
        after_line = line + body.count("\n")
        after = "\n".join(lines[after_line: after_line + 40])
        declared = set()
        for f_line in lines[max(0, line - 80): line]:
            dm = _rx(r"\s*(?:int|unsigned|long|char|size_t|uint\d+_t|int\d+_t)\s+"
                r"([A-Za-z_]\w*)\s*;").match(f_line)
            if dm:
                declared.add(dm.group(1))
        escape = assigned & declared
        used = [n for n in escape if re.search(rf"\b{n}\b", after)]
        if not used:
            continue
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=func_of(line), line=line, cls="UNINIT-SWITCH",
            message=f"masked switch has {len(cases)} arms for {states} states; {used[0]} may escape uninitialised",
            strength=laws.STRENGTH_FINDS, evidence=expr.strip()[:120],
        ))


_LOCK_ORDER_CALL = re.compile(r"\b(\w*lock)\s*\(\s*&?([A-Za-z_]\w*)")
_TOCTOU_CHECK = re.compile(r"\b(?:access|stat|lstat)\s*\(")
_TOCTOU_USE = re.compile(r"\b(?:open|fopen|unlink|remove|chmod)\s*\(")
_CXX_TYPE = re.compile(r"\b(?:struct|class)\s+([A-Za-z_]\w*)\b")
_CXX_VIRTUAL = re.compile(r"\bvirtual\b[^{;]*?\b([A-Za-z_]\w*)\s*\(")
_CXX_CALL = re.compile(r"(?:\b(?:this\s*->\s*)?([A-Za-z_]\w*)\s*\()")
_DTOR_HEAD = re.compile(
    r"(?m)^[ \t]*(?:[A-Za-z_]\w*::)?~(?P<name>[A-Za-z_]\w*)\s*\([^;{}]*\)[^{;]*\{"
)


def _first_two_locks(body: str) -> tuple[str, str] | None:
    """First two distinct mutex names acquired in textual order."""
    seen: set[str] = set()
    seq: list[str] = []
    for ln in body.splitlines():
        for m in _LOCK_ORDER_CALL.finditer(ln):
            if "unlock" in m.group(1).lower():
                continue
            name = m.group(2)
            if name in seen:
                continue
            seen.add(name)
            seq.append(name)
            if len(seq) >= 2:
                return seq[0], seq[1]
    return None


def _lock_order(lines, rel, funcs, out) -> None:
    """Two functions in one TU lock the same pair of mutexes in opposite order."""
    orders: dict[str, tuple[str, str]] = {}
    first_line: dict[str, int] = {}
    for fn in funcs:
        pair = _first_two_locks(fn.body)
        if not pair:
            continue
        orders[fn.name] = pair
        start = fn.span[0]
        seen: set[str] = set()
        for i, ln in enumerate(fn.body.splitlines()):
            for m in _LOCK_ORDER_CALL.finditer(ln):
                if "unlock" in m.group(1).lower():
                    continue
                name = m.group(2)
                if name in seen:
                    continue
                seen.add(name)
                first_line[fn.name] = start + i
                break
            if fn.name in first_line:
                break
    reported: set[str] = set()
    names = list(orders)
    for i, a in enumerate(names):
        x1, y1 = orders[a]
        for b in names[i + 1:]:
            x2, y2 = orders[b]
            if {x1, y1} != {x2, y2}:
                continue
            if x1 == x2 and y1 == y2:
                continue
            for fn_name in (a, b):
                if fn_name in reported:
                    continue
                reported.add(fn_name)
                x_ord, y_ord = orders[fn_name]
                line = first_line.get(fn_name, funcs[0].span[0] if funcs else 1)
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn_name, line=line, cls="LOCK-ORDER",
                    message=f"locks {x_ord} then {y_ord}; another function "
                    f"locks them in the opposite order",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))


_FILE_INT_GLOBAL = re.compile(r"^int\s+(?P<name>[A-Za-z_]\w*)\s*;")
_FILE_UNSIGNED_GLOBAL = re.compile(r"^unsigned\s+(?P<name>[A-Za-z_]\w*)\s*;")
_LOCK_IN_CALL = re.compile(
    r"\b(?:mtx_lock|pthread_mutex|\w*lock\w*)\s*\(", re.I
)


def _file_scope_int_globals(lines: list[str]) -> set[str]:
    depth = 0
    globals_: set[str] = set()
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            depth += stripped.count("{") - stripped.count("}")
            continue
        if depth == 0:
            m = _FILE_INT_GLOBAL.match(line)
            if m:
                globals_.add(m.group("name"))
            else:
                m = _FILE_UNSIGNED_GLOBAL.match(line)
                if m:
                    globals_.add(m.group("name"))
        depth += stripped.count("{") - stripped.count("}")
    return globals_


def _global_rmw(body: str, name: str) -> bool:
    v = re.escape(name)
    if re.search(rf"\b{v}\s*=\s*{v}\s*\+", body):
        return True
    if re.search(rf"\b{v}\+\+", body):
        return True
    if re.search(rf"\+\+\s*{v}\b", body):
        return True
    if re.search(rf"\b{v}\s*\+=", body):
        return True
    return False


def _fn_has_lock_call(body: str) -> bool:
    return bool(_lit_search(_LOCK_IN_CALL, body))


def _conc_atomicity(lines, rel, funcs, out) -> None:
    """Non-atomic read-modify-write on file-scope int/unsigned global."""
    globals_ = _file_scope_int_globals(lines)
    if not globals_:
        return
    reported: set[tuple[str, str]] = set()
    for fn in funcs:
        if _fn_has_lock_call(fn.body):
            continue
        for g in sorted(globals_):
            if not _global_rmw(fn.body, g):
                continue
            key = (fn.name, g)
            if key in reported:
                continue
            reported.add(key)
            v = re.escape(g)
            body_lines = fn.body.splitlines()
            line = fn.span[0]
            for i, ln in enumerate(body_lines):
                if (re.search(rf"\b{v}\s*=\s*{v}\s*\+", ln)
                        or re.search(rf"\b{v}\+\+", ln)
                        or re.search(rf"\+\+\s*{v}\b", ln)
                        or re.search(rf"\b{v}\s*\+=", ln)):
                    line = fn.span[0] + i
                    break
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CONC-ATOMICITY",
                message=f"global '{g}' incremented without mutex protection",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))


def _conc_toctou(lines, rel, funcs, out) -> None:
    """Path checked with access/stat/lstat then opened or mutated later."""
    for fn in funcs:
        body_lines = fn.body.splitlines()
        check_at: int | None = None
        use_at: int | None = None
        for i, ln in enumerate(body_lines):
            if check_at is None and _TOCTOU_CHECK.search(ln):
                check_at = i
            elif check_at is not None and i > check_at and _TOCTOU_USE.search(ln):
                use_at = i
                break
        if check_at is None or use_at is None:
            continue
        line = fn.span[0] + use_at
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=fn.name, line=line, cls="CONC-TOCTOU",
            message="path checked then opened or modified without holding "
            "authority across the gap",
            strength=laws.STRENGTH_FINDS,
            evidence=lines[line - 1].strip()
            if 0 < line <= len(lines) else "",
        ))


def _cxx_virtual_in_ctor(text: str, lines, rel, funcs, out) -> None:
    """C++ only: constructor calls a method declared virtual in the TU."""
    if "virtual" not in text:
        return
    virtuals = {m.group(1) for m in _lit_finditer(_CXX_VIRTUAL, text)}
    if not virtuals:
        return
    type_names = set(_CXX_TYPE.findall(text))
    reported: set[str] = set()
    for fn in funcs:
        is_ctor = fn.name in type_names or "::" in fn.name
        fallback = "ctor" in fn.name.lower()
        if not is_ctor and not fallback:
            continue
        calls = set(_CXX_CALL.findall(fn.body))
        hit = calls & virtuals
        if not hit:
            continue
        if fn.name in reported:
            continue
        reported.add(fn.name)
        callee = sorted(hit)[0]
        start = fn.span[0]
        line = start
        for i, ln in enumerate(fn.body.splitlines()):
            if re.search(rf"(?:\b(?:this\s*->\s*)?{re.escape(callee)}\s*\()", ln):
                line = start + i
                break
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=fn.name, line=line, cls="CXX-VIRTUAL-IN-CTOR",
            message=f"constructor calls virtual {callee}()",
            strength=laws.STRENGTH_FINDS,
            evidence=lines[line - 1].strip()
            if 0 < line <= len(lines) else "",
        ))


def _unlock_of(name: str) -> str | None:
    hits = list(_rx(r"(?i)lock").finditer(name))
    if not hits:
        return None
    if "ASSERT" in name.upper():
        return None
    m = hits[-1]
    if name[max(0, m.start() - 2): m.start()].lower() == "un":
        return None
    got = m.group(0)
    rep = "UNLOCK" if got.isupper() else "unlock"
    return name[: m.start()] + rep + name[m.end():]


def _lock_of(name: str) -> str | None:
    """Partner that acquires what `name` releases, or None."""
    hits = list(_rx(r"(?i)unlock").finditer(name))
    if not hits:
        return None
    if "ASSERT" in name.upper():
        return None
    m = hits[-1]
    got = m.group(0)
    if got.isupper():
        rep = "LOCK"
    elif got[0].isupper():
        rep = "Lock"
    else:
        rep = "lock"
    return name[: m.start()] + rep + name[m.end():]


def _lock_balance(lines, rel, funcs, out) -> None:
    """Inconsistency: released on some returns, not others. Never 'never unlocked'."""
    for fn in funcs:
        body_lines = fn.body.splitlines()
        pairs: dict[tuple[str, str], list[int]] = {}
        unlocks: dict[tuple[str, str], list[int]] = {}
        for i, ln in enumerate(body_lines):
            for m in LOCK_CALL.finditer(ln):
                name, arg = m.group(1), " ".join(m.group(2).split())
                u = _unlock_of(name)
                if u:
                    pairs.setdefault((name, arg), [])
                    pairs[(name, arg)].append(i)
                # unlocks
                if _rx(r"(?i)unlock").search(name):
                    unlocks.setdefault((name, arg), []).append(i)
        returns = [i for i, ln in enumerate(body_lines) if _rx(r"\s*return\b").match(ln)]
        if len(returns) < 2:
            continue
        for (lock, arg), _hits in pairs.items():
            u = _unlock_of(lock)
            if not u:
                continue
            rels = unlocks.get((u, arg), [])
            if not rels:
                continue  # never unlocked = contract, not a finding
            # very coarse: a return after which no unlock dominates if unlock
            # is not textually before it. Over-reports, which is the safe side.
            held_returns = []
            for r in returns:
                if any(u_i < r for u_i in rels) and not (
                    # unlock after last lock before this return
                    True
                ):
                    pass
                last_lock = max((h for h in _hits if h < r), default=-1)
                last_un = max((h for h in rels if h < r), default=-1)
                if last_lock > last_un:
                    held_returns.append(r)
            if held_returns and len(held_returns) < len(returns):
                ln = fn.span[0] + held_returns[0]
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=ln, cls="LOCK-IMBALANCE",
                    message=f"{lock} held on {len(held_returns)} of {len(returns)} returns; released on the others",
                    strength=laws.STRENGTH_FINDS, evidence=lock + "(" + arg + ")",
                ))


def _lock_double_unlock(lines, rel, funcs, out) -> None:
    """Two unlocks of the same lock with no acquire between them.

    A function that unlocks once without locking is a contract, not a
    finding. A second unlock before the next acquire is CWE-765. An
    unlock inside a block that leaves (see exit_blocks) does not reach
    the code after the block.
    """
    for fn in funcs:
        held: dict[tuple[str, str], bool] = {}
        body_lines = fn.body.splitlines()
        replay: _ExitReplay | None = None
        if _rx(r"(?i)unlock").search(fn.body):
            blocks = exit_blocks(body_lines)
            if blocks:
                replay = _ExitReplay(
                    blocks, lambda: dict(held), _held_restore(held),
                    _held_merge(held),
                )
        for i, ln in enumerate(body_lines):
            if replay is not None:
                replay.before(i)
            found = False
            for m in LOCK_CALL.finditer(ln):
                name, arg = m.group(1), " ".join(m.group(2).split())
                if _rx(r"(?i)unlock").search(name):
                    key = (name, arg)
                    if held.get(key) is False:
                        line = fn.span[0] + i
                        out.append(Finding(
                            stage="lints", status=laws.FAILED, file=rel,
                            function=fn.name, line=line,
                            cls="LOCK-DOUBLE-UNLOCK",
                            message=f"{name}({arg}) released twice without "
                            "an acquire between",
                            strength=laws.STRENGTH_FINDS,
                            evidence=lines[line - 1].strip()
                            if 0 < line <= len(lines) else ln.strip(),
                        ))
                        found = True
                        break
                    held[key] = False
                    continue
                u = _unlock_of(name)
                if u:
                    held[(u, arg)] = True
            if found:
                break
            if replay is not None:
                replay.after(i)


def _held_restore(
    held: dict[tuple[str, str], bool],
) -> Callable[[dict[tuple[str, str], bool]], None]:
    def restore(st: dict[tuple[str, str], bool]) -> None:
        held.clear()
        held.update(st)
    return restore


def _held_merge(
    held: dict[tuple[str, str], bool],
) -> Callable[[dict[tuple[str, str], bool]], None]:
    """A lock released on the path that rejoins is released (may-analysis)."""
    def merge(st: dict[tuple[str, str], bool]) -> None:
        for k, v in st.items():
            if v is False or k not in held:
                held[k] = v
    return merge


def _lock_double_lock(lines, rel, funcs, out) -> None:
    """Two acquires of the same lock with no release between them.

    Nested lock of a different mutex is not a finding (see LOCK-ORDER).
    A function that never unlocks is a contract, not a finding; a second
    acquire before the next release is CWE-764.
    """
    for fn in funcs:
        held: dict[tuple[str, str], bool] = {}
        body_lines = fn.body.splitlines()
        for i, ln in enumerate(body_lines):
            found = False
            for m in LOCK_CALL.finditer(ln):
                name, arg = m.group(1), " ".join(m.group(2).split())
                if _rx(r"(?i)unlock").search(name):
                    lock_name = _lock_of(name)
                    if lock_name:
                        held[(lock_name, arg)] = False
                    continue
                u = _unlock_of(name)
                if not u:
                    continue
                key = (name, arg)
                if held.get(key) is True:
                    line = fn.span[0] + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line,
                        cls="LOCK-DOUBLE-LOCK",
                        message=f"{name}({arg}) acquired twice without "
                        "a release between",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else ln.strip(),
                    ))
                    found = True
                    break
                held[key] = True
            if found:
                break


_MUTEX_LOCAL = re.compile(
    r"^\s*(?:static\s+)?(?:pthread_mutex_t|mtx_t)\s+"
    r"(?P<name>[A-Za-z_]\w*)(?P<init>\s*=)?",
    re.M,
)
_MUTEX_INIT_CALL = re.compile(
    r"\b(?:pthread_mutex_init|mtx_init)\s*\(\s*&?\s*([A-Za-z_]\w*)"
)
_MUTEX_LOCK_CALL = re.compile(
    r"\b(?:pthread_mutex_lock|pthread_mutex_trylock|"
    r"mtx_lock|mtx_trylock)\s*\(\s*&?\s*([A-Za-z_]\w*)"
)


def _lock_missing_init(lines, rel, funcs, out) -> None:
    """Automatic pthread_mutex_t / mtx_t locked without initializer or init.

    A mutex pointer parameter is the caller's object, not this class.
    std::mutex constructs on declaration and is not this class.
    """
    for fn in funcs:
        inited: set[str] = set()
        locals_: set[str] = set()
        for m in _lit_finditer(_MUTEX_LOCAL, fn.body):
            name = m.group("name")
            locals_.add(name)
            if m.group("init"):
                inited.add(name)
        if not locals_:
            continue
        for m in _lit_finditer(_MUTEX_INIT_CALL, fn.body):
            inited.add(m.group(1))
        start = fn.span[0]
        reported: set[str] = set()
        for i, ln in enumerate(_gated_lines(fn.body, _MUTEX_LOCK_CALL)):
            sm = _MUTEX_LOCK_CALL.search(ln)
            if not sm:
                continue
            name = sm.group(1)
            if name not in locals_ or name in inited or name in reported:
                continue
            reported.add(name)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="LOCK-MISSING-INIT",
                message=f"{name} is locked without pthread_mutex_init/"
                "mtx_init or a static initializer",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _cxx_throw_destructor(stripped: str, lines, rel, out) -> None:
    """Destructor body contains a throw statement (not a throw() specifier)."""
    reported: set[str] = set()
    for m in _lit_finditer(_DTOR_HEAD, stripped):
        name = m.group("name")
        if name in reported:
            continue
        brace = m.end() - 1
        close = _match_brace(stripped, brace)
        if close < 0:
            continue
        body = stripped[brace + 1: close]
        tm = _rx(r"\bthrow\b").search(body)
        if not tm:
            continue
        reported.add(name)
        line = stripped[: brace + 1 + tm.start()].count("\n") + 1
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=f"~{name}", line=line, cls="CXX-THROW-DESTRUCTOR",
            message=f"destructor ~{name}() throws",
            strength=laws.STRENGTH_FINDS,
            evidence=lines[line - 1].strip()
            if 0 < line <= len(lines) else "",
        ))


_CXX_THROW_SPEC = re.compile(
    r"(?P<name>[A-Za-z_]\w*)\s*\([^;{}]*\)\s*throw\s*\([^;{}]*\)\s*[{;]"
)
_THROW_SPEC_SKIP = _KW | {
    "if", "while", "for", "switch", "catch", "sizeof", "typeof",
    "alignof", "decltype", "return",
}


def _cxx_throw_spec(stripped: str, lines, rel, out) -> None:
    """Dynamic exception specification on a declarator (`throw()` / `throw(T)`).

    A throw *statement* (`throw expr;`) is not this class. `if (x) throw (e)`
    is skipped because the callee name is a keyword. `noexcept` is allowed.
    """
    reported: set[str] = set()
    for m in _lit_finditer(_CXX_THROW_SPEC, stripped):
        name = m.group("name")
        if name in _THROW_SPEC_SKIP:
            continue
        if name in reported:
            continue
        reported.add(name)
        line = stripped[: m.start()].count("\n") + 1
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=name, line=line, cls="CXX-THROW-SPEC",
            message=f"{name} uses a dynamic exception specification",
            strength=laws.STRENGTH_FINDS,
            evidence=lines[line - 1].strip()
            if 0 < line <= len(lines) else "",
        ))


_CXX_INHERIT = re.compile(
    r"\b(?:struct|class)\s+(?P<derived>[A-Za-z_]\w*)\s*:\s*"
    r"(?:(?:public|protected|private|virtual)\s+)*"
    r"(?P<base>[A-Za-z_]\w*)"
)
_CXX_LOCAL_OBJ = re.compile(
    r"^\s*(?:const\s+)?(?P<ty>[A-Za-z_]\w*)\s+(?P<name>[A-Za-z_]\w*)\s*(?:=|;)",
    re.M,
)


def _cxx_class_name(typ: str) -> str:
    t = _rx(r"\b(?:const|volatile|struct|class)\b").sub(" ", typ)
    t = t.replace("&", " ").replace("*", " ")
    return " ".join(t.split())


def _cxx_slicing(stripped: str, lines, rel, funcs, out) -> None:
    """Derived object passed to a parameter that takes the base by value.

    Base& / Base* parameters are not this class. Passing a Base as Base
    is not slicing.
    """
    inherit: dict[str, set[str]] = {}
    for m in _lit_finditer(_CXX_INHERIT, stripped):
        inherit.setdefault(m.group("derived"), set()).add(m.group("base"))
    if not inherit:
        return
    bases = {b for bs in inherit.values() for b in bs}
    slicers: dict[str, list[tuple[int, str]]] = {}
    for fn in funcs:
        hits: list[tuple[int, str]] = []
        for i, (typ, name) in enumerate(fn.params):
            if not name or "*" in typ or "&" in typ:
                continue
            t = _cxx_class_name(typ)
            if t in bases:
                hits.append((i, t))
        if hits:
            slicers[fn.name] = hits
    if not slicers:
        return
    derived_names = set(inherit)
    for fn in funcs:
        typed: dict[str, str] = {}
        for typ, name in fn.params:
            if not name or "*" in typ:
                continue
            t = _cxx_class_name(typ)
            if t in derived_names:
                typed[name] = t
        for m in _lit_finditer(_CXX_LOCAL_OBJ, fn.body):
            if m.group("ty") in derived_names:
                typed[m.group("name")] = m.group("ty")
        if not typed:
            continue
        start = fn.span[0]
        reported: set[tuple[str, int]] = set()
        for i, ln in enumerate(fn.body.splitlines()):
            for callee, positions in slicers.items():
                args = _find_call_args(ln, callee)
                if not args:
                    continue
                for idx, base in positions:
                    if idx >= len(args):
                        continue
                    arg = args[idx].strip()
                    if arg not in typed:
                        continue
                    key = (callee, i)
                    if key in reported:
                        continue
                    reported.add(key)
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="CXX-SLICING",
                        message=f"{callee}() takes {base} by value; "
                        f"{arg} is {typed[arg]}",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else ln.strip(),
                    ))


_DELETE_THIS = re.compile(r"\bdelete\s*(?:\[\s*\])?\s*this\b")


def _cxx_delete_this(lines, rel, funcs, out) -> None:
    """`delete this` / `delete[] this` while the member function still runs.

    `delete p` on another pointer is not this class.
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _DELETE_THIS)):
            if not _DELETE_THIS.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-DELETE-THIS",
                message="delete this while the member function still runs",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


_CATCH_CLAUSE = re.compile(r"\bcatch\s*\(\s*(?P<clause>[^)]+)\)")


def _catch_by_value(clause: str) -> bool:
    c = clause.strip()
    if not c or c == "...":
        return False
    return "&" not in c and "*" not in c


def _cxx_catch_by_value(lines, rel, funcs, out) -> None:
    """`catch (T x)` / `catch (T)` without `&` or `*` slices polymorphic types."""
    for fn in funcs:
        start = fn.span[0]
        reported: set[int] = set()
        for i, ln in enumerate(fn.body.splitlines()):
            for m in _CATCH_CLAUSE.finditer(ln):
                clause = m.group("clause")
                if not _catch_by_value(clause):
                    continue
                if i in reported:
                    continue
                reported.add(i)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-CATCH-BY-VALUE",
                    message=f"catch ({clause.strip()}) catches by value",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))


def _fn_promises_no_throw(signature: str) -> bool:
    m = _rx(r"\)([^({]*)").search(signature)
    if not m:
        return False
    tail = m.group(1)
    if _rx(r"\bnoexcept\b").search(tail):
        return True
    return bool(_rx(r"throw\s*\(\s*\)").search(tail))


def _cxx_throw_noexcept(lines, rel, funcs, out) -> None:
    """Function declared noexcept or throw() contains a throw statement."""
    for fn in funcs:
        if not _fn_promises_no_throw(fn.signature):
            continue
        tm = _rx(r"\bthrow\b").search(fn.body)
        if not tm:
            continue
        start = fn.span[0]
        line = start
        for i, ln in enumerate(fn.body.splitlines()):
            if _rx(r"\bthrow\b").search(ln):
                line = start + i
                break
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=fn.name, line=line, cls="CXX-THROW-NOEXCEPT",
            message=f"{fn.name} is noexcept but contains throw",
            strength=laws.STRENGTH_FINDS,
            evidence=lines[line - 1].strip()
            if 0 < line <= len(lines) else "",
        ))


_CLASS_DEF = re.compile(
    r"\b(?:class|struct)\s+(?P<name>[A-Za-z_]\w*)\b[^{;]*\{",
)
_CXX_PTR_DECL = re.compile(
    r"\b(?:const\s+)?(?P<cls>[A-Za-z_]\w*)\s*"
    r"(?P<ptr>(?:\s*\*+\s*)+)"
    r"(?P<name>[A-Za-z_]\w*)\b",
)


def _class_needs_virtual_dtor(stripped: str) -> set[str]:
    """Classes with a virtual method but no virtual destructor in the TU."""
    bad: set[str] = set()
    for m in _lit_finditer(_CLASS_DEF, stripped):
        name = m.group("name")
        brace = m.end() - 1
        close = _match_brace(stripped, brace)
        if close < 0:
            continue
        body = stripped[brace + 1: close]
        has_virt_dtor = bool(
            re.search(rf"\bvirtual\s+~(?:{re.escape(name)})\b", body)
        )
        has_virt_method = False
        for vm in _rx(r"\bvirtual\b").finditer(body):
            chunk = body[vm.start(): vm.start() + 120]
            if _rx(r"\bvirtual\s+~").search(chunk):
                continue
            if "(" in chunk:
                has_virt_method = True
                break
        if has_virt_method and not has_virt_dtor:
            bad.add(name)
    return bad


def _fn_ptr_types(fn) -> dict[str, str]:
    typed: dict[str, str] = {}
    for typ, pname in fn.params:
        if not pname or "*" not in typ:
            continue
        cls = _cxx_class_name(typ)
        if cls:
            typed[pname] = cls.split()[-1]
    for m in _lit_finditer(_CXX_PTR_DECL, fn.body):
        typed[m.group("name")] = m.group("cls")
    return typed


def _cxx_missing_virtual_dtor(stripped: str, lines, rel, funcs, out) -> None:
    """delete Base* when Base has virtual methods but no virtual ~Base."""
    bad_classes = _class_needs_virtual_dtor(stripped)
    if not bad_classes:
        return
    for fn in funcs:
        ptr_types = _fn_ptr_types(fn)
        if not ptr_types:
            continue
        start = fn.span[0]
        reported: set[str] = set()
        for i, ln in enumerate(_gated_lines(fn.body, _DELETE)):
            dm = _DELETE.search(ln)
            if not dm:
                continue
            var = dm.group("var")
            cls = ptr_types.get(var)
            if not cls or cls not in bad_classes or var in reported:
                continue
            reported.add(var)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-MISSING-VIRTUAL-DTOR",
                message=f"delete {var} ({cls}*) but {cls} lacks virtual "
                "destructor",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


_INT_PARAM_TY = re.compile(
    r"\b(?:int|long|short|unsigned|size_t|uint)\b", re.I,
)
_IGNORED_ERR = re.compile(
    r"^\s*(?:malloc|calloc|realloc|fopen|open)\s*\([^;]*\)\s*;\s*$",
)
_FD_OPEN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?"
    r"(?P<fn>fopen|open)\s*\(",
)
_FD_CLOSE = re.compile(
    r"\b(?P<fn>fclose|close)\s*\(\s*(?P<var>[A-Za-z_]\w*)\s*\)",
)
_VLA_DECL = re.compile(
    r"^\s*(?P<static>static\s+)?"
    r"(?:const\s+|volatile\s+)?"
    r"(?:unsigned\s+)?(?:char|short|int|long|float|double|"
    r"size_t|ssize_t|ptrdiff_t|int\d+_t|uint\d+_t)\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*\[\s*(?P<size>[^\]]+)\s*\]\s*;",
    re.M,
)


def _similar_param_type(a: str, b: str) -> bool:
    na = _rx(r"\s+").sub(" ", a.strip().lower())
    nb = _rx(r"\s+").sub(" ", b.strip().lower())
    return na == nb


def _integer_param_names(fn) -> list[tuple[str, str]]:
    out: list[tuple[str, str]] = []
    for typ, name in fn.params:
        if not name or "*" in typ or "[" in typ:
            continue
        if _INT_PARAM_TY.search(typ):
            out.append((name, typ))
    return out


def _param_guarded(name: str, body_lines: list[str]) -> bool:
    """Rejection-shaped `if (name …)` whose consequent returns."""
    v = re.escape(name)
    tests = (
        rf"\bif\s*\(\s*{v}\s*<\s*",
        rf"\bif\s*\(\s*{v}\s*>\s*",
        rf"\bif\s*\(\s*{v}\s*>=\s*",
        rf"\bif\s*\(\s*{v}\s*<=\s*",
        rf"\bif\s*\(\s*{v}\s*==\s*NULL\b",
    )
    for i, ln in enumerate(body_lines):
        if not any(re.search(p, ln) for p in tests):
            continue
        nxt = body_lines[i + 1] if i + 1 < len(body_lines) else ""
        if _rx(r"\breturn\b").search(ln) or _rx(r"\breturn\b").search(nxt):
            return True
    return False


def _has_if_operand(name: str, body: str) -> bool:
    v = re.escape(name)
    return bool(re.search(rf"\bif\s*\([^)]*\b{v}\b", body))


def _subscript_uses(name: str, body: str) -> bool:
    v = re.escape(name)
    if re.search(rf"\[[^\]]*\b{v}\b[^\]]*\]", body):
        return True
    return bool(re.search(rf"\b{v}\s*\[", body))


def _first_subscript_line(fn, name: str, lines: list[str]) -> int:
    v = re.escape(name)
    start, end = fn.span
    for i, ln in enumerate(lines[start - 1 : end], start):
        if re.search(rf"\[[^\]]*\b{v}\b[^\]]*\]", ln):
            return i
        if re.search(rf"\b{v}\s*\[", ln):
            return i
    return start


def _sibling_asymmetry(lines, rel, funcs, out) -> None:
    """Same integer param guarded in one function, subscripted bare in a sibling."""
    eligible = [f for f in funcs if f.kind in ("SCALAR", "VOID")]
    by_name: dict[str, list[tuple]] = {}
    for fn in eligible:
        for name, typ in _integer_param_names(fn):
            by_name.setdefault(name, []).append((fn, typ))
    reported: set[tuple[str, str]] = set()
    for pname, entries in by_name.items():
        if len(entries) < 2:
            continue
        guarders = [
            (fn, typ) for fn, typ in entries
            if _param_guarded(pname, fn.body.splitlines())
        ]
        if not guarders:
            continue
        for fn, typ in entries:
            body = fn.body
            if _has_if_operand(pname, body):
                continue
            if not _subscript_uses(pname, body):
                continue
            if not any(_similar_param_type(typ, gt) for _, gt in guarders):
                continue
            key = (fn.name, pname)
            if key in reported:
                continue
            reported.add(key)
            line = _first_subscript_line(fn, pname, lines)
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CTRL-SIBLING-ASYMMETRY",
                message=f"{pname} guarded in a sibling but used as subscript "
                f"here without a test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))


def _ignored_error(lines, rel, func_of, out) -> None:
    """Discarded malloc/calloc/realloc/fopen/open result."""
    for i, ln in enumerate(lines, 1):
        if not _IGNORED_ERR.match(ln):
            continue
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=func_of(i), line=i, cls="API-IGNORED-ERROR",
            message="allocation or open result is discarded",
            strength=laws.STRENGTH_FINDS, evidence=ln.strip(),
        ))


_SCANF_DISCARDED = re.compile(
    r"^\s*(?:scanf|sscanf|fscanf|wscanf|swscanf|fwscanf)\s*\([^;]*\)\s*;\s*$"
)


def _scanf_unchecked(lines, rel, func_of, out) -> None:
    """scanf-family conversion count discarded (not assigned or tested)."""
    for i, ln in enumerate(lines, 1):
        if not _SCANF_DISCARDED.match(ln):
            continue
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=func_of(i), line=i, cls="API-SCANF-UNCHECKED",
            message="scanf-family return is discarded",
            strength=laws.STRENGTH_FINDS, evidence=ln.strip(),
        ))


_GETS_CALL = re.compile(r"(?<![A-Za-z0-9_])gets\s*\(")


def _api_gets(lines, rel, funcs, out) -> None:
    """gets() is unbounded and removed from C11. fgets/gets_s are not this.

    A prototype `char *gets(char *);` is not a call.
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETS_CALL)):
            if not _GETS_CALL.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETS",
                message="gets() is unbounded (removed from C11)",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


_STRTOK_CALL = re.compile(r"(?<![A-Za-z0-9_])strtok\s*\(")


def _api_strtok(lines, rel, funcs, out) -> None:
    """strtok() keeps a static cursor. strtok_r / strtok_s are not this.

    A prototype `char *strtok(char *, const char *);` is not a call.
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _STRTOK_CALL)):
            if not _STRTOK_CALL.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-STRTOK-REENTRANT",
                message="strtok() is not reentrant; strtok_r/strtok_s keep no static cursor",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_mkstemp(lines, rel, funcs, out) -> None:
    """mkstemp/mkstemps template must be a writable array. A string literal is not.

    `char t[] = "/tmp/xXXXXXX"; mkstemp(t)` is the honest call. A prototype
    is not a call.
    """
    for fn in funcs:
        start = fn.span[0]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(fn.body.splitlines()):
            for fname in ("mkstemp", "mkstemps"):
                args = _find_call_args(ln, fname)
                if not args:
                    continue
                if not _is_string_literal(args[0]):
                    continue
                key = (fname, i)
                if key in seen:
                    continue
                seen.add(key)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="API-MKSTEMP",
                    message=f"{fname}() template is a string literal (not writable)",
                    strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


_TMPNAM_CALL = re.compile(r"(?<![A-Za-z0-9_])(?:tmpnam_r|tempnam|tmpnam)\s*\(")


def _api_tmpnam(lines, rel, funcs, out) -> None:
    """tmpnam/tempnam/tmpnam_r create predictable temp paths (CWE-377).

    mkstemp/mkdtemp on a writable template is the honest API. A prototype
    is not a call.
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _TMPNAM_CALL)):
            if not _TMPNAM_CALL.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-TMPNAM",
                message="tmpnam/tempnam is predictable; use mkstemp/mkdtemp",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


_MKTEMP_CALL = re.compile(r"(?<![A-Za-z0-9_])mktemp\s*\(")


def _api_mktemp(lines, rel, funcs, out) -> None:
    """mktemp() mutates a template in place and is predictable (CWE-377).

    mkstemp() on a writable array is the honest replacement. Distinct from
    API-MKSTEMP (literal template) and API-TMPNAM.
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MKTEMP_CALL)):
            if not _MKTEMP_CALL.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MKTEMP",
                message="mktemp() is insecure; use mkstemp/mkdtemp",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_signal(lines, rel, funcs, out) -> None:
    """signal() to install a handler; POSIX prefers sigaction() (CWE-479).

    SIG_DFL and SIG_IGN are simple dispositions, not handler installs.
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(fn.body.splitlines()):
            args = _find_call_args(ln, "signal")
            if not args or len(args) < 2:
                continue
            handler = args[1].strip()
            if handler in ("SIG_DFL", "SIG_IGN"):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SIGNAL",
                message="signal() installs a handler; prefer sigaction()",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _fmt_percent_n(lines, rel, funcs, out) -> None:
    """printf-family with %n in a string-literal format (write-back specifier)."""
    for fn in funcs:
        start = fn.span[0]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(fn.body.splitlines()):
            for fname, idx in _FMT_FN.items():
                args = _find_call_args(ln, fname)
                if args is None or len(args) <= idx:
                    continue
                fmt = args[idx].strip()
                if not _literal_has_percent_n(fmt):
                    continue
                key = (fname, i)
                if key in seen:
                    continue
                seen.add(key)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="FMT-PERCENT-N",
                    message=f"{fname}() format uses %n write-back",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))


_GETENV_USE_CALLEES: dict[str, list[int]] = {
    "strlen": [0],
    "strcpy": [1],
    "strcat": [1],
    "strcmp": [0, 1],
    "atoi": [0],
    "atol": [0],
    "atoll": [0],
}
_GETENV_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?"
    r"(?:secure_)?getenv\s*\(",
)


def _fmt_literal_has_percent_s(s: str) -> bool:
    """True when a string-literal format contains %s (not %%s)."""
    inner = _string_literal_inner(s)
    if inner is None:
        return False
    i = 0
    while i < len(inner):
        if inner[i] == "%":
            if i + 1 < len(inner) and inner[i + 1] == "%":
                i += 2
                continue
            if i + 1 < len(inner) and inner[i + 1] == "s":
                return True
        i += 1
    return False


def _getenv_assign_vars(ln: str) -> list[str]:
    if not _rx(r"\b(?:secure_)?getenv\s*\(").search(ln):
        return []
    m = _GETENV_ASSIGN.search(ln)
    return [m.group("var")] if m else []


def _getenv_in_truthy_if(ln: str, var: str) -> bool:
    v = re.escape(var)
    return bool(re.search(
        rf"\bif\s*\([^)]*\b{v}\s*=\s*(?:secure_)?getenv\s*\(",
        ln,
    ))


def _getenv_uses(var: str, ln: str) -> bool:
    v = re.escape(var)
    if re.search(rf"\breturn\s+{v}\s*;", ln):
        return False
    if re.search(rf"\b{v}\s*\[", ln):
        return True
    if re.search(rf"\*\s*{v}\b", ln):
        return True
    for callee, idxs in _GETENV_USE_CALLEES.items():
        args = _find_call_args(ln, callee)
        if not args:
            continue
        for idx in idxs:
            if idx < len(args) and args[idx].strip() == var:
                return True
    for fname, fmt_idx in _FMT_FN.items():
        args = _find_call_args(ln, fname)
        if args is None or len(args) <= fmt_idx:
            continue
        if not _fmt_literal_has_percent_s(args[fmt_idx]):
            continue
        for arg in args[fmt_idx + 1:]:
            if arg.strip() == var:
                return True
    return False


def _getenv_only_returned(var: str, chunk: list[str], assign_i: int) -> bool:
    v = re.escape(var)
    saw_return = False
    for j in range(assign_i + 1, len(chunk)):
        ln = chunk[j]
        if _getenv_uses(var, ln):
            return False
        if _rx(r"\breturn\b").search(ln):
            if re.search(rf"\breturn\s+{v}\s*;", ln):
                saw_return = True
            else:
                return False
    return saw_return


def _api_getenv_null(lines, rel, funcs, out) -> None:
    """getenv()/secure_getenv() result dereferenced or passed to string APIs unchecked.

    system()/popen() sinks are TAINT-SINK; returning the pointer alone is fine.
    """
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            for var in _getenv_assign_vars(ln):
                key = (var, i)
                if key in seen:
                    continue
                if _getenv_in_truthy_if(ln, var):
                    continue
                if _getenv_only_returned(var, chunk, i):
                    continue
                use_j = None
                for j in range(i + 1, len(chunk)):
                    if _getenv_uses(var, chunk[j]):
                        use_j = j
                        break
                if use_j is None:
                    continue
                between = "\n".join(chunk[i:use_j])
                if _null_test_for(var, between):
                    continue
                seen.add(key)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="API-GETENV-NULL",
                    message=f"{var} from getenv() used without a NULL test",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))


_STRDUP_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?"
    r"strn?dup\s*\(",
)


def _strdup_assign_vars(ln: str) -> list[str]:
    if not _rx(r"\bstrn?dup\s*\(").search(ln):
        return []
    m = _STRDUP_ASSIGN.search(ln)
    return [m.group("var")] if m else []


def _strdup_in_truthy_if(ln: str, var: str) -> bool:
    v = re.escape(var)
    return bool(re.search(
        rf"\bif\s*\([^)]*\b{v}\s*=\s*strn?dup\s*\(",
        ln,
    ))


def _strdup_only_returned(var: str, chunk: list[str], assign_i: int) -> bool:
    v = re.escape(var)
    saw_return = False
    for j in range(assign_i + 1, len(chunk)):
        ln = chunk[j]
        if _getenv_uses(var, ln):
            return False
        if _rx(r"\breturn\b").search(ln):
            if re.search(rf"\breturn\s+{v}\s*;", ln):
                saw_return = True
            else:
                return False
    return saw_return


def _api_strdup_null(lines, rel, funcs, out) -> None:
    """strdup()/strndup() result used without a NULL test (CWE-690).

    Distinct from API-GETENV-NULL and PTR-UNCHECKED-ALLOC (malloc family).
    """
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            for var in _strdup_assign_vars(ln):
                key = (var, i)
                if key in seen:
                    continue
                if _strdup_in_truthy_if(ln, var):
                    continue
                if _strdup_only_returned(var, chunk, i):
                    continue
                use_j = None
                for j in range(i + 1, len(chunk)):
                    if _getenv_uses(var, chunk[j]):
                        use_j = j
                        break
                if use_j is None:
                    continue
                between = "\n".join(chunk[i:use_j])
                if _null_test_for(var, between):
                    continue
                seen.add(key)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="API-STRDUP-NULL",
                    message=f"{var} from strdup() used without a NULL test",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))


def _api_system(lines, rel, funcs, out) -> None:
    """system() with a non-literal command string (CWE-78).

    system("literal") is not this lint; taint tracks getenv→system separately.
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(fn.body.splitlines()):
            args = _find_call_args(ln, "system")
            if not args:
                continue
            if _is_string_literal(args[0]):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SYSTEM",
                message="system() command is not a string literal",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


_CHROOT_CALL = re.compile(r"(?<![A-Za-z0-9_])chroot\s*\(")
_UMASK_CALL = re.compile(r"(?<![A-Za-z0-9_])umask\s*\(")
_SRAND_TIME = re.compile(
    r"\b(?:srand|srandom)\s*\(\s*(?:\([^)]*\)\s*)*time\s*\(",
)


def _umask_arg_unsafe(arg: str) -> bool:
    """True only for umask(0) / umask(00); identifiers and 077/0x3f are safe."""
    lit = _parse_int_literal(arg.strip())
    return lit is not None and lit == 0


def _api_umask(lines, rel, funcs, out) -> None:
    """umask(0) leaves a world-writable default file-creation mask (CWE-732)."""
    for fn in funcs:
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            if not _UMASK_CALL.search(ln):
                continue
            args = _find_call_args(ln, "umask")
            if not args or len(args) != 1:
                continue
            if not _umask_arg_unsafe(args[0]):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-UMASK",
                message="umask(0) leaves world-writable default",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


_FORK_DISCARDED = re.compile(
    r"^\s*(?:vfork|fork)\s*\([^;]*\)\s*;\s*$"
)
_FORK_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?:vfork|fork)\s*\("
)
_EXEC_FNS = ("execlp", "execle", "execl", "execvpe", "execvp", "execve", "execv")
_MMAP_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?mmap\s*\("
)
_GETCWD_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?getcwd\s*\("
)
_IOCTL_DISCARDED = re.compile(
    r"^\s*ioctl\s*\([^;]*\)\s*;\s*$"
)
_SETUID_DISCARDED = re.compile(
    r"^\s*(?:setuid|seteuid|setgid)\s*\([^;]*\)\s*;\s*$"
)
_SOCKET_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?socket\s*\("
)
_BIND_DISCARDED = re.compile(
    r"^\s*bind\s*\([^;]*\)\s*;\s*$"
)
_LISTEN_DISCARDED = re.compile(
    r"^\s*listen\s*\([^;]*\)\s*;\s*$"
)
_CONNECT_DISCARDED = re.compile(
    r"^\s*connect\s*\([^;]*\)\s*;\s*$"
)
_PIPE_DISCARDED = re.compile(
    r"^\s*(?:pipe2|pipe)\s*\([^;]*\)\s*;\s*$"
)
_PIPE_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?:pipe2|pipe)\s*\("
)
_DUP_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?:dup3|dup2|dup)\s*\("
)
_FCNTL_DISCARDED = re.compile(
    r"^\s*fcntl\s*\([^;]*\)\s*;\s*$"
)
_WAIT_DISCARDED = re.compile(
    r"^\s*(?:waitpid|waitid|wait)\s*\([^;]*\)\s*;\s*$"
)
_SELECT_DISCARDED = re.compile(
    r"^\s*(?:epoll_wait|select|poll)\s*\([^;]*\)\s*;\s*$"
)
_SEND_DISCARDED = re.compile(
    r"^\s*(?:sendto|recvfrom|send|recv)\s*\([^;]*\)\s*;\s*$"
)
_SHUTDOWN_DISCARDED = re.compile(
    r"^\s*shutdown\s*\([^;]*\)\s*;\s*$"
)
_KILL_DISCARDED = re.compile(
    r"^\s*(?:kill|raise)\s*\([^;]*\)\s*;\s*$"
)
_GETADDRINFO_DISCARDED = re.compile(
    r"^\s*getaddrinfo\s*\([^;]*\)\s*;\s*$"
)
_GETADDRINFO_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?getaddrinfo\s*\("
)
_PTHREAD_JOIN_DISCARDED = re.compile(
    r"^\s*(?:pthread_join|pthread_detach)\s*\([^;]*\)\s*;\s*$"
)
_THRD_JOIN_DISCARDED = re.compile(
    r"^\s*(?:thrd_join|thrd_detach)\s*\([^;]*\)\s*;\s*$"
)
_SEM_WAIT_DISCARDED = re.compile(
    r"^\s*(?:sem_wait|sem_post)\s*\([^;]*\)\s*;\s*$"
)
_OPENAT_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?openat\s*\("
)
_FLOCK_DISCARDED = re.compile(
    r"^\s*flock\s*\([^;]*\)\s*;\s*$"
)
_CHOWN_DISCARDED = re.compile(
    r"^\s*(?:fchown|lchown|chown)\s*\([^;]*\)\s*;\s*$"
)
_SYMLINK_DISCARDED = re.compile(
    r"^\s*(?:symlink|readlink)\s*\([^;]*\)\s*;\s*$"
)
_OPENDIR_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?opendir\s*\("
)
_SETRLIMIT_DISCARDED = re.compile(
    r"^\s*(?:setrlimit|getrlimit)\s*\([^;]*\)\s*;\s*$"
)
_GETSOCKOPT_DISCARDED = re.compile(
    r"^\s*(?:getsockopt|setsockopt)\s*\([^;]*\)\s*;\s*$"
)
_STAT_DISCARDED = re.compile(
    r"^\s*(?:lstat|fstat|stat)\s*\([^;]*\)\s*;\s*$"
)
_MKDIR_DISCARDED = re.compile(
    r"^\s*(?:mkdir|rmdir)\s*\([^;]*\)\s*;\s*$"
)
_GETPWUID_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?"
    r"(?:getpwuid|getpwnam)\s*\("
)
_CLOCK_GETTIME_DISCARDED = re.compile(
    r"^\s*(?:clock_gettime|gettimeofday)\s*\([^;]*\)\s*;\s*$"
)
_SHM_OPEN_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?shm_open\s*\("
)
_POSIX_SPAWN_DISCARDED = re.compile(
    r"^\s*(?:posix_spawnp|posix_spawn)\s*\([^;]*\)\s*;\s*$"
)
_GLOB_DISCARDED = re.compile(
    r"^\s*glob\s*\([^;]*\)\s*;\s*$"
)
_FSEEK_DISCARDED = re.compile(
    r"^\s*(?:fseek|ftell)\s*\([^;]*\)\s*;\s*$"
)
_ACCESS_DISCARDED = re.compile(
    r"^\s*access\s*\([^;]*\)\s*;\s*$"
)
_GETOPT_DISCARDED = re.compile(
    r"^\s*(?:getopt_long|getopt)\s*\([^;]*\)\s*;\s*$"
)
_UNAME_DISCARDED = re.compile(
    r"^\s*(?:uname|gethostname)\s*\([^;]*\)\s*;\s*$"
)
_SENDFILE_DISCARDED = re.compile(
    r"^\s*(?:sendfile|copy_file_range)\s*\([^;]*\)\s*;\s*$"
)
_MEMFD_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?memfd_create\s*\("
)
_PRCTL_DISCARDED = re.compile(
    r"^\s*(?:prctl|ptrace)\s*\([^;]*\)\s*;\s*$"
)
_TCGETATTR_DISCARDED = re.compile(
    r"^\s*(?:tcsetattr|tcgetattr)\s*\([^;]*\)\s*;\s*$"
)
_SYSCONF_DISCARDED = re.compile(
    r"^\s*(?:pathconf|sysconf)\s*\([^;]*\)\s*;\s*$"
)
_GETRUSAGE_DISCARDED = re.compile(
    r"^\s*getrusage\s*\([^;]*\)\s*;\s*$"
)
_NFTW_DISCARDED = re.compile(
    r"^\s*(?:nftw|ftw)\s*\([^;]*\)\s*;\s*$"
)
_WORDEXP_DISCARDED = re.compile(
    r"^\s*wordexp\s*\([^;]*\)\s*;\s*$"
)
_GETLOGIN_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?"
    r"(?:getlogin|ttyname)\s*\("
)
_INET_PTON_DISCARDED = re.compile(
    r"^\s*(?:inet_pton|inet_aton)\s*\([^;]*\)\s*;\s*$"
)
_MLOCK_DISCARDED = re.compile(
    r"^\s*(?:munlock|mlock)\s*\([^;]*\)\s*;\s*$"
)
_SPLICE_DISCARDED = re.compile(
    r"^\s*splice\s*\([^;]*\)\s*;\s*$"
)
_INOTIFY_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?"
    r"(?:inotify_init1|inotify_init)\s*\("
)
_FSYNC_DISCARDED = re.compile(
    r"^\s*(?:fdatasync|fsync)\s*\([^;]*\)\s*;\s*$"
)
_GETRANDOM_DISCARDED = re.compile(
    r"^\s*(?:getrandom|getentropy)\s*\([^;]*\)\s*;\s*$"
)
_GETLINE_DISCARDED = re.compile(
    r"^\s*(?:getdelim|getline)\s*\([^;]*\)\s*;\s*$"
)
_ASPRINTF_DISCARDED = re.compile(
    r"^\s*(?:vasprintf|asprintf)\s*\([^;]*\)\s*;\s*$"
)
_STRLCPY_DISCARDED = re.compile(
    r"^\s*(?:strlcpy|strlcat)\s*\([^;]*\)\s*;\s*$"
)
_ISATTY_DISCARDED = re.compile(
    r"^\s*isatty\s*\([^;]*\)\s*;\s*$"
)
_PTSNAME_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?ptsname\s*\("
)
_MOUNT_DISCARDED = re.compile(
    r"^\s*(?:umount|mount)\s*\([^;]*\)\s*;\s*$"
)
_FMEMOPEN_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?"
    r"(?:open_memstream|fmemopen)\s*\("
)
_SCANDIR_DISCARDED = re.compile(
    r"^\s*scandir\s*\([^;]*\)\s*;\s*$"
)
_SETXATTR_DISCARDED = re.compile(
    r"^\s*(?:listxattr|getxattr|setxattr)\s*\([^;]*\)\s*;\s*$"
)
_SCHED_AFFINITY_DISCARDED = re.compile(
    r"^\s*(?:sched_setaffinity|sched_getaffinity)\s*\([^;]*\)\s*;\s*$"
)
_AIO_DISCARDED = re.compile(
    r"^\s*(?:aio_read|aio_write)\s*\([^;]*\)\s*;\s*$"
)
_STATX_DISCARDED = re.compile(
    r"^\s*statx\s*\([^;]*\)\s*;\s*$"
)
_PIDFD_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?pidfd_open\s*\("
)
_CAPSET_DISCARDED = re.compile(
    r"^\s*(?:capset|capget)\s*\([^;]*\)\s*;\s*$"
)
_FANOTIFY_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?fanotify_init\s*\("
)
_SECCOMP_DISCARDED = re.compile(
    r"^\s*seccomp\s*\([^;]*\)\s*;\s*$"
)
_GETGRNAM_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?"
    r"(?:getgrnam|getgrgid|getspnam)\s*\("
)
_FALLOCATE_DISCARDED = re.compile(
    r"^\s*(?:posix_fallocate|fallocate)\s*\([^;]*\)\s*;\s*$"
)
_CLOSE_RANGE_DISCARDED = re.compile(
    r"^\s*close_range\s*\([^;]*\)\s*;\s*$"
)
_BPF_DISCARDED = re.compile(
    r"^\s*bpf\s*\([^;]*\)\s*;\s*$"
)
_USERFAULTFD_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?userfaultfd\s*\("
)
_GETPASS_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?getpass\s*\("
)
_INITGROUPS_DISCARDED = re.compile(
    r"^\s*(?:initgroups|setgroups)\s*\([^;]*\)\s*;\s*$"
)
_CLONE_DISCARDED = re.compile(
    r"^\s*(?:clone|unshare|setns)\s*\([^;]*\)\s*;\s*$"
)
_OPENAT2_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?openat2\s*\("
)
_LANDLOCK_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?landlock_create_ruleset\s*\("
)
_GETPRIORITY_DISCARDED = re.compile(
    r"^\s*(?:getpriority|setpriority)\s*\([^;]*\)\s*;\s*$"
)
_SIGNALFD_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?signalfd\s*\("
)
_SENDMMSG_DISCARDED = re.compile(
    r"^\s*(?:sendmmsg|recvmmsg)\s*\([^;]*\)\s*;\s*$"
)
_PERSONALITY_DISCARDED = re.compile(
    r"^\s*personality\s*\([^;]*\)\s*;\s*$"
)
_QUOTACTL_DISCARDED = re.compile(
    r"^\s*quotactl\s*\([^;]*\)\s*;\s*$"
)
_NAME_TO_HANDLE_DISCARDED = re.compile(
    r"^\s*(?:name_to_handle_at|open_by_handle_at)\s*\([^;]*\)\s*;\s*$"
)
_PROCESS_MADVISE_DISCARDED = re.compile(
    r"^\s*process_madvise\s*\([^;]*\)\s*;\s*$"
)
_PIVOT_ROOT_DISCARDED = re.compile(
    r"^\s*pivot_root\s*\([^;]*\)\s*;\s*$"
)
_STATFS_DISCARDED = re.compile(
    r"^\s*\b(?:fstatfs|statfs)\s*\([^;]*\)\s*;\s*$"
)
_PRLIMIT_DISCARDED = re.compile(
    r"^\s*(?:prlimit64|prlimit)\s*\([^;]*\)\s*;\s*$"
)
_PERF_EVENT_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?perf_event_open\s*\("
)
_MEMBARRIER_DISCARDED = re.compile(
    r"^\s*membarrier\s*\([^;]*\)\s*;\s*$"
)
_PKEY_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?pkey_alloc\s*\("
)
_SYNCFS_DISCARDED = re.compile(
    r"^\s*\bsyncfs\s*\([^;]*\)\s*;\s*$"
)
_PROCESS_VM_DISCARDED = re.compile(
    r"^\s*(?:process_vm_readv|process_vm_writev)\s*\([^;]*\)\s*;\s*$"
)
_CLONE3_DISCARDED = re.compile(
    r"^\s*\bclone3\s*\([^;]*\)\s*;\s*$"
)
_FUTEX_DISCARDED = re.compile(
    r"^\s*futex\s*\([^;]*\)\s*;\s*$"
)
_KEYCTL_DISCARDED = re.compile(
    r"^\s*keyctl\s*\([^;]*\)\s*;\s*$"
)
_KCMP_DISCARDED = re.compile(
    r"^\s*kcmp\s*\([^;]*\)\s*;\s*$"
)
_FSOPEN_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?\bfsopen\s*\("
)
_MQ_OPEN_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?\bmq_open\s*\("
)
_SHMGET_DISCARDED = re.compile(
    r"^\s*shmget\s*\([^;]*\)\s*;\s*$"
)
_SHMGET_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?\bshmget\s*\("
)
_REBOOT_DISCARDED = re.compile(
    r"^\s*reboot\s*\([^;]*\)\s*;\s*$"
)
_ADJTIMEX_DISCARDED = re.compile(
    r"^\s*\badjtimex\s*\([^;]*\)\s*;\s*$"
)
_SETHOSTNAME_DISCARDED = re.compile(
    r"^\s*sethostname\s*\([^;]*\)\s*;\s*$"
)
_SWAPON_DISCARDED = re.compile(
    r"^\s*\b(?:swapon|swapoff)\s*\([^;]*\)\s*;\s*$"
)
_ACCT_DISCARDED = re.compile(
    r"^\s*\bacct\s*\([^;]*\)\s*;\s*$"
)
_IOPERM_DISCARDED = re.compile(
    r"^\s*\b(?:ioperm|iopl)\s*\([^;]*\)\s*;\s*$"
)
_MINCORE_DISCARDED = re.compile(
    r"^\s*\bmincore\s*\([^;]*\)\s*;\s*$"
)
_RSEQ_DISCARDED = re.compile(
    r"^\s*\brseq\s*\([^;]*\)\s*;\s*$"
)
_TIMER_CREATE_DISCARDED = re.compile(
    r"^\s*\btimer_create\s*\([^;]*\)\s*;\s*$"
)
_SEMGET_DISCARDED = re.compile(
    r"^\s*\bsemget\s*\([^;]*\)\s*;\s*$"
)
_SEMGET_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?\bsemget\s*\("
)
_MSGGET_DISCARDED = re.compile(
    r"^\s*\bmsgget\s*\([^;]*\)\s*;\s*$"
)
_MSGGET_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?\bmsgget\s*\("
)
_KLOGCTL_DISCARDED = re.compile(
    r"^\s*\bklogctl\s*\([^;]*\)\s*;\s*$"
)
_MOUNT_SETATTR_DISCARDED = re.compile(
    r"^\s*\bmount_setattr\s*\([^;]*\)\s*;\s*$"
)
_GETCPU_DISCARDED = re.compile(
    r"^\s*\bgetcpu\s*\([^;]*\)\s*;\s*$"
)
_PROCESS_MRELEASE_DISCARDED = re.compile(
    r"^\s*\bprocess_mrelease\s*\([^;]*\)\s*;\s*$"
)
_MEMFD_SECRET_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?\bmemfd_secret\s*\("
)
_IOPRIO_DISCARDED = re.compile(
    r"^\s*\b(?:ioprio_set|ioprio_get)\s*\([^;]*\)\s*;\s*$"
)
_INIT_MODULE_DISCARDED = re.compile(
    r"^\s*\b(?:init_module|finit_module|delete_module)\s*\([^;]*\)\s*;\s*$"
)
_KEXEC_DISCARDED = re.compile(
    r"^\s*\b(?:kexec_load|kexec_file_load)\s*\([^;]*\)\s*;\s*$"
)
_QUOTACTL_FD_DISCARDED = re.compile(
    r"^\s*\bquotactl_fd\s*\([^;]*\)\s*;\s*$"
)
_PKEY_FREE_DISCARDED = re.compile(
    r"^\s*\b(?:pkey_free|pkey_mprotect)\s*\([^;]*\)\s*;\s*$"
)
_TGKILL_DISCARDED = re.compile(
    r"^\s*\btgkill\s*\([^;]*\)\s*;\s*$"
)
_ADD_KEY_DISCARDED = re.compile(
    r"^\s*\badd_key\s*\([^;]*\)\s*;\s*$"
)
_SEMCTL_DISCARDED = re.compile(
    r"^\s*\bsemctl\s*\([^;]*\)\s*;\s*$"
)
_MSGCTL_DISCARDED = re.compile(
    r"^\s*\bmsgctl\s*\([^;]*\)\s*;\s*$"
)
_IO_SETUP_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>io_setup|io_destroy|io_cancel|io_pgetevents)\s*\([^;]*\)\s*;\s*$"
)
_REQUEST_KEY_DISCARDED = re.compile(
    r"^\s*\brequest_key\s*\([^;]*\)\s*;\s*$"
)
_TKILL_DISCARDED = re.compile(
    r"^\s*\btkill\s*\([^;]*\)\s*;\s*$"
)
_TIMER_DELETE_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>timer_delete|timer_gettime|timer_getoverrun)\s*\([^;]*\)\s*;\s*$"
)
_MQ_UNLINK_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>mq_unlink|mq_timedsend|mq_timedreceive|mq_notify|mq_getsetattr)\s*\([^;]*\)\s*;\s*$"
)
_SHMAT_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>shmat|shmdt)\s*\([^;]*\)\s*;\s*$"
)
_SEMOP_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>semop|semtimedop)\s*\([^;]*\)\s*;\s*$"
)
_MSGSND_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>msgsnd|msgrcv)\s*\([^;]*\)\s*;\s*$"
)
_SYNC_FILE_RANGE_DISCARDED = re.compile(
    r"^\s*\bsync_file_range\s*\([^;]*\)\s*;\s*$"
)
_MSYNC_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>msync|mremap)\s*\([^;]*\)\s*;\s*$"
)
_SOCKETPAIR_DISCARDED = re.compile(
    r"^\s*\bsocketpair\s*\([^;]*\)\s*;\s*$"
)
_SYSINFO_DISCARDED = re.compile(
    r"^\s*\bsysinfo\s*\([^;]*\)\s*;\s*$"
)
_CLOCK_SETTIME_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>clock_settime|clock_adjtime|clock_nanosleep)\s*\([^;]*\)\s*;\s*$"
)
_SETTIMEOFDAY_DISCARDED = re.compile(
    r"^\s*\bsettimeofday\s*\([^;]*\)\s*;\s*$"
)
_GETTID_DISCARDED = re.compile(
    r"^\s*\bgettid\s*\([^;]*\)\s*;\s*$"
)
_SCHED_SETSCHEDULER_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>sched_setscheduler|sched_getscheduler|sched_setparam|sched_getparam)\s*\([^;]*\)\s*;\s*$"
)
_SETITIMER_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>setitimer|getitimer)\s*\([^;]*\)\s*;\s*$"
)
_NICE_DISCARDED = re.compile(
    r"^\s*\bnice\s*\([^;]*\)\s*;\s*$"
)
_ARCH_PRCTL_DISCARDED = re.compile(
    r"^\s*\barch_prctl\s*\([^;]*\)\s*;\s*$"
)
_GETDENTS_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>getdents(?:64)?)\s*\([^;]*\)\s*;\s*$"
)
_UTIMENSAT_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>utimensat|futimens|utimes)\s*\([^;]*\)\s*;\s*$"
)
_LINKAT_DISCARDED = re.compile(
    r"^\s*\blinkat\s*\([^;]*\)\s*;\s*$"
)
_MBIND_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>mbind|set_mempolicy|get_mempolicy)\s*\([^;]*\)\s*;\s*$"
)
_FUTEX_WAITV_DISCARDED = re.compile(
    r"^\s*\bfutex_waitv\s*\([^;]*\)\s*;\s*$"
)
_SYSLOG_DISCARDED = re.compile(
    r"^\s*\bsyslog\s*\([^;]*\)\s*;\s*$"
)
_SETPGID_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>setpgid|setsid|getsid)\s*\([^;]*\)\s*;\s*$"
)
_SETREUID_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>setreuid|setregid|setresuid|setresgid)\s*\([^;]*\)\s*;\s*$"
)
_GETGROUPS_DISCARDED = re.compile(
    r"^\s*\bgetgroups\s*\([^;]*\)\s*;\s*$"
)
_EPOLL_CREATE_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>epoll_create(?:1)?)\s*\([^;]*\)\s*;\s*$"
)
_TIMERFD_SETTIME_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>timerfd_settime|timerfd_gettime)\s*\([^;]*\)\s*;\s*$"
)
_REMAP_FILE_PAGES_DISCARDED = re.compile(
    r"^\s*\bremap_file_pages\s*\([^;]*\)\s*;\s*$"
)
_MOVE_PAGES_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>migrate_pages|move_pages)\s*\([^;]*\)\s*;\s*$"
)
_CACHESTAT_DISCARDED = re.compile(
    r"^\s*\bcachestat\s*\([^;]*\)\s*;\s*$"
)
_MAP_SHADOW_STACK_DISCARDED = re.compile(
    r"^\s*\bmap_shadow_stack\s*\([^;]*\)\s*;\s*$"
)
_SCHED_YIELD_DISCARDED = re.compile(
    r"^\s*\bsched_yield\s*\([^;]*\)\s*;\s*$"
)
_SETFSUID_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>setfsuid|setfsgid)\s*\([^;]*\)\s*;\s*$"
)
_WAIT4_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>wait[34])\s*\([^;]*\)\s*;\s*$"
)
_PREADV_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>preadv2|pwritev2|preadv|pwritev)\s*\([^;]*\)\s*;\s*$"
)
_SENDMSG_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>sendmsg|recvmsg)\s*\([^;]*\)\s*;\s*$"
)
_GETSOCKNAME_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>getsockname|getpeername)\s*\([^;]*\)\s*;\s*$"
)
_EPOLL_PWAIT_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>epoll_pwait2|epoll_pwait)\s*\([^;]*\)\s*;\s*$"
)
_INOTIFY_RM_WATCH_DISCARDED = re.compile(
    r"^\s*\binotify_rm_watch\s*\([^;]*\)\s*;\s*$"
)
_EVENTFD_RW_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>eventfd_read|eventfd_write)\s*\([^;]*\)\s*;\s*$"
)
_SCHED_SETATTR_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>sched_setattr|sched_getattr)\s*\([^;]*\)\s*;\s*$"
)
_RENAMEAT2_DISCARDED = re.compile(
    r"^\s*\brenameat2\s*\([^;]*\)\s*;\s*$"
)
_EXECVEAT_DISCARDED = re.compile(
    r"^\s*\bexecveat\s*\([^;]*\)\s*;\s*$"
)
_MLOCK2_DISCARDED = re.compile(
    r"^\s*\bmlock2\s*\([^;]*\)\s*;\s*$"
)
_FACCESSAT2_DISCARDED = re.compile(
    r"^\s*\bfaccessat2\s*\([^;]*\)\s*;\s*$"
)
_POSIX_FADVISE_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>posix_fadvise(?:64)?)\s*\([^;]*\)\s*;\s*$"
)
_READAHEAD_DISCARDED = re.compile(
    r"^\s*\breadahead\s*\([^;]*\)\s*;\s*$"
)
_SIGACTION_DISCARDED = re.compile(
    r"^\s*\bsigaction\s*\([^;]*\)\s*;\s*$"
)
_SIGPROCMASK_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>sig(?:procmask|suspend))\s*\([^;]*\)\s*;\s*$"
)
_SEM_OPEN_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>sem_(?:open|close|unlink))\s*\([^;]*\)\s*;\s*$"
)
_RWLOCK_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>pthread_rwlock_\w+)\s*\([^;]*\)\s*;\s*$"
)
_PTHREAD_COND_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>pthread_cond_\w+)\s*\([^;]*\)\s*;\s*$"
)
_SIGALTSTACK_DISCARDED = re.compile(
    r"^\s*\bsigaltstack\s*\([^;]*\)\s*;\s*$"
)
_RENAMEAT_DISCARDED = re.compile(
    r"^\s*\brenameat(?!2)\s*\([^;]*\)\s*;\s*$"
)
_FACCESSAT_DISCARDED = re.compile(
    r"^\s*\bfaccessat(?!2)\s*\([^;]*\)\s*;\s*$"
)
_FCHMODAT_DISCARDED = re.compile(
    r"^\s*\bfchmodat(?!2)\s*\([^;]*\)\s*;\s*$"
)
_PTHREAD_BARRIER_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>pthread_barrier_\w+)\s*\([^;]*\)\s*;\s*$"
)
_SYMLINKAT_DISCARDED = re.compile(
    r"^\s*\bsymlinkat\s*\([^;]*\)\s*;\s*$"
)
_UNLINKAT_DISCARDED = re.compile(
    r"^\s*\bunlinkat\s*\([^;]*\)\s*;\s*$"
)
_MKDIRAT_DISCARDED = re.compile(
    r"^\s*\bmkdirat\s*\([^;]*\)\s*;\s*$"
)
_MKNODAT_DISCARDED = re.compile(
    r"^\s*\bmknodat\s*\([^;]*\)\s*;\s*$"
)
_READLINKAT_DISCARDED = re.compile(
    r"^\s*\breadlinkat\s*\([^;]*\)\s*;\s*$"
)
_FSTATAT_DISCARDED = re.compile(
    r"^\s*\bfstatat\s*\([^;]*\)\s*;\s*$"
)
_PTHREAD_SPIN_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>pthread_spin_\w+)\s*\([^;]*\)\s*;\s*$"
)
_PTHREAD_KEY_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>pthread_(?:key_create|key_delete|setspecific|getspecific))\s*\([^;]*\)\s*;\s*$"
)
_PTHREAD_CANCEL_DISCARDED = re.compile(
    r"^\s*\bpthread_cancel\s*\([^;]*\)\s*;\s*$"
)
_PTHREAD_KILL_DISCARDED = re.compile(
    r"^\s*\bpthread_kill\s*\([^;]*\)\s*;\s*$"
)
_PTHREAD_SIGMASK_DISCARDED = re.compile(
    r"^\s*\bpthread_sigmask\s*\([^;]*\)\s*;\s*$"
)
_PTHREAD_ATFORK_DISCARDED = re.compile(
    r"^\s*\bpthread_atfork\s*\([^;]*\)\s*;\s*$"
)
_PLEDGE_DISCARDED = re.compile(
    r"^\s*\bpledge\s*\([^;]*\)\s*;\s*$"
)
_UNVEIL_DISCARDED = re.compile(
    r"^\s*\bunveil\s*\([^;]*\)\s*;\s*$"
)
_SYSCTL_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>sysctl(?:byname)?)\s*\([^;]*\)\s*;\s*$"
)
_KQUEUE_DISCARDED = re.compile(
    r"^\s*\bkqueue\s*\([^;]*\)\s*;\s*$"
)
_KQUEUE_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?kqueue\s*\("
)
_KEVENT_DISCARDED = re.compile(
    r"^\s*\bkevent\s*\([^;]*\)\s*;\s*$"
)
_PAUSE_DISCARDED = re.compile(
    r"^\s*\bpause\s*\([^;]*\)\s*;\s*$"
)
_PPOLL_DISCARDED = re.compile(
    r"^\s*\bppoll\s*\([^;]*\)\s*;\s*$"
)
_SIGWAIT_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>sigtimedwait|sigwaitinfo|sigpending|sigwait)\s*\([^;]*\)\s*;\s*$"
)
_SIGQUEUE_DISCARDED = re.compile(
    r"^\s*\bsigqueue\s*\([^;]*\)\s*;\s*$"
)
_UCONTEXT_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>(?:get|set|swap|make)context)\s*\([^;]*\)\s*;\s*$"
)
_SEM_TIMEDWAIT_DISCARDED = re.compile(
    r"^\s*\bsem_timedwait\s*\([^;]*\)\s*;\s*$"
)
_PTHREAD_ATTR_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>pthread_attr_(?:init|destroy|setstack\w*|setdetachstate))\s*\([^;]*\)\s*;\s*$"
)
_CAP_ENTER_DISCARDED = re.compile(
    r"^\s*\bcap_enter\s*\([^;]*\)\s*;\s*$"
)
_CAP_RIGHTS_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>cap_rights_(?:limit|get))\s*\([^;]*\)\s*;\s*$"
)
_PDFORK_DISCARDED = re.compile(
    r"^\s*\bpdfork\s*\([^;]*\)\s*;\s*$"
)
_PROCCTL_DISCARDED = re.compile(
    r"^\s*\bprocctl\s*\([^;]*\)\s*;\s*$"
)
_CLOSEFROM_DISCARDED = re.compile(
    r"^\s*\bclosefrom\s*\([^;]*\)\s*;\s*$"
)
_ISSETUGID_DISCARDED = re.compile(
    r"^\s*\bissetugid\s*\([^;]*\)\s*;\s*$"
)
_ARC4RANDOM_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>arc4random(?:_buf|_uniform)?)\s*\([^;]*\)\s*;\s*$"
)
_CHFLAGS_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>(?:f|l)?chflags)\s*\([^;]*\)\s*;\s*$"
)
_GETFSSTAT_DISCARDED = re.compile(
    r"^\s*\bgetfsstat\s*\([^;]*\)\s*;\s*$"
)
_PTHREAD_YIELD_DISCARDED = re.compile(
    r"^\s*\bpthread_yield\s*\([^;]*\)\s*;\s*$"
)
_SEM_TRYWAIT_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>sem_(?:trywait|getvalue))\s*\([^;]*\)\s*;\s*$"
)
_ADJTIME_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>(?:ntp_)?adjtime(?!x))\s*\([^;]*\)\s*;\s*$"
)
_REVOKE_DISCARDED = re.compile(
    r"^\s*\brevoke\s*\([^;]*\)\s*;\s*$"
)
_KTRACE_DISCARDED = re.compile(
    r"^\s*\bktrace\s*\([^;]*\)\s*;\s*$"
)
_RFORK_DISCARDED = re.compile(
    r"^\s*\brfork\s*\([^;]*\)\s*;\s*$"
)
_JAIL_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>jail(?:_attach|_get|_set|_remove)?)\s*\([^;]*\)\s*;\s*$"
)
_SETLOGIN_DISCARDED = re.compile(
    r"^\s*\bsetlogin\s*\([^;]*\)\s*;\s*$"
)
_GETRESUID_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>getres(?:uid|gid))\s*\([^;]*\)\s*;\s*$"
)
_GETPEEREID_DISCARDED = re.compile(
    r"^\s*\bgetpeereid\s*\([^;]*\)\s*;\s*$"
)
_STRTONUM_DISCARDED = re.compile(
    r"^\s*\bstrtonum\s*\([^;]*\)\s*;\s*$"
)
_REALLOCARRAY_DISCARDED = re.compile(
    r"^\s*\breallocarray\s*\([^;]*\)\s*;\s*$"
)
_REALLOCARRAY_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?\breallocarray\s*\("
)
_TIMINGSAFE_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>timingsafe_(?:bcmp|memcmp))\s*\([^;]*\)\s*;\s*$"
)
_GETPROGNAME_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>(?:get|set)progname)\s*\([^;]*\)\s*;\s*$"
)
_DAEMON_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>daemon|setproctitle)\s*\([^;]*\)\s*;\s*$"
)
_CAP_FCNTLS_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>cap_(?:fcntls|ioctls)_limit)\s*\([^;]*\)\s*;\s*$"
)
_PDGETPID_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>pd(?:getpid|wait4))\s*\([^;]*\)\s*;\s*$"
)
_KLDLOAD_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>kld(?:load|unload|find|sym|stat))\s*\([^;]*\)\s*;\s*$"
)
_EXTATTR_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>extattr_(?:set|get|delete|list)_(?:file|fd|link))\s*\([^;]*\)\s*;\s*$"
)
_MAC_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>mac_(?:set|get)_(?:proc|fd|file))\s*\([^;]*\)\s*;\s*$"
)
_AUDIT_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>auditon|getaudit|setaudit|auditctl)\s*\([^;]*\)\s*;\s*$"
)
_KVM_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>kvm_(?:open|openfiles|getprocs|close|nlist))\s*\([^;]*\)\s*;\s*$"
)
_REALLOCF_DISCARDED = re.compile(
    r"^\s*\breallocf\s*\([^;]*\)\s*;\s*$"
)
_REALLOCF_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?\breallocf\s*\("
)
_UUIDGEN_DISCARDED = re.compile(
    r"^\s*\buuidgen\s*\([^;]*\)\s*;\s*$"
)
_SETFIB_DISCARDED = re.compile(
    r"^\s*\bsetfib\s*\([^;]*\)\s*;\s*$"
)
_NTP_GETTIME_DISCARDED = re.compile(
    r"^\s*\bntp_gettime\s*\([^;]*\)\s*;\s*$"
)
_CRYPT_NEWHASH_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>crypt_(?:newhash|checkpass))\s*\([^;]*\)\s*;\s*$"
)
_WAIT6_DISCARDED = re.compile(
    r"^\s*\bwait6\s*\([^;]*\)\s*;\s*$"
)
_CPUSET_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>cpuset_(?:set|get)affinity)\s*\([^;]*\)\s*;\s*$"
)
_RTPRIO_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>rtprio(?:_thread)?)\s*\([^;]*\)\s*;\s*$"
)
_KENV_DISCARDED = re.compile(
    r"^\s*\bkenv\s*\([^;]*\)\s*;\s*$"
)
_GETFH_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>getfh|fhopen|fhstatfs|fhstat|getfhat)\s*\([^;]*\)\s*;\s*$"
)
_GETMNTINFO_DISCARDED = re.compile(
    r"^\s*\bgetmntinfo\s*\([^;]*\)\s*;\s*$"
)
_NMOUNT_DISCARDED = re.compile(
    r"^\s*\bnmount\s*\([^;]*\)\s*;\s*$"
)
_STRMODE_DISCARDED = re.compile(
    r"^\s*\bstrmode\s*\([^;]*\)\s*;\s*$"
)
_GETOSRELDATE_DISCARDED = re.compile(
    r"^\s*\bgetosreldate\s*\([^;]*\)\s*;\s*$"
)
_CAP_SANDBOXED_DISCARDED = re.compile(
    r"^\s*\bcap_sandboxed\s*\([^;]*\)\s*;\s*$"
)
_GETGROUPLIST_DISCARDED = re.compile(
    r"^\s*\bgetgrouplist\s*\([^;]*\)\s*;\s*$"
)
_EACCESS_DISCARDED = re.compile(
    r"^\s*\beaccess\s*\([^;]*\)\s*;\s*$"
)
_LOGIN_GETCLASS_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>login_getclass|setusercontext)\s*\([^;]*\)\s*;\s*$"
)
_FFLAGS_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>fflagstostr|strtofflags)\s*\([^;]*\)\s*;\s*$"
)
_GETDIRENTRIES_DISCARDED = re.compile(
    r"^\s*\bgetdirentries\s*\([^;]*\)\s*;\s*$"
)
_KINFO_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>kinfo_get(?:proc|file|vmmap))\s*\([^;]*\)\s*;\s*$"
)
_UMTX_DISCARDED = re.compile(
    r"^\s*\b_umtx_op\s*\([^;]*\)\s*;\s*$"
)
_THR_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>thr_(?:new|kill2|kill|self|exit))\s*\([^;]*\)\s*;\s*$"
)
_MODFIND_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>mod(?:find|stat|next|fnext))\s*\([^;]*\)\s*;\s*$"
)
_LPATHCONF_DISCARDED = re.compile(
    r"^\s*\blpathconf\s*\([^;]*\)\s*;\s*$"
)
_LOGINCLASS_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>(?:get|set)loginclass)\s*\([^;]*\)\s*;\s*$"
)
_GETFSENT_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>(?:get|set|end)fsent)\s*\([^;]*\)\s*;\s*$"
)
_MINHERIT_DISCARDED = re.compile(
    r"^\s*\bminherit\s*\([^;]*\)\s*;\s*$"
)
_CAP_GETMODE_DISCARDED = re.compile(
    r"^\s*\bcap_getmode\s*\([^;]*\)\s*;\s*$"
)
_NFSSVC_DISCARDED = re.compile(
    r"^\s*\bnfssvc\s*\([^;]*\)\s*;\s*$"
)
_SYSARCH_DISCARDED = re.compile(
    r"^\s*\bsysarch\s*\([^;]*\)\s*;\s*$"
)
_GETPAGESIZES_DISCARDED = re.compile(
    r"^\s*\bgetpagesizes\s*\([^;]*\)\s*;\s*$"
)
_SBRK_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>sbrk|brk)\s*\([^;]*\)\s*;\s*$"
)
_KSEM_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>ksem_(?:open|close|unlink|wait|post))\s*\([^;]*\)\s*;\s*$"
)
_CAP_GETRIGHTS_DISCARDED = re.compile(
    r"^\s*\bcap_getrights\s*\([^;]*\)\s*;\s*$"
)
_DEVNAME_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>devname(?:_r)?)\s*\([^;]*\)\s*;\s*$"
)
_GETBOOTFILE_DISCARDED = re.compile(
    r"^\s*\bgetbootfile\s*\([^;]*\)\s*;\s*$"
)
_KLDFIRSTMOD_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>kld(?:firstmod|nextmod))\s*\([^;]*\)\s*;\s*$"
)
_FHLINK_DISCARDED = re.compile(
    r"^\s*\b(?P<fn>fh(?:linkat|link|readlink))\s*\([^;]*\)\s*;\s*$"
)
_VALLOC_DISCARDED = re.compile(
    r"^\s*\bvalloc\s*\([^;]*\)\s*;\s*$"
)
_VALLOC_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?\bvalloc\s*\("
)
_GETDOMAINNAME_DISCARDED = re.compile(
    r"^\s*\bgetdomainname\s*\([^;]*\)\s*;\s*$"
)
_UNLINK_DISCARDED = re.compile(
    r"^\s*(?:unlink|remove)\s*\([^;]*\)\s*;\s*$"
)
_MKFIFO_DISCARDED = re.compile(
    r"^\s*(?:mkfifo|mknod)\s*\([^;]*\)\s*;\s*$"
)
_DLOPEN_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?dlopen\s*\("
)
_ACCEPT_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?:accept4|accept)\s*\("
)
_REALPATH_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?realpath\s*\("
)
_CLZ_FNS = (
    "__builtin_clzll", "__builtin_ctzll",
    "__builtin_clz", "__builtin_ctz",
)
_WORLD_WRITABLE_MODE = 0o777


def _fork_compared_in_stmt(ln: str) -> bool:
    return bool(_rx(r"(?:vfork|fork)\s*\([^;]*\)\s*(?:==|!=|<|>|<=|>=)").search(ln) or _rx(r"(?:==|!=|<|>|<=|>=)\s*(?:vfork|fork)\s*\(").search(ln))


def _fork_pid_tested(var: str, text: str) -> bool:
    v = re.escape(var)
    if re.search(rf"{v}\s*(?:==|!=|<|>|<=|>=)\s*-?\d+", text):
        return True
    if re.search(rf"!\s*{v}\b", text):
        return True
    if re.search(rf"\bif\s*\(\s*{v}\s*\)", text):
        return True
    return False


def _api_fork(lines, rel, funcs, out) -> None:
    """fork()/vfork() result unused: not compared to 0/-1, not assigned then tested."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[int] = set()
        for i, ln in enumerate(chunk):
            if not _rx(r"(?<![A-Za-z0-9_])(?:vfork|fork)\s*\(").search(ln):
                continue
            if _fork_compared_in_stmt(ln):
                continue
            if _FORK_DISCARDED.match(ln):
                if i in seen:
                    continue
                seen.add(i)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="API-FORK",
                    message="fork()/vfork() result unused (not compared to 0/-1)",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                continue
            m = _FORK_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            rest = "\n".join(chunk[i:])
            if _fork_pid_tested(var, rest):
                continue
            if i in seen:
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FORK",
                message="fork()/vfork() result unused (not compared to 0/-1)",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_exec(lines, rel, funcs, out) -> None:
    """execl/execv family with a non-literal path (CWE-78)."""
    for fn in funcs:
        start = fn.span[0]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(fn.body.splitlines()):
            for fname in _EXEC_FNS:
                args = _find_call_args(ln, fname)
                if not args:
                    continue
                if _is_string_literal(args[0]):
                    continue
                key = (fname, i)
                if key in seen:
                    continue
                seen.add(key)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="API-EXEC",
                    message=f"{fname}() path is not a string literal",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                break


def _map_failed_test_for(var: str, text: str) -> bool:
    """True if `text` compares `var` to MAP_FAILED or (void*)-1."""
    v = re.escape(var)
    failed = r"(?:MAP_FAILED|\(\s*void\s*\*\s*\)\s*-\s*1)"
    return bool(re.search(
        rf"(?:{v}\s*(?:==|!=)\s*{failed}|{failed}\s*(?:==|!=)\s*{v})",
        text,
    ))


def _api_mmap(lines, rel, funcs, out) -> None:
    """mmap() result used without a MAP_FAILED / (void*)-1 test."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _MMAP_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            use_j = _first_ptr_use(var, chunk, i)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _map_failed_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MMAP",
                message=f"{var} from mmap() used without a MAP_FAILED test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _wcs_unbounded(lines, rel, funcs, out) -> None:
    """wcscpy/wcscat into a fixed-size local wchar_t buffer."""
    for fn in funcs:
        locals_arr = _local_wchar_arrays(fn)
        if not locals_arr:
            continue
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            for fname, dst_idx in _WCS_UNBOUNDED_FN.items():
                args = _find_call_args(ln, fname)
                if args is None or len(args) <= dst_idx:
                    continue
                dst = _rx(r"\s+").sub("", args[dst_idx])
                if dst not in locals_arr:
                    continue
                key = (fname, i)
                if key in seen:
                    continue
                seen.add(key)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="STR-WCSCPY",
                    message=f"{fname}() into local {dst}[…] with no bound",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))


def _api_getcwd(lines, rel, funcs, out) -> None:
    """getcwd() result used without a NULL test (CWE-252/476)."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _GETCWD_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            use_j = _first_ptr_use(var, chunk, i)
            if use_j is None:
                if not any(_getenv_uses(var, chunk[j]) for j in range(i + 1, len(chunk))):
                    continue
                use_j = next(
                    j for j in range(i + 1, len(chunk)) if _getenv_uses(var, chunk[j])
                )
            between = "\n".join(chunk[i:use_j + 1])
            if _null_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETCWD",
                message=f"{var} from getcwd() used without a NULL test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_ioctl(lines, rel, funcs, out) -> None:
    """ioctl() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _IOCTL_DISCARDED)):
            if not _IOCTL_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-IOCTL",
                message="ioctl() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _var_in_call_args(var: str, ln: str, skip_callees: tuple[str, ...] = ()) -> bool:
    """True when `var` is passed as an argument to some call on `ln`."""
    for m in _rx(r"\b([A-Za-z_]\w*)\s*\(").finditer(ln):
        name = m.group(1)
        if name in _KW or name in skip_callees:
            continue
        args = _find_call_args(ln, name)
        if not args:
            continue
        if any(_rx(r"\s+").sub("", a) == var for a in args):
            return True
    return False


def _first_ptr_or_arg_use(
    var: str, chunk: list[str], start: int, skip_callees: tuple[str, ...] = (),
) -> int | None:
    ptr = _first_ptr_use(var, chunk, start)
    arg = None
    for j in range(start + 1, len(chunk)):
        if _var_in_call_args(var, chunk[j], skip_callees):
            arg = j
            break
    cands = [x for x in (ptr, arg) if x is not None]
    return min(cands) if cands else None


def _lt_zero_test_for(var: str, text: str) -> bool:
    """True if `text` tests `var < 0` (accept/dup/pipe error)."""
    v = re.escape(var)
    if re.search(rf"{v}\s*<\s*0\b", text):
        return True
    if _rx(r"(?:accept4|accept|dup3|dup2|dup|pipe2|pipe|openat2|openat|shm_open|"
        r"memfd_secret|memfd_create|inotify_init1|inotify_init|pidfd_open|fanotify_init|"
        r"userfaultfd|landlock_create_ruleset|signalfd|perf_event_open|"
        r"pkey_alloc|fsopen|mq_open|shmget|semget|msgget)"
        r"\s*\([^;]*\)\s*\)*\s*<\s*0\b").search(text):
        return True
    return False


def _first_fd_use(
    var: str, chunk: list[str], start: int,
    skip_callees: tuple[str, ...] = ("accept", "accept4"),
) -> int | None:
    v = re.escape(var)
    for j in range(start + 1, len(chunk)):
        ln = chunk[j]
        if _var_in_call_args(var, ln, skip_callees=skip_callees):
            return j
        if re.search(rf"\breturn\s+{v}\s*;", ln):
            return j
        if re.search(rf"\b{v}\s*[+\-*/%]", ln):
            return j
    return None


def _nonzero_guard_for(var: str, text: str) -> bool:
    """True if `text` tests that `var` is nonzero before a clz/ctz call."""
    v = re.escape(var)
    if re.search(rf"\bif\s*\(\s*!\s*{v}\s*\)", text):
        return True
    if re.search(rf"\bif\s*\(\s*{v}\s*==\s*0\b", text):
        return True
    if re.search(rf"\bif\s*\(\s*0\s*==\s*{v}\b", text):
        return True
    if re.search(rf"\bif\s*\(\s*{v}\s*!=\s*0\b", text):
        return True
    if re.search(rf"\bif\s*\(\s*{v}\s*\)", text):
        return True
    return False


def _api_dlopen(lines, rel, funcs, out) -> None:
    """dlopen() result used without a NULL test (CWE-252)."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _DLOPEN_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            use_j = _first_ptr_or_arg_use(var, chunk, i, skip_callees=("dlopen",))
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _null_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-DLOPEN",
                message=f"{var} from dlopen() used without a NULL test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_accept(lines, rel, funcs, out) -> None:
    """accept()/accept4() result used without a `< 0` test (CWE-252)."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _ACCEPT_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-ACCEPT",
                message=f"{var} from accept() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_realpath(lines, rel, funcs, out) -> None:
    """realpath() result used without a NULL test (CWE-252/22)."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _REALPATH_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            use_j = _first_ptr_or_arg_use(var, chunk, i, skip_callees=("realpath",))
            if use_j is None:
                if not any(_getenv_uses(var, chunk[j]) for j in range(i + 1, len(chunk))):
                    continue
                use_j = next(
                    j for j in range(i + 1, len(chunk)) if _getenv_uses(var, chunk[j])
                )
            between = "\n".join(chunk[i:use_j + 1])
            if _null_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-REALPATH",
                message=f"{var} from realpath() used without a NULL test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_chmod_world(lines, rel, funcs, out) -> None:
    """chmod/fchmod with mode 0777 / 0x1ff / 511 (CWE-732)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(fn.body.splitlines()):
            for fname in ("chmod", "fchmod"):
                args = _find_call_args(ln, fname)
                if not args or len(args) < 2:
                    continue
                mode = _parse_int_literal(args[1].strip())
                if mode != _WORLD_WRITABLE_MODE:
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="API-CHMOD-WORLD",
                    message=f"{fname}() mode is world-writable 0777",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                break


def _int_clz_zero(lines, rel, funcs, out) -> None:
    """__builtin_clz/ctz (and ll) on constant 0 or an unguarded variable (CWE-758)."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[int] = set()
        for i, ln in enumerate(chunk):
            for fname in _CLZ_FNS:
                args = _find_call_args(ln, fname)
                if args is None or not args:
                    continue
                arg = args[0].strip()
                lit = _parse_int_literal(arg)
                if lit == 0:
                    bad = True
                elif _rx(r"[A-Za-z_]\w*").fullmatch(arg):
                    before = "\n".join(chunk[:i + 1])
                    bad = not _nonzero_guard_for(arg, before)
                else:
                    bad = False
                if not bad:
                    continue
                if i in seen:
                    continue
                seen.add(i)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="INT-CLZ-ZERO",
                    message=f"{fname}() on 0 is undefined",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                break


def _api_setuid(lines, rel, funcs, out) -> None:
    """setuid/seteuid/setgid to 0 with the return discarded (CWE-250)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SETUID_DISCARDED)):
            if not _SETUID_DISCARDED.match(ln):
                continue
            fname = None
            args = None
            for name in ("setuid", "seteuid", "setgid"):
                args = _find_call_args(ln, name)
                if args is not None:
                    fname = name
                    break
            if not args or _parse_int_literal(args[0].strip()) != 0:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SETUID",
                message=f"{fname}(0) return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_socket(lines, rel, funcs, out) -> None:
    """socket() result used without a `< 0` test (CWE-252)."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _SOCKET_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SOCKET",
                message=f"{var} from socket() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_bind(lines, rel, funcs, out) -> None:
    """bind() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _BIND_DISCARDED)):
            if not _BIND_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-BIND",
                message="bind() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_listen(lines, rel, funcs, out) -> None:
    """listen() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _LISTEN_DISCARDED)):
            if not _LISTEN_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-LISTEN",
                message="listen() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_connect(lines, rel, funcs, out) -> None:
    """connect() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CONNECT_DISCARDED)):
            if not _CONNECT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CONNECT",
                message="connect() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _pipe_compared_in_stmt(ln: str) -> bool:
    return bool(_rx(r"(?:pipe2|pipe)\s*\([^;]*\)\s*(?:==|!=|<|>|<=|>=)").search(ln) or _rx(r"(?:==|!=|<|>|<=|>=)\s*(?:pipe2|pipe)\s*\(").search(ln))


def _pipe_fds_used(name: str, chunk: list[str], start: int) -> bool:
    v = re.escape(name)
    for j in range(start + 1, len(chunk)):
        ln = chunk[j]
        if re.search(rf"\b{v}\s*\[", ln):
            return True
        if _var_in_call_args(name, ln, skip_callees=("pipe", "pipe2")):
            return True
        if re.search(rf"\breturn\s+{v}\s*;", ln):
            return True
    return False


def _api_pipe(lines, rel, funcs, out) -> None:
    """pipe()/pipe2() result used without a `< 0` test (CWE-252)."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[int] = set()
        for i, ln in enumerate(chunk):
            if not _rx(r"(?<![A-Za-z0-9_])(?:pipe2|pipe)\s*\(").search(ln):
                continue
            if _pipe_compared_in_stmt(ln):
                continue
            fname = "pipe2" if _find_call_args(ln, "pipe2") else "pipe"
            args = _find_call_args(ln, fname)
            if not args:
                continue
            fds = _rx(r"\s+").sub("", args[0])
            if not _rx(r"^[A-Za-z_]\w*$").match(fds):
                continue
            if i in seen:
                continue
            if _PIPE_DISCARDED.match(ln):
                if not _pipe_fds_used(fds, chunk, i):
                    continue
                seen.add(i)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="API-PIPE",
                    message=f"{fds} from {fname}() used without a < 0 test",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                continue
            m = _PIPE_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=("pipe", "pipe2"))
            fds_used = _pipe_fds_used(fds, chunk, i)
            if use_j is None and not fds_used:
                continue
            end_j = use_j if use_j is not None else i
            between = "\n".join(chunk[i:end_j + 1]) if use_j is not None else ln
            if _lt_zero_test_for(var, between):
                continue
            if fds_used:
                rest = "\n".join(chunk[i:])
                if _lt_zero_test_for(var, rest):
                    continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PIPE",
                message=f"{fname}() result used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_dup(lines, rel, funcs, out) -> None:
    """dup/dup2/dup3 result used without a `< 0` test (CWE-252)."""
    skip = ("dup", "dup2", "dup3", "accept", "accept4")
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _DUP_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-DUP",
                message=f"{var} from dup() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_fcntl(lines, rel, funcs, out) -> None:
    """fcntl() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _FCNTL_DISCARDED)):
            if not _FCNTL_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FCNTL",
                message="fcntl() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_wait(lines, rel, funcs, out) -> None:
    """wait/waitpid/waitid return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _WAIT_DISCARDED)):
            if not _WAIT_DISCARDED.match(ln):
                continue
            if _find_call_args(ln, "waitpid"):
                fname = "waitpid"
            elif _find_call_args(ln, "waitid"):
                fname = "waitid"
            else:
                fname = "wait"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-WAIT",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_select(lines, rel, funcs, out) -> None:
    """select()/poll()/epoll_wait() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SELECT_DISCARDED)):
            if not _SELECT_DISCARDED.match(ln):
                continue
            if _find_call_args(ln, "epoll_wait"):
                fname = "epoll_wait"
            elif _find_call_args(ln, "poll"):
                fname = "poll"
            else:
                fname = "select"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SELECT",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_send(lines, rel, funcs, out) -> None:
    """send()/recv()/sendto()/recvfrom() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SEND_DISCARDED)):
            if not _SEND_DISCARDED.match(ln):
                continue
            if _find_call_args(ln, "sendto"):
                fname = "sendto"
            elif _find_call_args(ln, "recvfrom"):
                fname = "recvfrom"
            elif _find_call_args(ln, "send"):
                fname = "send"
            else:
                fname = "recv"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SEND",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_shutdown(lines, rel, funcs, out) -> None:
    """shutdown() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SHUTDOWN_DISCARDED)):
            if not _SHUTDOWN_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SHUTDOWN",
                message="shutdown() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_kill(lines, rel, funcs, out) -> None:
    """kill()/raise() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _KILL_DISCARDED)):
            if not _KILL_DISCARDED.match(ln):
                continue
            fname = "kill" if _find_call_args(ln, "kill") else "raise"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-KILL",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _getaddrinfo_zero_test(var: str | None, text: str) -> bool:
    """True if `text` tests getaddrinfo success with `== 0` / `!= 0`."""
    if _rx(r"getaddrinfo\s*\([^;]*\)\s*\)*\s*(?:==|!=)\s*0\b").search(text):
        return True
    if _rx(r"(?:==|!=)\s*0\b[^=\n]*getaddrinfo\s*\(|"
        r"0\s*(?:==|!=)\s*getaddrinfo\s*\(").search(text):
        return True
    if var:
        v = re.escape(var)
        if re.search(rf"{v}\s*(?:==|!=)\s*0\b", text):
            return True
        if re.search(rf"0\s*(?:==|!=)\s*{v}\b", text):
            return True
        if re.search(rf"\bif\s*\(\s*{v}\s*\)", text):
            return True
    return False


def _api_getaddrinfo(lines, rel, funcs, out) -> None:
    """getaddrinfo() result used without a `== 0` / error test (CWE-252)."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[int] = set()
        for i, ln in enumerate(chunk):
            if i in seen:
                continue
            if _GETADDRINFO_DISCARDED.match(ln):
                rest = "\n".join(chunk[i:])
                if _getaddrinfo_zero_test(None, rest):
                    continue
                seen.add(i)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="API-GETADDRINFO",
                    message="getaddrinfo() result used without a == 0 test",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                continue
            m = _GETADDRINFO_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            if _getaddrinfo_zero_test(var, ln):
                continue
            rest = "\n".join(chunk[i:])
            if _getaddrinfo_zero_test(var, rest):
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETADDRINFO",
                message=f"{var} from getaddrinfo() used without a == 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pthread_join(lines, rel, funcs, out) -> None:
    """pthread_join()/pthread_detach() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PTHREAD_JOIN_DISCARDED)):
            if not _PTHREAD_JOIN_DISCARDED.match(ln):
                continue
            fname = (
                "pthread_join" if _find_call_args(ln, "pthread_join")
                else "pthread_detach"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PTHREAD-JOIN",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_thrd_join(lines, rel, funcs, out) -> None:
    """ISO C11 thrd_join()/thrd_detach() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _THRD_JOIN_DISCARDED)):
            if not _THRD_JOIN_DISCARDED.match(ln):
                continue
            fname = (
                "thrd_join" if _find_call_args(ln, "thrd_join")
                else "thrd_detach"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-THRD-JOIN",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sem_wait(lines, rel, funcs, out) -> None:
    """sem_wait()/sem_post() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SEM_WAIT_DISCARDED)):
            if not _SEM_WAIT_DISCARDED.match(ln):
                continue
            fname = "sem_wait" if _find_call_args(ln, "sem_wait") else "sem_post"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SEM-WAIT",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_openat(lines, rel, funcs, out) -> None:
    """openat() result used without a `< 0` test (CWE-252)."""
    skip = ("openat",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _OPENAT_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-OPENAT",
                message=f"{var} from openat() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_flock(lines, rel, funcs, out) -> None:
    """flock() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _FLOCK_DISCARDED)):
            if not _FLOCK_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FLOCK",
                message="flock() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_chown(lines, rel, funcs, out) -> None:
    """chown/fchown/lchown return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CHOWN_DISCARDED)):
            if not _CHOWN_DISCARDED.match(ln):
                continue
            if _find_call_args(ln, "fchown"):
                fname = "fchown"
            elif _find_call_args(ln, "lchown"):
                fname = "lchown"
            else:
                fname = "chown"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CHOWN",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_symlink(lines, rel, funcs, out) -> None:
    """symlink()/readlink() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SYMLINK_DISCARDED)):
            if not _SYMLINK_DISCARDED.match(ln):
                continue
            fname = "symlink" if _find_call_args(ln, "symlink") else "readlink"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SYMLINK",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_opendir(lines, rel, funcs, out) -> None:
    """opendir() result used without a NULL test (CWE-252/476)."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _OPENDIR_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            use_j = _first_ptr_or_arg_use(var, chunk, i, skip_callees=("opendir",))
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _null_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-OPENDIR",
                message=f"{var} from opendir() used without a NULL test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_setrlimit(lines, rel, funcs, out) -> None:
    """setrlimit()/getrlimit() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SETRLIMIT_DISCARDED)):
            if not _SETRLIMIT_DISCARDED.match(ln):
                continue
            fname = "setrlimit" if _find_call_args(ln, "setrlimit") else "getrlimit"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SETRLIMIT",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getsockopt(lines, rel, funcs, out) -> None:
    """getsockopt()/setsockopt() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETSOCKOPT_DISCARDED)):
            if not _GETSOCKOPT_DISCARDED.match(ln):
                continue
            fname = "getsockopt" if _find_call_args(ln, "getsockopt") else "setsockopt"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETSOCKOPT",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_stat(lines, rel, funcs, out) -> None:
    """stat()/lstat()/fstat() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _STAT_DISCARDED)):
            if not _STAT_DISCARDED.match(ln):
                continue
            if _find_call_args(ln, "lstat"):
                fname = "lstat"
            elif _find_call_args(ln, "fstat"):
                fname = "fstat"
            else:
                fname = "stat"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-STAT",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_mkdir(lines, rel, funcs, out) -> None:
    """mkdir()/rmdir() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MKDIR_DISCARDED)):
            if not _MKDIR_DISCARDED.match(ln):
                continue
            fname = "mkdir" if _find_call_args(ln, "mkdir") else "rmdir"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MKDIR",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getpwuid(lines, rel, funcs, out) -> None:
    """getpwuid()/getpwnam() result used without a NULL test (CWE-252/476)."""
    skip = ("getpwuid", "getpwnam")
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _GETPWUID_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            use_j = _first_ptr_or_arg_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _null_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            fname = "getpwnam" if _find_call_args(ln, "getpwnam") else "getpwuid"
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETPWUID",
                message=f"{var} from {fname}() used without a NULL test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_clock_gettime(lines, rel, funcs, out) -> None:
    """clock_gettime()/gettimeofday() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CLOCK_GETTIME_DISCARDED)):
            if not _CLOCK_GETTIME_DISCARDED.match(ln):
                continue
            fname = (
                "gettimeofday" if _find_call_args(ln, "gettimeofday")
                else "clock_gettime"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CLOCK-GETTIME",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_shm_open(lines, rel, funcs, out) -> None:
    """shm_open() result used without a `< 0` test (CWE-252)."""
    skip = ("shm_open",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _SHM_OPEN_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SHM-OPEN",
                message=f"{var} from shm_open() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_posix_spawn(lines, rel, funcs, out) -> None:
    """posix_spawn()/posix_spawnp() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _POSIX_SPAWN_DISCARDED)):
            if not _POSIX_SPAWN_DISCARDED.match(ln):
                continue
            fname = (
                "posix_spawnp" if _find_call_args(ln, "posix_spawnp")
                else "posix_spawn"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-POSIX-SPAWN",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_glob(lines, rel, funcs, out) -> None:
    """glob() return discarded (not compared to 0) (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GLOB_DISCARDED)):
            if not _GLOB_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GLOB",
                message="glob() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_fseek(lines, rel, funcs, out) -> None:
    """fseek()/ftell() return discarded (CWE-252). rewind is void — skip."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _FSEEK_DISCARDED)):
            if not _FSEEK_DISCARDED.match(ln):
                continue
            fname = "ftell" if _find_call_args(ln, "ftell") else "fseek"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FSEEK",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_access(lines, rel, funcs, out) -> None:
    """access() return discarded (CWE-252). Not CONC-TOCTOU (no open)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _ACCESS_DISCARDED)):
            if not _ACCESS_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-ACCESS",
                message="access() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getopt(lines, rel, funcs, out) -> None:
    """getopt()/getopt_long() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETOPT_DISCARDED)):
            if not _GETOPT_DISCARDED.match(ln):
                continue
            fname = (
                "getopt_long" if _find_call_args(ln, "getopt_long")
                else "getopt"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETOPT",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_uname(lines, rel, funcs, out) -> None:
    """uname()/gethostname() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _UNAME_DISCARDED)):
            if not _UNAME_DISCARDED.match(ln):
                continue
            fname = "gethostname" if _find_call_args(ln, "gethostname") else "uname"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-UNAME",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sendfile(lines, rel, funcs, out) -> None:
    """sendfile()/copy_file_range() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SENDFILE_DISCARDED)):
            if not _SENDFILE_DISCARDED.match(ln):
                continue
            fname = (
                "copy_file_range" if _find_call_args(ln, "copy_file_range")
                else "sendfile"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SENDFILE",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_memfd(lines, rel, funcs, out) -> None:
    """memfd_create() result used without a `< 0` test (CWE-252)."""
    skip = ("memfd_create",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _MEMFD_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MEMFD",
                message=f"{var} from memfd_create() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_prctl(lines, rel, funcs, out) -> None:
    """prctl()/ptrace() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PRCTL_DISCARDED)):
            if not _PRCTL_DISCARDED.match(ln):
                continue
            fname = "ptrace" if _find_call_args(ln, "ptrace") else "prctl"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PRCTL",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_tcgetattr(lines, rel, funcs, out) -> None:
    """tcgetattr()/tcsetattr() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _TCGETATTR_DISCARDED)):
            if not _TCGETATTR_DISCARDED.match(ln):
                continue
            fname = (
                "tcsetattr" if _find_call_args(ln, "tcsetattr")
                else "tcgetattr"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-TCGETATTR",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sysconf(lines, rel, funcs, out) -> None:
    """sysconf()/pathconf() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SYSCONF_DISCARDED)):
            if not _SYSCONF_DISCARDED.match(ln):
                continue
            fname = "pathconf" if _find_call_args(ln, "pathconf") else "sysconf"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SYSCONF",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getrusage(lines, rel, funcs, out) -> None:
    """getrusage() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETRUSAGE_DISCARDED)):
            if not _GETRUSAGE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETRUSAGE",
                message="getrusage() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_nftw(lines, rel, funcs, out) -> None:
    """nftw()/ftw() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _NFTW_DISCARDED)):
            if not _NFTW_DISCARDED.match(ln):
                continue
            fname = "nftw" if _find_call_args(ln, "nftw") else "ftw"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-NFTW",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_wordexp(lines, rel, funcs, out) -> None:
    """wordexp() return discarded (not compared to 0) (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _WORDEXP_DISCARDED)):
            if not _WORDEXP_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-WORDEXP",
                message="wordexp() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getlogin(lines, rel, funcs, out) -> None:
    """getlogin()/ttyname() result used without a NULL test (CWE-252/476)."""
    skip = ("getlogin", "ttyname")
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _GETLOGIN_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            use_j = _first_ptr_or_arg_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _null_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            fname = "ttyname" if _find_call_args(ln, "ttyname") else "getlogin"
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETLOGIN",
                message=f"{var} from {fname}() used without a NULL test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_inet_pton(lines, rel, funcs, out) -> None:
    """inet_pton()/inet_aton() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _INET_PTON_DISCARDED)):
            if not _INET_PTON_DISCARDED.match(ln):
                continue
            fname = (
                "inet_aton" if _find_call_args(ln, "inet_aton")
                else "inet_pton"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-INET-PTON",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_mlock(lines, rel, funcs, out) -> None:
    """mlock()/munlock() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MLOCK_DISCARDED)):
            if not _MLOCK_DISCARDED.match(ln):
                continue
            fname = "munlock" if _find_call_args(ln, "munlock") else "mlock"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MLOCK",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_splice(lines, rel, funcs, out) -> None:
    """splice() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SPLICE_DISCARDED)):
            if not _SPLICE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SPLICE",
                message="splice() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_inotify(lines, rel, funcs, out) -> None:
    """inotify_init()/inotify_init1() result used without a `< 0` test (CWE-252)."""
    skip = ("inotify_init", "inotify_init1")
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _INOTIFY_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            fname = (
                "inotify_init1" if _find_call_args(ln, "inotify_init1")
                else "inotify_init"
            )
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-INOTIFY",
                message=f"{var} from {fname}() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_fsync(lines, rel, funcs, out) -> None:
    """fsync()/fdatasync() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _FSYNC_DISCARDED)):
            if not _FSYNC_DISCARDED.match(ln):
                continue
            fname = (
                "fdatasync" if _find_call_args(ln, "fdatasync") else "fsync"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FSYNC",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getrandom(lines, rel, funcs, out) -> None:
    """getrandom()/getentropy() return discarded (CWE-252/330)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETRANDOM_DISCARDED)):
            if not _GETRANDOM_DISCARDED.match(ln):
                continue
            fname = (
                "getentropy" if _find_call_args(ln, "getentropy")
                else "getrandom"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETRANDOM",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getline(lines, rel, funcs, out) -> None:
    """getline()/getdelim() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETLINE_DISCARDED)):
            if not _GETLINE_DISCARDED.match(ln):
                continue
            fname = (
                "getdelim" if _find_call_args(ln, "getdelim") else "getline"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETLINE",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_asprintf(lines, rel, funcs, out) -> None:
    """asprintf()/vasprintf() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _ASPRINTF_DISCARDED)):
            if not _ASPRINTF_DISCARDED.match(ln):
                continue
            fname = (
                "vasprintf" if _find_call_args(ln, "vasprintf") else "asprintf"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-ASPRINTF",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_strlcpy(lines, rel, funcs, out) -> None:
    """strlcpy()/strlcat() return discarded (CWE-252). Not STR-UNBOUNDED-COPY."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _STRLCPY_DISCARDED)):
            if not _STRLCPY_DISCARDED.match(ln):
                continue
            fname = "strlcat" if _find_call_args(ln, "strlcat") else "strlcpy"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-STRLCPY",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_isatty(lines, rel, funcs, out) -> None:
    """isatty() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _ISATTY_DISCARDED)):
            if not _ISATTY_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-ISATTY",
                message="isatty() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_ptsname(lines, rel, funcs, out) -> None:
    """ptsname() result used without a NULL test (CWE-252/476)."""
    skip = ("ptsname",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _PTSNAME_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            use_j = _first_ptr_or_arg_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _null_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PTSNAME",
                message=f"{var} from ptsname() used without a NULL test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_mount(lines, rel, funcs, out) -> None:
    """mount()/umount() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MOUNT_DISCARDED)):
            if not _MOUNT_DISCARDED.match(ln):
                continue
            fname = "umount" if _find_call_args(ln, "umount") else "mount"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MOUNT",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_fmemopen(lines, rel, funcs, out) -> None:
    """fmemopen()/open_memstream() result used without a NULL test (CWE-252/476)."""
    skip = ("fmemopen", "open_memstream")
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _FMEMOPEN_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            use_j = _first_ptr_or_arg_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _null_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            fname = (
                "open_memstream" if _find_call_args(ln, "open_memstream")
                else "fmemopen"
            )
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FMEMOPEN",
                message=f"{var} from {fname}() used without a NULL test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_scandir(lines, rel, funcs, out) -> None:
    """scandir() return discarded (not compared to <0) (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SCANDIR_DISCARDED)):
            if not _SCANDIR_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SCANDIR",
                message="scandir() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_setxattr(lines, rel, funcs, out) -> None:
    """setxattr()/getxattr()/listxattr() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SETXATTR_DISCARDED)):
            if not _SETXATTR_DISCARDED.match(ln):
                continue
            if _find_call_args(ln, "listxattr"):
                fname = "listxattr"
            elif _find_call_args(ln, "getxattr"):
                fname = "getxattr"
            else:
                fname = "setxattr"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SETXATTR",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sched_affinity(lines, rel, funcs, out) -> None:
    """sched_setaffinity()/sched_getaffinity() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SCHED_AFFINITY_DISCARDED)):
            if not _SCHED_AFFINITY_DISCARDED.match(ln):
                continue
            fname = (
                "sched_getaffinity" if _find_call_args(ln, "sched_getaffinity")
                else "sched_setaffinity"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SCHED-AFFINITY",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_aio(lines, rel, funcs, out) -> None:
    """aio_read()/aio_write() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _AIO_DISCARDED)):
            if not _AIO_DISCARDED.match(ln):
                continue
            fname = "aio_write" if _find_call_args(ln, "aio_write") else "aio_read"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-AIO",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_statx(lines, rel, funcs, out) -> None:
    """statx() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _STATX_DISCARDED)):
            if not _STATX_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-STATX",
                message="statx() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pidfd(lines, rel, funcs, out) -> None:
    """pidfd_open() result used without a `< 0` test (CWE-252)."""
    skip = ("pidfd_open",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _PIDFD_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PIDFD",
                message=f"{var} from pidfd_open() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_capset(lines, rel, funcs, out) -> None:
    """capset()/capget() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CAPSET_DISCARDED)):
            if not _CAPSET_DISCARDED.match(ln):
                continue
            fname = "capget" if _find_call_args(ln, "capget") else "capset"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CAPSET",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_fanotify(lines, rel, funcs, out) -> None:
    """fanotify_init() result used without a `< 0` test (CWE-252)."""
    skip = ("fanotify_init",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _FANOTIFY_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FANOTIFY",
                message=f"{var} from fanotify_init() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_seccomp(lines, rel, funcs, out) -> None:
    """seccomp() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SECCOMP_DISCARDED)):
            if not _SECCOMP_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SECCOMP",
                message="seccomp() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getgrnam(lines, rel, funcs, out) -> None:
    """getgrnam()/getgrgid()/getspnam() used without a NULL test (CWE-252/476)."""
    skip = ("getgrnam", "getgrgid", "getspnam")
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _GETGRNAM_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            use_j = _first_ptr_or_arg_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _null_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            if _find_call_args(ln, "getspnam"):
                fname = "getspnam"
            elif _find_call_args(ln, "getgrgid"):
                fname = "getgrgid"
            else:
                fname = "getgrnam"
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETGRNAM",
                message=f"{var} from {fname}() used without a NULL test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_fallocate(lines, rel, funcs, out) -> None:
    """posix_fallocate()/fallocate() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _FALLOCATE_DISCARDED)):
            if not _FALLOCATE_DISCARDED.match(ln):
                continue
            fname = (
                "posix_fallocate" if _find_call_args(ln, "posix_fallocate")
                else "fallocate"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FALLOCATE",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_close_range(lines, rel, funcs, out) -> None:
    """close_range() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CLOSE_RANGE_DISCARDED)):
            if not _CLOSE_RANGE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CLOSE-RANGE",
                message="close_range() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_bpf(lines, rel, funcs, out) -> None:
    """bpf() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _BPF_DISCARDED)):
            if not _BPF_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-BPF",
                message="bpf() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_userfaultfd(lines, rel, funcs, out) -> None:
    """userfaultfd() result used without a `< 0` test (CWE-252)."""
    skip = ("userfaultfd",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _USERFAULTFD_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-USERFAULTFD",
                message=f"{var} from userfaultfd() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getpass(lines, rel, funcs, out) -> None:
    """getpass() result used without a NULL test (CWE-252/476)."""
    skip = ("getpass",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _GETPASS_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            use_j = _first_ptr_or_arg_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _null_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETPASS",
                message=f"{var} from getpass() used without a NULL test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_initgroups(lines, rel, funcs, out) -> None:
    """initgroups()/setgroups() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _INITGROUPS_DISCARDED)):
            if not _INITGROUPS_DISCARDED.match(ln):
                continue
            fname = (
                "setgroups" if _find_call_args(ln, "setgroups")
                else "initgroups"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-INITGROUPS",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_clone(lines, rel, funcs, out) -> None:
    """unshare()/setns()/clone() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CLONE_DISCARDED)):
            if not _CLONE_DISCARDED.match(ln):
                continue
            if _find_call_args(ln, "unshare"):
                fname = "unshare"
            elif _find_call_args(ln, "setns"):
                fname = "setns"
            else:
                fname = "clone"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CLONE",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_openat2(lines, rel, funcs, out) -> None:
    """openat2() result used without a `< 0` test (CWE-252)."""
    skip = ("openat2",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _OPENAT2_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-OPENAT2",
                message=f"{var} from openat2() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_landlock(lines, rel, funcs, out) -> None:
    """landlock_create_ruleset() result used without a `< 0` test (CWE-252)."""
    skip = ("landlock_create_ruleset",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _LANDLOCK_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-LANDLOCK",
                message=f"{var} from landlock_create_ruleset() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getpriority(lines, rel, funcs, out) -> None:
    """getpriority()/setpriority() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETPRIORITY_DISCARDED)):
            if not _GETPRIORITY_DISCARDED.match(ln):
                continue
            fname = (
                "setpriority" if _find_call_args(ln, "setpriority")
                else "getpriority"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETPRIORITY",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_signalfd(lines, rel, funcs, out) -> None:
    """signalfd() result used without a `< 0` test (CWE-252)."""
    skip = ("signalfd",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _SIGNALFD_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SIGNALFD",
                message=f"{var} from signalfd() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sendmmsg(lines, rel, funcs, out) -> None:
    """sendmmsg()/recvmmsg() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SENDMMSG_DISCARDED)):
            if not _SENDMMSG_DISCARDED.match(ln):
                continue
            fname = (
                "recvmmsg" if _find_call_args(ln, "recvmmsg") else "sendmmsg"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SENDMMSG",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_personality(lines, rel, funcs, out) -> None:
    """personality() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PERSONALITY_DISCARDED)):
            if not _PERSONALITY_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PERSONALITY",
                message="personality() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_quotactl(lines, rel, funcs, out) -> None:
    """quotactl() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _QUOTACTL_DISCARDED)):
            if not _QUOTACTL_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-QUOTACTL",
                message="quotactl() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_name_to_handle(lines, rel, funcs, out) -> None:
    """name_to_handle_at()/open_by_handle_at() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _NAME_TO_HANDLE_DISCARDED)):
            if not _NAME_TO_HANDLE_DISCARDED.match(ln):
                continue
            fname = (
                "open_by_handle_at"
                if _find_call_args(ln, "open_by_handle_at")
                else "name_to_handle_at"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-NAME-TO-HANDLE",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_process_madvise(lines, rel, funcs, out) -> None:
    """process_madvise() return discarded (CWE-252). Not madvise/posix_madvise."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PROCESS_MADVISE_DISCARDED)):
            if not _PROCESS_MADVISE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PROCESS-MADVISE",
                message="process_madvise() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pivot_root(lines, rel, funcs, out) -> None:
    """pivot_root() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PIVOT_ROOT_DISCARDED)):
            if not _PIVOT_ROOT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PIVOT-ROOT",
                message="pivot_root() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_statfs(lines, rel, funcs, out) -> None:
    """statfs()/fstatfs() return discarded (CWE-252). Not stat/fstat/lstat/statx."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _STATFS_DISCARDED)):
            if not _STATFS_DISCARDED.match(ln):
                continue
            fname = "fstatfs" if _find_call_args(ln, "fstatfs") else "statfs"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-STATFS",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_prlimit(lines, rel, funcs, out) -> None:
    """prlimit()/prlimit64() return discarded (CWE-252). Not setrlimit/getrlimit."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PRLIMIT_DISCARDED)):
            if not _PRLIMIT_DISCARDED.match(ln):
                continue
            fname = "prlimit64" if _find_call_args(ln, "prlimit64") else "prlimit"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PRLIMIT",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_perf_event(lines, rel, funcs, out) -> None:
    """perf_event_open() result used without a `< 0` test (CWE-252)."""
    skip = ("perf_event_open",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _PERF_EVENT_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PERF-EVENT",
                message=f"{var} from perf_event_open() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_membarrier(lines, rel, funcs, out) -> None:
    """membarrier() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MEMBARRIER_DISCARDED)):
            if not _MEMBARRIER_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MEMBARRIER",
                message="membarrier() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pkey(lines, rel, funcs, out) -> None:
    """pkey_alloc() result used without a `< 0` test (CWE-252). Not a bare pkey."""
    skip = ("pkey_alloc",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _PKEY_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PKEY",
                message=f"{var} from pkey_alloc() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_syncfs(lines, rel, funcs, out) -> None:
    """syncfs() return discarded (CWE-252). Not fsync/fdatasync/sync."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SYNCFS_DISCARDED)):
            if not _SYNCFS_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SYNCFS",
                message="syncfs() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_process_vm(lines, rel, funcs, out) -> None:
    """process_vm_readv()/process_vm_writev() return discarded (CWE-252).

    Not process_madvise.
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PROCESS_VM_DISCARDED)):
            if not _PROCESS_VM_DISCARDED.match(ln):
                continue
            fname = (
                "process_vm_writev"
                if _find_call_args(ln, "process_vm_writev")
                else "process_vm_readv"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PROCESS-VM",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_clone3(lines, rel, funcs, out) -> None:
    """clone3() return discarded (CWE-252). Not clone/unshare/setns."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CLONE3_DISCARDED)):
            if not _CLONE3_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CLONE3",
                message="clone3() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_futex(lines, rel, funcs, out) -> None:
    """futex() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _FUTEX_DISCARDED)):
            if not _FUTEX_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FUTEX",
                message="futex() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_keyctl(lines, rel, funcs, out) -> None:
    """keyctl() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _KEYCTL_DISCARDED)):
            if not _KEYCTL_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-KEYCTL",
                message="keyctl() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_kcmp(lines, rel, funcs, out) -> None:
    """kcmp() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _KCMP_DISCARDED)):
            if not _KCMP_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-KCMP",
                message="kcmp() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_fsopen(lines, rel, funcs, out) -> None:
    """fsopen() result used without a `< 0` test (CWE-252). Not open/openat/fopen."""
    skip = ("fsopen",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _FSOPEN_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FSOPEN",
                message=f"{var} from fsopen() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _mq_error_test_for(var: str, text: str) -> bool:
    """True if `text` tests mq descriptor `var` against <0 or (mqd_t)-1."""
    if _lt_zero_test_for(var, text):
        return True
    v = re.escape(var)
    neg1 = r"(?:\(\s*mqd_t\s*\)\s*)?-\s*1"
    if re.search(rf"{v}\s*(?:==|!=)\s*{neg1}", text):
        return True
    if re.search(rf"{neg1}\s*(?:==|!=)\s*{v}", text):
        return True
    if re.search(rf"{v}\s*<\s*\(\s*mqd_t\s*\)\s*0\b", text):
        return True
    if re.search(
        rf"\bmq_open\s*\([^;]*\)\s*\)*\s*(?:==|!=|<)\s*"
        rf"(?:\(\s*mqd_t\s*\)\s*)?(?:0|{neg1})",
        text,
    ):
        return True
    return False


def _api_mq_open(lines, rel, funcs, out) -> None:
    """mq_open() result used without (mqd_t)-1 / <0 (CWE-252). Not open/openat."""
    skip = ("mq_open",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _MQ_OPEN_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _mq_error_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                v = re.escape(var)
                for j in range(i + 1, len(chunk)):
                    if re.search(rf"\(\s*void\s*\)\s*{v}\b", chunk[j]):
                        use_j = j
                        break
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _mq_error_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MQ-OPEN",
                message=f"{var} from mq_open() used without a (mqd_t)-1 / < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_shmget(lines, rel, funcs, out) -> None:
    """shmget() discarded or used without `< 0` (CWE-252). Not shm_open."""
    skip = ("shmget",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[int] = set()
        for i, ln in enumerate(chunk):
            if _SHMGET_DISCARDED.match(ln):
                if i in seen:
                    continue
                seen.add(i)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="API-SHMGET",
                    message="shmget() return is discarded",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                continue
            m = _SHMGET_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            if i in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SHMGET",
                message=f"{var} from shmget() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_reboot(lines, rel, funcs, out) -> None:
    """reboot() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _REBOOT_DISCARDED)):
            if not _REBOOT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-REBOOT",
                message="reboot() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_adjtimex(lines, rel, funcs, out) -> None:
    """adjtimex() return discarded (CWE-252). Not clock_gettime."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _ADJTIMEX_DISCARDED)):
            if not _ADJTIMEX_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-ADJTIMEX",
                message="adjtimex() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sethostname(lines, rel, funcs, out) -> None:
    """sethostname() return discarded (CWE-252). Not gethostname/uname."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SETHOSTNAME_DISCARDED)):
            if not _SETHOSTNAME_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SETHOSTNAME",
                message="sethostname() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_swapon(lines, rel, funcs, out) -> None:
    """swapon()/swapoff() return discarded (CWE-252). Not C++ swap()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SWAPON_DISCARDED)):
            if not _SWAPON_DISCARDED.match(ln):
                continue
            fname = "swapoff" if _find_call_args(ln, "swapoff") else "swapon"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SWAPON",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_acct(lines, rel, funcs, out) -> None:
    """acct() return discarded (CWE-252). Not access()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _ACCT_DISCARDED)):
            if not _ACCT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-ACCT",
                message="acct() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_ioperm(lines, rel, funcs, out) -> None:
    """ioperm()/iopl() return discarded (CWE-252). Not ioprio_set."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _IOPERM_DISCARDED)):
            if not _IOPERM_DISCARDED.match(ln):
                continue
            fname = "iopl" if _find_call_args(ln, "iopl") else "ioperm"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-IOPERM",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_mincore(lines, rel, funcs, out) -> None:
    """mincore() return discarded (CWE-252). Not mlock/madvise."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MINCORE_DISCARDED)):
            if not _MINCORE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MINCORE",
                message="mincore() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_rseq(lines, rel, funcs, out) -> None:
    """rseq() return discarded (CWE-252). Not membarrier."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _RSEQ_DISCARDED)):
            if not _RSEQ_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-RSEQ",
                message="rseq() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_timer_create(lines, rel, funcs, out) -> None:
    """timer_create() return discarded (CWE-252). Not timerfd_create."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _TIMER_CREATE_DISCARDED)):
            if not _TIMER_CREATE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-TIMER-CREATE",
                message="timer_create() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_semget(lines, rel, funcs, out) -> None:
    """semget() discarded or used without `< 0` (CWE-252). Not sem_wait/semctl."""
    skip = ("semget",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[int] = set()
        for i, ln in enumerate(chunk):
            if _SEMGET_DISCARDED.match(ln):
                if i in seen:
                    continue
                seen.add(i)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="API-SEMGET",
                    message="semget() return is discarded",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                continue
            m = _SEMGET_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            if i in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SEMGET",
                message=f"{var} from semget() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_msgget(lines, rel, funcs, out) -> None:
    """msgget() discarded or used without `< 0` (CWE-252). Not mq_open/msgctl."""
    skip = ("msgget",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[int] = set()
        for i, ln in enumerate(chunk):
            if _MSGGET_DISCARDED.match(ln):
                if i in seen:
                    continue
                seen.add(i)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="API-MSGGET",
                    message="msgget() return is discarded",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                continue
            m = _MSGGET_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            if i in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MSGGET",
                message=f"{var} from msgget() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_klogctl(lines, rel, funcs, out) -> None:
    """klogctl() return discarded (CWE-252). Not a bare syslog(."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _KLOGCTL_DISCARDED)):
            if not _KLOGCTL_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-KLOGCTL",
                message="klogctl() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_mount_setattr(lines, rel, funcs, out) -> None:
    """mount_setattr() return discarded (CWE-252). Not mount/umount."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MOUNT_SETATTR_DISCARDED)):
            if not _MOUNT_SETATTR_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MOUNT-SETATTR",
                message="mount_setattr() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getcpu(lines, rel, funcs, out) -> None:
    """getcpu() return discarded (CWE-252). Not getrusage()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETCPU_DISCARDED)):
            if not _GETCPU_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETCPU",
                message="getcpu() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_process_mrelease(lines, rel, funcs, out) -> None:
    """process_mrelease() return discarded (CWE-252). Not process_madvise/process_vm."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PROCESS_MRELEASE_DISCARDED)):
            if not _PROCESS_MRELEASE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PROCESS-MRELEASE",
                message="process_mrelease() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_memfd_secret(lines, rel, funcs, out) -> None:
    """memfd_secret() result used without a `< 0` test (CWE-252). Not memfd_create."""
    skip = ("memfd_secret",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _MEMFD_SECRET_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            key = (var, i)
            if key in seen:
                continue
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MEMFD-SECRET",
                message=f"{var} from memfd_secret() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_ioprio(lines, rel, funcs, out) -> None:
    """ioprio_set()/ioprio_get() return discarded (CWE-252). Not ioperm()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _IOPRIO_DISCARDED)):
            if not _IOPRIO_DISCARDED.match(ln):
                continue
            fname = "ioprio_get" if _find_call_args(ln, "ioprio_get") else "ioprio_set"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-IOPRIO",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_init_module(lines, rel, funcs, out) -> None:
    """init_module()/finit_module()/delete_module() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _INIT_MODULE_DISCARDED)):
            if not _INIT_MODULE_DISCARDED.match(ln):
                continue
            if _find_call_args(ln, "finit_module"):
                fname = "finit_module"
            elif _find_call_args(ln, "delete_module"):
                fname = "delete_module"
            else:
                fname = "init_module"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-INIT-MODULE",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_kexec(lines, rel, funcs, out) -> None:
    """kexec_load()/kexec_file_load() return discarded (CWE-252). Not a bare kexec."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _KEXEC_DISCARDED)):
            if not _KEXEC_DISCARDED.match(ln):
                continue
            fname = (
                "kexec_file_load" if _find_call_args(ln, "kexec_file_load")
                else "kexec_load"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-KEXEC",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_quotactl_fd(lines, rel, funcs, out) -> None:
    """quotactl_fd() return discarded (CWE-252). Not quotactl()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _QUOTACTL_FD_DISCARDED)):
            if not _QUOTACTL_FD_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-QUOTACTL-FD",
                message="quotactl_fd() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pkey_free(lines, rel, funcs, out) -> None:
    """pkey_free()/pkey_mprotect() return discarded (CWE-252). Not pkey_alloc."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PKEY_FREE_DISCARDED)):
            if not _PKEY_FREE_DISCARDED.match(ln):
                continue
            fname = (
                "pkey_mprotect" if _find_call_args(ln, "pkey_mprotect")
                else "pkey_free"
            )
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PKEY-FREE",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_tgkill(lines, rel, funcs, out) -> None:
    """tgkill() return discarded (CWE-252). Not kill()/tkill()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _TGKILL_DISCARDED)):
            if not _TGKILL_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-TGKILL",
                message="tgkill() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_add_key(lines, rel, funcs, out) -> None:
    """add_key() return discarded (CWE-252). Not keyctl()/request_key()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _ADD_KEY_DISCARDED)):
            if not _ADD_KEY_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-ADD-KEY",
                message="add_key() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_semctl(lines, rel, funcs, out) -> None:
    """semctl() return discarded (CWE-252). Not semget()/semop()/sem_wait()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SEMCTL_DISCARDED)):
            if not _SEMCTL_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SEMCTL",
                message="semctl() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_msgctl(lines, rel, funcs, out) -> None:
    """msgctl() return discarded (CWE-252). Not msgget()/msgsnd()/mq_open()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MSGCTL_DISCARDED)):
            if not _MSGCTL_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MSGCTL",
                message="msgctl() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_io_setup(lines, rel, funcs, out) -> None:
    """io_setup()/io_destroy()/io_cancel()/io_pgetevents() discarded (CWE-252).

    Not io_submit()/io_getevents()/io_uring_*/aio_read().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _IO_SETUP_DISCARDED)):
            m = _IO_SETUP_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-IO-SETUP",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_request_key(lines, rel, funcs, out) -> None:
    """request_key() return discarded (CWE-252). Not keyctl()/add_key()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _REQUEST_KEY_DISCARDED)):
            if not _REQUEST_KEY_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-REQUEST-KEY",
                message="request_key() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_tkill(lines, rel, funcs, out) -> None:
    """tkill() return discarded (CWE-252). Not tgkill()/kill()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _TKILL_DISCARDED)):
            if not _TKILL_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-TKILL",
                message="tkill() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_timer_delete(lines, rel, funcs, out) -> None:
    """timer_delete()/timer_gettime()/timer_getoverrun() discarded (CWE-252).

    Not timer_create()/timer_settime()/timerfd_*.
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _TIMER_DELETE_DISCARDED)):
            m = _TIMER_DELETE_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-TIMER-DELETE",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_mq_unlink(lines, rel, funcs, out) -> None:
    """mq_unlink()/mq_timedsend()/mq_timedreceive()/mq_notify()/mq_getsetattr()
    discarded (CWE-252). Not mq_open()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MQ_UNLINK_DISCARDED)):
            m = _MQ_UNLINK_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MQ-UNLINK",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_shmat(lines, rel, funcs, out) -> None:
    """shmat()/shmdt() return discarded (CWE-252). Not shmget()/shmctl()/shm_open()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SHMAT_DISCARDED)):
            m = _SHMAT_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SHMAT",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_semop(lines, rel, funcs, out) -> None:
    """semop()/semtimedop() return discarded (CWE-252). Not semget()/semctl()/sem_wait()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SEMOP_DISCARDED)):
            m = _SEMOP_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SEMOP",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_msgsnd(lines, rel, funcs, out) -> None:
    """msgsnd()/msgrcv() return discarded (CWE-252). Not msgget()/msgctl()/mq_open()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MSGSND_DISCARDED)):
            m = _MSGSND_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MSGSND",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sync_file_range(lines, rel, funcs, out) -> None:
    """sync_file_range() return discarded (CWE-252). Not fsync()/syncfs()/sync()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SYNC_FILE_RANGE_DISCARDED)):
            if not _SYNC_FILE_RANGE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SYNC-FILE-RANGE",
                message="sync_file_range() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_msync(lines, rel, funcs, out) -> None:
    """msync()/mremap() return discarded (CWE-252). Not mmap()/mprotect()/munmap()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MSYNC_DISCARDED)):
            m = _MSYNC_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MSYNC",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_socketpair(lines, rel, funcs, out) -> None:
    """socketpair() return discarded (CWE-252). Not socket()/pipe()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SOCKETPAIR_DISCARDED)):
            if not _SOCKETPAIR_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SOCKETPAIR",
                message="socketpair() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sysinfo(lines, rel, funcs, out) -> None:
    """sysinfo() return discarded (CWE-252). Not getrusage()/uname()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SYSINFO_DISCARDED)):
            if not _SYSINFO_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SYSINFO",
                message="sysinfo() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_clock_settime(lines, rel, funcs, out) -> None:
    """clock_settime()/clock_adjtime()/clock_nanosleep() return discarded (CWE-252).

    Not clock_gettime()/gettimeofday().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CLOCK_SETTIME_DISCARDED)):
            m = _CLOCK_SETTIME_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CLOCK-SETTIME",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_settimeofday(lines, rel, funcs, out) -> None:
    """settimeofday() return discarded (CWE-252). Not gettimeofday()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SETTIMEOFDAY_DISCARDED)):
            if not _SETTIMEOFDAY_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SETTIMEOFDAY",
                message="settimeofday() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_gettid(lines, rel, funcs, out) -> None:
    """gettid() return discarded (CWE-252). Not gettimeofday()/getpid()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETTID_DISCARDED)):
            if not _GETTID_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETTID",
                message="gettid() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sched_setscheduler(lines, rel, funcs, out) -> None:
    """sched_setscheduler()/getscheduler()/setparam()/getparam() discarded (CWE-252).

    Not sched_setaffinity()/sched_yield().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SCHED_SETSCHEDULER_DISCARDED)):
            m = _SCHED_SETSCHEDULER_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SCHED-SETSCHEDULER",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_setitimer(lines, rel, funcs, out) -> None:
    """setitimer()/getitimer() return discarded (CWE-252).

    Not timer_create()/timer_settime()/timerfd_*.
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SETITIMER_DISCARDED)):
            m = _SETITIMER_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SETITIMER",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_nice(lines, rel, funcs, out) -> None:
    """nice() return discarded (CWE-252). Not getpriority()/setpriority()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _NICE_DISCARDED)):
            if not _NICE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-NICE",
                message="nice() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_arch_prctl(lines, rel, funcs, out) -> None:
    """arch_prctl() return discarded (CWE-252). Not prctl()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _ARCH_PRCTL_DISCARDED)):
            if not _ARCH_PRCTL_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-ARCH-PRCTL",
                message="arch_prctl() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getdents(lines, rel, funcs, out) -> None:
    """getdents()/getdents64() return discarded (CWE-252). Not opendir()/readdir()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETDENTS_DISCARDED)):
            m = _GETDENTS_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETDENTS",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_utimensat(lines, rel, funcs, out) -> None:
    """utimensat()/futimens()/utimes() return discarded (CWE-252).

    Not a bare time()/clock_gettime().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _UTIMENSAT_DISCARDED)):
            m = _UTIMENSAT_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-UTIMENSAT",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_linkat(lines, rel, funcs, out) -> None:
    """linkat() return discarded (CWE-252). Not unlink()/symlink()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _LINKAT_DISCARDED)):
            if not _LINKAT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-LINKAT",
                message="linkat() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_mbind(lines, rel, funcs, out) -> None:
    """mbind()/set_mempolicy()/get_mempolicy() return discarded (CWE-252).

    Not mmap()/mprotect()/mlock().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MBIND_DISCARDED)):
            m = _MBIND_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MBIND",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_futex_waitv(lines, rel, funcs, out) -> None:
    """futex_waitv() return discarded (CWE-252). Not futex()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _FUTEX_WAITV_DISCARDED)):
            if not _FUTEX_WAITV_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FUTEX-WAITV",
                message="futex_waitv() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_syslog(lines, rel, funcs, out) -> None:
    """syslog() return discarded (CWE-252). Not klogctl()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SYSLOG_DISCARDED)):
            if not _SYSLOG_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SYSLOG",
                message="syslog() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_setpgid(lines, rel, funcs, out) -> None:
    """setpgid()/setsid()/getsid() return discarded (CWE-252).

    Not getpid()/setuid().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SETPGID_DISCARDED)):
            m = _SETPGID_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SETPGID",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_setreuid(lines, rel, funcs, out) -> None:
    """setreuid()/setregid()/setresuid()/setresgid() discarded (CWE-252).

    Not setuid()/seteuid()/setgid().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SETREUID_DISCARDED)):
            m = _SETREUID_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SETREUID",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getgroups(lines, rel, funcs, out) -> None:
    """getgroups() return discarded (CWE-252). Not initgroups()/setgroups()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETGROUPS_DISCARDED)):
            if not _GETGROUPS_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETGROUPS",
                message="getgroups() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_epoll_create(lines, rel, funcs, out) -> None:
    """epoll_create()/epoll_create1() return discarded (CWE-252).

    Not epoll_wait()/epoll_pwait()/epoll_ctl().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _EPOLL_CREATE_DISCARDED)):
            m = _EPOLL_CREATE_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-EPOLL-CREATE",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_timerfd_settime(lines, rel, funcs, out) -> None:
    """timerfd_settime()/timerfd_gettime() return discarded (CWE-252).

    Not timerfd_create()/timer_settime()/timer_create().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _TIMERFD_SETTIME_DISCARDED)):
            m = _TIMERFD_SETTIME_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-TIMERFD-SETTIME",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_remap_file_pages(lines, rel, funcs, out) -> None:
    """remap_file_pages() return discarded (CWE-252). Not mmap()/mprotect()/mremap()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _REMAP_FILE_PAGES_DISCARDED)):
            if not _REMAP_FILE_PAGES_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-REMAP-FILE-PAGES",
                message="remap_file_pages() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_move_pages(lines, rel, funcs, out) -> None:
    """migrate_pages()/move_pages() return discarded (CWE-252).

    Not process_vm_readv()/mbind().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MOVE_PAGES_DISCARDED)):
            m = _MOVE_PAGES_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MOVE-PAGES",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_cachestat(lines, rel, funcs, out) -> None:
    """cachestat() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CACHESTAT_DISCARDED)):
            if not _CACHESTAT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CACHESTAT",
                message="cachestat() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_map_shadow_stack(lines, rel, funcs, out) -> None:
    """map_shadow_stack() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MAP_SHADOW_STACK_DISCARDED)):
            if not _MAP_SHADOW_STACK_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MAP-SHADOW-STACK",
                message="map_shadow_stack() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sched_yield(lines, rel, funcs, out) -> None:
    """sched_yield() return discarded (CWE-252).

    Not sched_setaffinity()/sched_setscheduler()/sched_setattr().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SCHED_YIELD_DISCARDED)):
            if not _SCHED_YIELD_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SCHED-YIELD",
                message="sched_yield() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_setfsuid(lines, rel, funcs, out) -> None:
    """setfsuid()/setfsgid() return discarded (CWE-252). Not setuid()/setgid()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SETFSUID_DISCARDED)):
            m = _SETFSUID_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SETFSUID",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_wait4(lines, rel, funcs, out) -> None:
    """wait4()/wait3() return discarded (CWE-252). Not wait()/waitpid()/waitid()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _WAIT4_DISCARDED)):
            m = _WAIT4_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-WAIT4",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_preadv(lines, rel, funcs, out) -> None:
    """preadv2/pwritev2/preadv/pwritev return discarded (CWE-252).

    Not pread()/pwrite()/read()/write()/readv().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PREADV_DISCARDED)):
            m = _PREADV_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PREADV",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sendmsg(lines, rel, funcs, out) -> None:
    """sendmsg()/recvmsg() return discarded (CWE-252).

    Not send()/recv()/sendto()/sendmmsg().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SENDMSG_DISCARDED)):
            m = _SENDMSG_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SENDMSG",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getsockname(lines, rel, funcs, out) -> None:
    """getsockname()/getpeername() return discarded (CWE-252).

    Not accept()/bind()/socket().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETSOCKNAME_DISCARDED)):
            m = _GETSOCKNAME_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETSOCKNAME",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_epoll_pwait(lines, rel, funcs, out) -> None:
    """epoll_pwait()/epoll_pwait2() return discarded (CWE-252).

    Not epoll_wait()/epoll_create()/epoll_ctl().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _EPOLL_PWAIT_DISCARDED)):
            m = _EPOLL_PWAIT_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-EPOLL-PWAIT",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_inotify_rm_watch(lines, rel, funcs, out) -> None:
    """inotify_rm_watch() return discarded (CWE-252).

    Not inotify_init()/inotify_add_watch().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _INOTIFY_RM_WATCH_DISCARDED)):
            if not _INOTIFY_RM_WATCH_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-INOTIFY-RM-WATCH",
                message="inotify_rm_watch() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_eventfd_read(lines, rel, funcs, out) -> None:
    """eventfd_read()/eventfd_write() return discarded (CWE-252).

    Not eventfd()/eventfd_create().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _EVENTFD_RW_DISCARDED)):
            m = _EVENTFD_RW_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-EVENTFD-READ",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sched_setattr(lines, rel, funcs, out) -> None:
    """sched_setattr()/sched_getattr() return discarded (CWE-252).

    Not sched_setscheduler()/sched_yield()/sched_setaffinity()/sched_setparam().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SCHED_SETATTR_DISCARDED)):
            m = _SCHED_SETATTR_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SCHED-SETATTR",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_renameat2(lines, rel, funcs, out) -> None:
    """renameat2() return discarded (CWE-252). Not rename()/renameat()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _RENAMEAT2_DISCARDED)):
            if not _RENAMEAT2_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-RENAMEAT2",
                message="renameat2() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_execveat(lines, rel, funcs, out) -> None:
    """execveat() return discarded (CWE-252). Not execve()/execl()/execvp()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _EXECVEAT_DISCARDED)):
            if not _EXECVEAT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-EXECVEAT",
                message="execveat() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_mlock2(lines, rel, funcs, out) -> None:
    """mlock2() return discarded (CWE-252). Not mlock()/mlockall()/munlock()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MLOCK2_DISCARDED)):
            if not _MLOCK2_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MLOCK2",
                message="mlock2() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_faccessat2(lines, rel, funcs, out) -> None:
    """faccessat2() return discarded (CWE-252). Not access()/faccessat()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _FACCESSAT2_DISCARDED)):
            if not _FACCESSAT2_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FACCESSAT2",
                message="faccessat2() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_posix_fadvise(lines, rel, funcs, out) -> None:
    """posix_fadvise()/posix_fadvise64() return discarded (CWE-252).

    Not posix_madvise()/madvise().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _POSIX_FADVISE_DISCARDED)):
            m = _POSIX_FADVISE_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-POSIX-FADVISE",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_readahead(lines, rel, funcs, out) -> None:
    """readahead() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _READAHEAD_DISCARDED)):
            if not _READAHEAD_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-READAHEAD",
                message="readahead() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sigaction(lines, rel, funcs, out) -> None:
    """sigaction() return discarded (CWE-252). Not signal()/signalfd()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SIGACTION_DISCARDED)):
            if not _SIGACTION_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SIGACTION",
                message="sigaction() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sigprocmask(lines, rel, funcs, out) -> None:
    """sigprocmask()/sigsuspend() return discarded (CWE-252).

    Not sigaction()/signal()/pthread_sigmask().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SIGPROCMASK_DISCARDED)):
            m = _SIGPROCMASK_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SIGPROCMASK",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sem_open(lines, rel, funcs, out) -> None:
    """sem_open()/sem_close()/sem_unlink() return discarded (CWE-252).

    Not sem_wait()/sem_post()/sem_init()/semget()/semctl()/semop().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SEM_OPEN_DISCARDED)):
            m = _SEM_OPEN_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SEM-OPEN",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_rwlock(lines, rel, funcs, out) -> None:
    """pthread_rwlock_* return discarded (CWE-252).

    Not pthread_mutex/pthread_join/pthread_spin.
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _RWLOCK_DISCARDED)):
            m = _RWLOCK_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-RWLOCK",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pthread_cond(lines, rel, funcs, out) -> None:
    """pthread_cond_* return discarded (CWE-252). Not C++ condition_variable."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PTHREAD_COND_DISCARDED)):
            m = _PTHREAD_COND_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PTHREAD-COND",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sigaltstack(lines, rel, funcs, out) -> None:
    """sigaltstack() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SIGALTSTACK_DISCARDED)):
            if not _SIGALTSTACK_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SIGALTSTACK",
                message="sigaltstack() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_renameat(lines, rel, funcs, out) -> None:
    """renameat() return discarded (CWE-252). Not renameat2()/rename()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _RENAMEAT_DISCARDED)):
            if not _RENAMEAT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-RENAMEAT",
                message="renameat() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_faccessat(lines, rel, funcs, out) -> None:
    """faccessat() return discarded (CWE-252). Not faccessat2()/access()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _FACCESSAT_DISCARDED)):
            if not _FACCESSAT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FACCESSAT",
                message="faccessat() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_fchmodat(lines, rel, funcs, out) -> None:
    """fchmodat() return discarded (CWE-252). Not fchmodat2()/chmod()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _FCHMODAT_DISCARDED)):
            if not _FCHMODAT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FCHMODAT",
                message="fchmodat() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pthread_barrier(lines, rel, funcs, out) -> None:
    """pthread_barrier_* return discarded (CWE-252). Not C++ std::barrier."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PTHREAD_BARRIER_DISCARDED)):
            m = _PTHREAD_BARRIER_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PTHREAD-BARRIER",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_symlinkat(lines, rel, funcs, out) -> None:
    """symlinkat() return discarded (CWE-252). Not symlink()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SYMLINKAT_DISCARDED)):
            if not _SYMLINKAT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SYMLINKAT",
                message="symlinkat() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_unlinkat(lines, rel, funcs, out) -> None:
    """unlinkat() return discarded (CWE-252). Not unlink()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _UNLINKAT_DISCARDED)):
            if not _UNLINKAT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-UNLINKAT",
                message="unlinkat() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_mkdirat(lines, rel, funcs, out) -> None:
    """mkdirat() return discarded (CWE-252). Not mkdir()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MKDIRAT_DISCARDED)):
            if not _MKDIRAT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MKDIRAT",
                message="mkdirat() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_mknodat(lines, rel, funcs, out) -> None:
    """mknodat() return discarded (CWE-252). Not mknod()/mkfifo()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MKNODAT_DISCARDED)):
            if not _MKNODAT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MKNODAT",
                message="mknodat() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_readlinkat(lines, rel, funcs, out) -> None:
    """readlinkat() return discarded (CWE-252). Not readlink()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _READLINKAT_DISCARDED)):
            if not _READLINKAT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-READLINKAT",
                message="readlinkat() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_fstatat(lines, rel, funcs, out) -> None:
    """fstatat() return discarded (CWE-252). Not fstat()/stat()/statx()/statfs()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _FSTATAT_DISCARDED)):
            if not _FSTATAT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FSTATAT",
                message="fstatat() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pthread_spin(lines, rel, funcs, out) -> None:
    """pthread_spin_* return discarded (CWE-252). Not pthread_mutex/rwlock."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PTHREAD_SPIN_DISCARDED)):
            m = _PTHREAD_SPIN_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PTHREAD-SPIN",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pthread_key(lines, rel, funcs, out) -> None:
    """pthread_key_create/delete/setspecific/getspecific return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PTHREAD_KEY_DISCARDED)):
            m = _PTHREAD_KEY_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PTHREAD-KEY",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pthread_cancel(lines, rel, funcs, out) -> None:
    """pthread_cancel() return discarded (CWE-252). Not pthread_create/join."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PTHREAD_CANCEL_DISCARDED)):
            if not _PTHREAD_CANCEL_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PTHREAD-CANCEL",
                message="pthread_cancel() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pthread_kill(lines, rel, funcs, out) -> None:
    """pthread_kill() return discarded (CWE-252). Not POSIX kill()/raise()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PTHREAD_KILL_DISCARDED)):
            if not _PTHREAD_KILL_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PTHREAD-KILL",
                message="pthread_kill() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pthread_sigmask(lines, rel, funcs, out) -> None:
    """pthread_sigmask() return discarded (CWE-252). Not sigprocmask()/sigaction()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PTHREAD_SIGMASK_DISCARDED)):
            if not _PTHREAD_SIGMASK_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PTHREAD-SIGMASK",
                message="pthread_sigmask() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pthread_atfork(lines, rel, funcs, out) -> None:
    """pthread_atfork() return discarded (CWE-252). Not fork()/vfork()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PTHREAD_ATFORK_DISCARDED)):
            if not _PTHREAD_ATFORK_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PTHREAD-ATFORK",
                message="pthread_atfork() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pledge(lines, rel, funcs, out) -> None:
    """pledge() return discarded (CWE-252/250/273)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PLEDGE_DISCARDED)):
            if not _PLEDGE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PLEDGE",
                message="pledge() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_unveil(lines, rel, funcs, out) -> None:
    """unveil() return discarded (CWE-252/250/273)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _UNVEIL_DISCARDED)):
            if not _UNVEIL_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-UNVEIL",
                message="unveil() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sysctl(lines, rel, funcs, out) -> None:
    """sysctl()/sysctlbyname() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SYSCTL_DISCARDED)):
            m = _SYSCTL_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SYSCTL",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_kqueue(lines, rel, funcs, out) -> None:
    """kqueue() result used without a <0 test or discarded (CWE-252). Not kevent()."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[int] = set()
        for i, ln in enumerate(fn.body.splitlines()):
            if _KQUEUE_DISCARDED.match(ln):
                line = start + i
                seen.add(i)
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="API-KQUEUE",
                    message="kqueue() return is discarded",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                continue
            m = _KQUEUE_ASSIGN.search(ln)
            if not m or i in seen:
                continue
            var = m.group("var")
            if _lt_zero_test_for(var, ln):
                continue
            use_j = _first_fd_use(var, chunk, i, skip_callees=("kqueue",))
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _lt_zero_test_for(var, between):
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-KQUEUE",
                message=f"{var} from kqueue() used without a < 0 test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_kevent(lines, rel, funcs, out) -> None:
    """kevent() return discarded (CWE-252). Not kqueue()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _KEVENT_DISCARDED)):
            if not _KEVENT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-KEVENT",
                message="kevent() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pause(lines, rel, funcs, out) -> None:
    """pause() return discarded (CWE-252). Not sleep()/nanosleep()/pselect()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PAUSE_DISCARDED)):
            if not _PAUSE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PAUSE",
                message="pause() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_ppoll(lines, rel, funcs, out) -> None:
    """ppoll() return discarded (CWE-252). Not poll()/pselect()/epoll()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PPOLL_DISCARDED)):
            if not _PPOLL_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PPOLL",
                message="ppoll() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sigwait(lines, rel, funcs, out) -> None:
    """sigwait/sigwaitinfo/sigtimedwait/sigpending return discarded (CWE-252).

    Not sigaction()/sigprocmask()/signalfd().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SIGWAIT_DISCARDED)):
            m = _SIGWAIT_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SIGWAIT",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sigqueue(lines, rel, funcs, out) -> None:
    """sigqueue() return discarded (CWE-252). Not rt_sigqueueinfo()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SIGQUEUE_DISCARDED)):
            if not _SIGQUEUE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SIGQUEUE",
                message="sigqueue() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_ucontext(lines, rel, funcs, out) -> None:
    """getcontext/setcontext/swapcontext/makecontext return discarded (CWE-252).

    Not setjmp()/longjmp().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _UCONTEXT_DISCARDED)):
            m = _UCONTEXT_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-UCONTEXT",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sem_timedwait(lines, rel, funcs, out) -> None:
    """sem_timedwait() return discarded (CWE-252). Not sem_wait()/sem_open()/semget()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SEM_TIMEDWAIT_DISCARDED)):
            if not _SEM_TIMEDWAIT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SEM-TIMEDWAIT",
                message="sem_timedwait() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pthread_attr(lines, rel, funcs, out) -> None:
    """pthread_attr_init/destroy/setstack*/setdetachstate return discarded (CWE-252).

    Not pthread_create()/pthread_join()/pthread_atfork().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PTHREAD_ATTR_DISCARDED)):
            m = _PTHREAD_ATTR_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PTHREAD-ATTR",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_cap_enter(lines, rel, funcs, out) -> None:
    """cap_enter() return discarded (CWE-252/250/273). Not pledge()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CAP_ENTER_DISCARDED)):
            if not _CAP_ENTER_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CAP-ENTER",
                message="cap_enter() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_cap_rights(lines, rel, funcs, out) -> None:
    """cap_rights_limit()/cap_rights_get() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CAP_RIGHTS_DISCARDED)):
            m = _CAP_RIGHTS_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CAP-RIGHTS",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pdfork(lines, rel, funcs, out) -> None:
    """pdfork() return discarded (CWE-252). Not fork()/vfork()/pthread_atfork()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PDFORK_DISCARDED)):
            if not _PDFORK_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PDFORK",
                message="pdfork() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_procctl(lines, rel, funcs, out) -> None:
    """procctl() return discarded (CWE-252). Not prctl()/ptrace()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PROCCTL_DISCARDED)):
            if not _PROCCTL_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PROCCTL",
                message="procctl() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_closefrom(lines, rel, funcs, out) -> None:
    """closefrom() return discarded (CWE-252). Not close_range()/close()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CLOSEFROM_DISCARDED)):
            if not _CLOSEFROM_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CLOSEFROM",
                message="closefrom() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_issetugid(lines, rel, funcs, out) -> None:
    """issetugid() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _ISSETUGID_DISCARDED)):
            if not _ISSETUGID_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-ISSETUGID",
                message="issetugid() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_arc4random(lines, rel, funcs, out) -> None:
    """arc4random()/arc4random_buf()/arc4random_uniform() return discarded (CWE-252/330).

    Not getrandom()/getentropy()/rand().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _ARC4RANDOM_DISCARDED)):
            m = _ARC4RANDOM_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-ARC4RANDOM",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_chflags(lines, rel, funcs, out) -> None:
    """chflags()/fchflags()/lchflags() return discarded (CWE-252). Not chmod()/chown()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CHFLAGS_DISCARDED)):
            m = _CHFLAGS_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CHFLAGS",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getfsstat(lines, rel, funcs, out) -> None:
    """getfsstat() return discarded (CWE-252). Not statfs()/fstatfs()/stat()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETFSSTAT_DISCARDED)):
            if not _GETFSSTAT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETFSSTAT",
                message="getfsstat() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pthread_yield(lines, rel, funcs, out) -> None:
    """pthread_yield() return discarded (CWE-252). Not sched_yield()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PTHREAD_YIELD_DISCARDED)):
            if not _PTHREAD_YIELD_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PTHREAD-YIELD",
                message="pthread_yield() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sem_trywait(lines, rel, funcs, out) -> None:
    """sem_trywait()/sem_getvalue() return discarded (CWE-252).

    Not sem_wait()/sem_timedwait()/sem_open().
    """
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SEM_TRYWAIT_DISCARDED)):
            m = _SEM_TRYWAIT_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SEM-TRYWAIT",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_adjtime(lines, rel, funcs, out) -> None:
    """adjtime()/ntp_adjtime() return discarded (CWE-252). Not adjtimex()/clock_adjtime()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _ADJTIME_DISCARDED)):
            m = _ADJTIME_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-ADJTIME",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_revoke(lines, rel, funcs, out) -> None:
    """revoke() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _REVOKE_DISCARDED)):
            if not _REVOKE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-REVOKE",
                message="revoke() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_ktrace(lines, rel, funcs, out) -> None:
    """ktrace() return discarded (CWE-252). Not ptrace()/prctl()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _KTRACE_DISCARDED)):
            if not _KTRACE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-KTRACE",
                message="ktrace() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_rfork(lines, rel, funcs, out) -> None:
    """rfork() return discarded (CWE-252). Not fork()/vfork()/pdfork()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _RFORK_DISCARDED)):
            if not _RFORK_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-RFORK",
                message="rfork() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_jail(lines, rel, funcs, out) -> None:
    """jail()/jail_attach()/jail_get()/jail_set()/jail_remove() discarded (CWE-252/250). Not chroot()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _JAIL_DISCARDED)):
            m = _JAIL_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-JAIL",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_setlogin(lines, rel, funcs, out) -> None:
    """setlogin() return discarded (CWE-252). Not getlogin()/getlogin_r()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SETLOGIN_DISCARDED)):
            if not _SETLOGIN_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SETLOGIN",
                message="setlogin() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getresuid(lines, rel, funcs, out) -> None:
    """getresuid()/getresgid() return discarded (CWE-252). Not setresuid()/setreuid()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETRESUID_DISCARDED)):
            m = _GETRESUID_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETRESUID",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getpeereid(lines, rel, funcs, out) -> None:
    """getpeereid() return discarded (CWE-252). Not getpeername()/getsockname()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETPEEREID_DISCARDED)):
            if not _GETPEEREID_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETPEEREID",
                message="getpeereid() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_strtonum(lines, rel, funcs, out) -> None:
    """strtonum() return discarded (CWE-252). Not strtol()/atoi()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _STRTONUM_DISCARDED)):
            if not _STRTONUM_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-STRTONUM",
                message="strtonum() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_reallocarray(lines, rel, funcs, out) -> None:
    """reallocarray() discarded or used without NULL (CWE-252/190/680). Not realloc()/reallocf()."""
    skip = ("reallocarray",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[int] = set()
        for i, ln in enumerate(chunk):
            if _REALLOCARRAY_DISCARDED.match(ln):
                if i in seen:
                    continue
                seen.add(i)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="API-REALLOCARRAY",
                    message="reallocarray() return is discarded",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                continue
            m = _REALLOCARRAY_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            if i in seen:
                continue
            if _null_test_for(var, ln):
                continue
            use_j = _first_ptr_or_arg_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _null_test_for(var, between):
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-REALLOCARRAY",
                message=f"{var} from reallocarray() used without a NULL check",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_timingsafe(lines, rel, funcs, out) -> None:
    """timingsafe_bcmp()/timingsafe_memcmp() return discarded (CWE-252/208). Not memcmp()/explicit_bzero()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _TIMINGSAFE_DISCARDED)):
            m = _TIMINGSAFE_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-TIMINGSAFE",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getprogname(lines, rel, funcs, out) -> None:
    """getprogname()/setprogname() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETPROGNAME_DISCARDED)):
            m = _GETPROGNAME_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETPROGNAME",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_daemon(lines, rel, funcs, out) -> None:
    """daemon()/setproctitle() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _DAEMON_DISCARDED)):
            m = _DAEMON_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-DAEMON",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_cap_fcntls(lines, rel, funcs, out) -> None:
    """cap_fcntls_limit()/cap_ioctls_limit() discarded (CWE-252). Not cap_enter()/cap_rights_limit()/fcntl()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CAP_FCNTLS_DISCARDED)):
            m = _CAP_FCNTLS_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CAP-FCNTLS",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_pdgetpid(lines, rel, funcs, out) -> None:
    """pdgetpid()/pdwait4() return discarded (CWE-252). Not pdfork()/wait4()/getpid()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _PDGETPID_DISCARDED)):
            m = _PDGETPID_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-PDGETPID",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_kldload(lines, rel, funcs, out) -> None:
    """kldload()/kldunload()/kldfind()/kldsym()/kldstat() discarded (CWE-252/114). Not dlopen()/init_module()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _KLDLOAD_DISCARDED)):
            m = _KLDLOAD_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-KLDLOAD",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_extattr(lines, rel, funcs, out) -> None:
    """extattr_{set,get,delete,list}_{file,fd,link}() discarded (CWE-252). Not setxattr()/getxattr()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _EXTATTR_DISCARDED)):
            m = _EXTATTR_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-EXTATTR",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_mac(lines, rel, funcs, out) -> None:
    """mac_{set,get}_{proc,fd,file}() return discarded (CWE-252). Not pledge()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MAC_DISCARDED)):
            m = _MAC_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MAC",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_audit(lines, rel, funcs, out) -> None:
    """auditon()/getaudit()/setaudit()/auditctl() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _AUDIT_DISCARDED)):
            m = _AUDIT_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-AUDIT",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_kvm(lines, rel, funcs, out) -> None:
    """kvm_open()/kvm_openfiles()/kvm_getprocs()/kvm_close()/kvm_nlist() discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _KVM_DISCARDED)):
            m = _KVM_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-KVM",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_reallocf(lines, rel, funcs, out) -> None:
    """reallocf() discarded or used without NULL (CWE-252/401/680). Not realloc()/reallocarray()."""
    skip = ("reallocf",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[int] = set()
        for i, ln in enumerate(chunk):
            if _REALLOCF_DISCARDED.match(ln):
                if i in seen:
                    continue
                seen.add(i)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="API-REALLOCF",
                    message="reallocf() return is discarded",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                continue
            m = _REALLOCF_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            if i in seen:
                continue
            if _null_test_for(var, ln):
                continue
            use_j = _first_ptr_or_arg_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _null_test_for(var, between):
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-REALLOCF",
                message=f"{var} from reallocf() used without a NULL check",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_uuidgen(lines, rel, funcs, out) -> None:
    """uuidgen() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _UUIDGEN_DISCARDED)):
            if not _UUIDGEN_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-UUIDGEN",
                message="uuidgen() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_setfib(lines, rel, funcs, out) -> None:
    """setfib() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SETFIB_DISCARDED)):
            if not _SETFIB_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SETFIB",
                message="setfib() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_ntp_gettime(lines, rel, funcs, out) -> None:
    """ntp_gettime() return discarded (CWE-252). Not ntp_adjtime()/adjtime()/gettimeofday()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _NTP_GETTIME_DISCARDED)):
            if not _NTP_GETTIME_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-NTP-GETTIME",
                message="ntp_gettime() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_crypt_newhash(lines, rel, funcs, out) -> None:
    """crypt_newhash()/crypt_checkpass() discarded (CWE-252/916). Not bare crypt()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CRYPT_NEWHASH_DISCARDED)):
            m = _CRYPT_NEWHASH_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CRYPT-NEWHASH",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_wait6(lines, rel, funcs, out) -> None:
    """wait6() return discarded (CWE-252). Not wait()/wait4()/waitpid()/pdwait4()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _WAIT6_DISCARDED)):
            if not _WAIT6_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-WAIT6",
                message="wait6() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_cpuset(lines, rel, funcs, out) -> None:
    """cpuset_{set,get}affinity() discarded (CWE-252). Not sched_setaffinity()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CPUSET_DISCARDED)):
            m = _CPUSET_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CPUSET",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_rtprio(lines, rel, funcs, out) -> None:
    """rtprio()/rtprio_thread() discarded (CWE-252). Not nice()/getpriority()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _RTPRIO_DISCARDED)):
            m = _RTPRIO_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-RTPRIO",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_kenv(lines, rel, funcs, out) -> None:
    """kenv() return discarded (CWE-252/526). Not getenv()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _KENV_DISCARDED)):
            if not _KENV_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-KENV",
                message="kenv() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getfh(lines, rel, funcs, out) -> None:
    """getfh()/fhopen()/fhstat()/fhstatfs()/getfhat() discarded (CWE-252). Not open()/stat()/statfs()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETFH_DISCARDED)):
            m = _GETFH_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETFH",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getmntinfo(lines, rel, funcs, out) -> None:
    """getmntinfo() return discarded (CWE-252). Not getfsstat()/statfs()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETMNTINFO_DISCARDED)):
            if not _GETMNTINFO_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETMNTINFO",
                message="getmntinfo() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_nmount(lines, rel, funcs, out) -> None:
    """nmount() return discarded (CWE-252). Not mount()/umount()/listmount()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _NMOUNT_DISCARDED)):
            if not _NMOUNT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-NMOUNT",
                message="nmount() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_strmode(lines, rel, funcs, out) -> None:
    """strmode() return discarded (CWE-252). Not strtonum()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _STRMODE_DISCARDED)):
            if not _STRMODE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-STRMODE",
                message="strmode() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getosreldate(lines, rel, funcs, out) -> None:
    """getosreldate() return discarded (CWE-252). Not uname()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETOSRELDATE_DISCARDED)):
            if not _GETOSRELDATE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETOSRELDATE",
                message="getosreldate() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_cap_sandboxed(lines, rel, funcs, out) -> None:
    """cap_sandboxed() return discarded (CWE-252). Not cap_enter()/cap_fcntls_limit()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CAP_SANDBOXED_DISCARDED)):
            if not _CAP_SANDBOXED_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CAP-SANDBOXED",
                message="cap_sandboxed() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getgrouplist(lines, rel, funcs, out) -> None:
    """getgrouplist() return discarded (CWE-252). Not getgroups()/initgroups()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETGROUPLIST_DISCARDED)):
            if not _GETGROUPLIST_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETGROUPLIST",
                message="getgrouplist() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_eaccess(lines, rel, funcs, out) -> None:
    """eaccess() return discarded (CWE-252/284). Not access()/faccessat()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _EACCESS_DISCARDED)):
            if not _EACCESS_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-EACCESS",
                message="eaccess() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_login_getclass(lines, rel, funcs, out) -> None:
    """login_getclass()/setusercontext() discarded (CWE-252). Not getlogin()/setlogin()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _LOGIN_GETCLASS_DISCARDED)):
            m = _LOGIN_GETCLASS_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-LOGIN-GETCLASS",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_fflags(lines, rel, funcs, out) -> None:
    """fflagstostr()/strtofflags() discarded (CWE-252). Not strmode()/strtonum()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _FFLAGS_DISCARDED)):
            m = _FFLAGS_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FFLAGS",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getdirentries(lines, rel, funcs, out) -> None:
    """getdirentries() return discarded (CWE-252). Not getdents()/readdir()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETDIRENTRIES_DISCARDED)):
            if not _GETDIRENTRIES_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETDIRENTRIES",
                message="getdirentries() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_kinfo(lines, rel, funcs, out) -> None:
    """kinfo_getproc()/kinfo_getfile() discarded (CWE-252). Not kvm_open()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _KINFO_DISCARDED)):
            m = _KINFO_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-KINFO",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_umtx(lines, rel, funcs, out) -> None:
    """_umtx_op() return discarded (CWE-252). Not futex()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _UMTX_DISCARDED)):
            if not _UMTX_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-UMTX",
                message="_umtx_op() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_thr(lines, rel, funcs, out) -> None:
    """thr_new()/thr_kill()/thr_kill2() discarded (CWE-252). Not pthread_kill()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _THR_DISCARDED)):
            m = _THR_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-THR",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_modfind(lines, rel, funcs, out) -> None:
    """modfind()/modstat()/modnext() discarded (CWE-252). Not kldload()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MODFIND_DISCARDED)):
            m = _MODFIND_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MODFIND",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_lpathconf(lines, rel, funcs, out) -> None:
    """lpathconf() return discarded (CWE-252). Not pathconf()/fpathconf()/sysconf()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _LPATHCONF_DISCARDED)):
            if not _LPATHCONF_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-LPATHCONF",
                message="lpathconf() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_loginclass(lines, rel, funcs, out) -> None:
    """getloginclass()/setloginclass() discarded (CWE-252). Not getlogin()/login_getclass()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _LOGINCLASS_DISCARDED)):
            m = _LOGINCLASS_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-LOGINCLASS",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getfsent(lines, rel, funcs, out) -> None:
    """getfsent()/setfsent()/endfsent() discarded (CWE-252). Not getfsstat()/getmntinfo()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETFSENT_DISCARDED)):
            m = _GETFSENT_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETFSENT",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_minherit(lines, rel, funcs, out) -> None:
    """minherit() return discarded (CWE-252). Not mmap()/mprotect()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MINHERIT_DISCARDED)):
            if not _MINHERIT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MINHERIT",
                message="minherit() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_cap_getmode(lines, rel, funcs, out) -> None:
    """cap_getmode() return discarded (CWE-252). Not cap_enter()/cap_sandboxed()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CAP_GETMODE_DISCARDED)):
            if not _CAP_GETMODE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CAP-GETMODE",
                message="cap_getmode() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_nfssvc(lines, rel, funcs, out) -> None:
    """nfssvc() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _NFSSVC_DISCARDED)):
            if not _NFSSVC_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-NFSSVC",
                message="nfssvc() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sysarch(lines, rel, funcs, out) -> None:
    """sysarch() return discarded (CWE-252). Not syscall()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SYSARCH_DISCARDED)):
            if not _SYSARCH_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SYSARCH",
                message="sysarch() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getpagesizes(lines, rel, funcs, out) -> None:
    """getpagesizes() return discarded (CWE-252). Not getpagesize()/sysconf()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETPAGESIZES_DISCARDED)):
            if not _GETPAGESIZES_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETPAGESIZES",
                message="getpagesizes() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_sbrk(lines, rel, funcs, out) -> None:
    """sbrk()/brk() return discarded (CWE-252/770). Not abort(); word-bounded brk."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _SBRK_DISCARDED)):
            m = _SBRK_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-SBRK",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_ksem(lines, rel, funcs, out) -> None:
    """ksem_open()/ksem_close()/ksem_unlink() discarded (CWE-252). Not sem_open()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _KSEM_DISCARDED)):
            m = _KSEM_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-KSEM",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_cap_getrights(lines, rel, funcs, out) -> None:
    """cap_getrights() return discarded (CWE-252). Not cap_rights_get()/cap_rights_limit()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CAP_GETRIGHTS_DISCARDED)):
            if not _CAP_GETRIGHTS_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CAP-GETRIGHTS",
                message="cap_getrights() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_devname(lines, rel, funcs, out) -> None:
    """devname()/devname_r() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _DEVNAME_DISCARDED)):
            m = _DEVNAME_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-DEVNAME",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getbootfile(lines, rel, funcs, out) -> None:
    """getbootfile() return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETBOOTFILE_DISCARDED)):
            if not _GETBOOTFILE_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETBOOTFILE",
                message="getbootfile() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_kldfirstmod(lines, rel, funcs, out) -> None:
    """kldfirstmod()/kldnextmod() discarded (CWE-252). Not kldload()/modfind()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _KLDFIRSTMOD_DISCARDED)):
            m = _KLDFIRSTMOD_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-KLDFIRSTMOD",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_fhlink(lines, rel, funcs, out) -> None:
    """fhlink()/fhlinkat()/fhreadlink() discarded (CWE-252). Not link()/linkat()/getfh()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _FHLINK_DISCARDED)):
            m = _FHLINK_DISCARDED.match(ln)
            if not m:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-FHLINK",
                message=f"{m.group('fn')}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_valloc(lines, rel, funcs, out) -> None:
    """valloc() discarded or used without NULL (CWE-252/590). Not posix_memalign()/aligned_alloc()/malloc()."""
    skip = ("valloc",)
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[int] = set()
        for i, ln in enumerate(chunk):
            if _VALLOC_DISCARDED.match(ln):
                if i in seen:
                    continue
                seen.add(i)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="API-VALLOC",
                    message="valloc() return is discarded",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                continue
            m = _VALLOC_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            if i in seen:
                continue
            if _null_test_for(var, ln):
                continue
            use_j = _first_ptr_or_arg_use(var, chunk, i, skip_callees=skip)
            if use_j is None:
                continue
            between = "\n".join(chunk[i:use_j + 1])
            if _null_test_for(var, between):
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-VALLOC",
                message=f"{var} from valloc() used without a NULL check",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_getdomainname(lines, rel, funcs, out) -> None:
    """getdomainname() return discarded (CWE-252). Not setdomainname()/gethostname()/uname()."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _GETDOMAINNAME_DISCARDED)):
            if not _GETDOMAINNAME_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-GETDOMAINNAME",
                message="getdomainname() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _explicit_nul_store_for(var: str, text: str) -> bool:
    """True if `text` writes `var[n-1]=0` or `var[i]='\\0'`."""
    v = re.escape(var)
    return bool(re.search(
        rf"\b{v}\s*\[\s*[^\]]+\s*\]\s*=\s*(?:0\b|'\\0')",
        text,
    ))


def _str_strncpy_nul(lines, rel, funcs, out) -> None:
    """strncpy into a local char array with no following explicit NUL store.

    Distinct from STR-OFF-BY-ONE (n == sizeof dst) and STR-MISSING-NUL
    (memcpy of a string literal). The ok twin writes the terminator.
    """
    for fn in funcs:
        arrays = _local_char_array_sizes(fn)
        if not arrays:
            continue
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[int] = set()
        for i, ln in enumerate(chunk):
            args = _find_call_args(ln, "strncpy")
            if args is None or len(args) < 1:
                continue
            dst = _rx(r"\s+").sub("", args[0])
            if dst not in arrays:
                continue
            following = "\n".join(chunk[i + 1:])
            if _explicit_nul_store_for(dst, following):
                continue
            if i in seen:
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="STR-STRNCPY-NUL",
                message=f"strncpy({dst}, …) with no following explicit NUL store",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _str_snprintf(lines, rel, funcs, out) -> None:
    """snprintf size literal larger than the local destination array (CWE-120)."""
    for fn in funcs:
        arrays = _local_char_array_sizes(fn)
        if not arrays:
            continue
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[int] = set()
        for i, ln in enumerate(chunk):
            args = _find_call_args(ln, "snprintf")
            if args is None or len(args) < 2:
                continue
            dst = _rx(r"\s+").sub("", args[0])
            if dst not in arrays:
                continue
            n_arg = args[1].strip()
            if _sizeof_dst(n_arg, dst):
                continue
            n = _parse_int_literal(n_arg)
            m = arrays[dst]
            if n is None or n <= m:
                continue
            if i in seen:
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="STR-SNPRINTF",
                message=f"snprintf({dst}, {n}, …) exceeds local {dst}[{m}]",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_unlink(lines, rel, funcs, out) -> None:
    """unlink/remove return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _UNLINK_DISCARDED)):
            if not _UNLINK_DISCARDED.match(ln):
                continue
            fname = "unlink" if _find_call_args(ln, "unlink") else "remove"
            args = _find_call_args(ln, fname)
            if not args:
                continue
            # Distinct from API-IGNORED-ERROR (malloc/fopen/open). A
            # string-literal path is still this class: testdata is unlink("x").
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-UNLINK",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _api_mkfifo(lines, rel, funcs, out) -> None:
    """mkfifo/mknod return discarded (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _MKFIFO_DISCARDED)):
            if not _MKFIFO_DISCARDED.match(ln):
                continue
            fname = "mkfifo" if _find_call_args(ln, "mkfifo") else "mknod"
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-MKFIFO",
                message=f"{fname}() return is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _mem_bcopy(lines, rel, funcs, out) -> None:
    """bcopy with the same identifier as both source and destination."""
    for fn in funcs:
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[int] = set()
        for i, ln in enumerate(chunk):
            args = _find_call_args(ln, "bcopy")
            if args is None or len(args) < 2:
                continue
            src = _rx(r"\s+").sub("", args[0])
            dst = _rx(r"\s+").sub("", args[1])
            if src != dst or not _rx(r"^[A-Za-z_]\w*$").match(src):
                continue
            if i in seen:
                continue
            seen.add(i)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="MEM-BCOPY",
                message=f"bcopy({src}, {src}, …) has overlapping source and destination",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))


def _chroot_followed_by_chdir(body_lines: list[str], chroot_idx: int) -> bool:
    """True when chdir(…) appears after chroot(…) in the same function."""
    for ln in body_lines[chroot_idx + 1:]:
        if _find_call_args(ln, "chdir"):
            return True
    return False


def _api_chroot(lines, rel, funcs, out) -> None:
    """chroot(path) without a later chdir(…) in the same function (CWE-243)."""
    for fn in funcs:
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            if not _CHROOT_CALL.search(ln):
                continue
            if _chroot_followed_by_chdir(body_lines, i):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="API-CHROOT",
                message="chroot() without chdir(/) into the new root",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


_ATOI_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?"
    r"(?:atoi|atol|atoll)\s*\("
)


def _atoi_vars(body: str) -> set[str]:
    return {m.group("var") for m in _lit_finditer(_ATOI_ASSIGN, body)}


def _has_bounds_guard(var: str, body: str) -> bool:
    """Lower and upper bound checks on a parsed integer (combined or separate)."""
    v = re.escape(var)
    if re.search(
        rf"\bif\s*\([^)]*{v}\s*<\s*0[^)]*\|\|[^)]*{v}\s*>=",
        body,
    ):
        return True
    if re.search(
        rf"\bif\s*\([^)]*{v}\s*>=\s*\d+[^)]*\|\|[^)]*{v}\s*<",
        body,
    ):
        return True
    has_lower = bool(
        re.search(rf"\bif\s*\(\s*0\s*>\s*{v}\b", body)
        or re.search(rf"\bif\s*\(\s*{v}\s*<\s*0\b", body)
        or re.search(rf"\bif\s*\(\s*{v}\s*<=\s*-1\b", body)
    )
    has_upper = bool(
        re.search(rf"\bif\s*\(\s*{v}\s*>=\s*\d+\b", body)
        or re.search(rf"\bif\s*\(\s*{v}\s*>\s*\d+\b", body)
        or re.search(rf"\bif\s*\(\s*\d+\s*<=\s*{v}\b", body)
        or re.search(rf"\bif\s*\(\s*\d+\s*>\s*{v}\b", body)
    )
    return has_lower and has_upper


def _atoi_index_or_size_use(var: str, text: str) -> bool:
    v = re.escape(var)
    if re.search(rf"\[\s*{v}\s*\]", text):
        return True
    if re.search(rf"\b(?:malloc|alloca)\s*\(\s*{v}\b", text):
        return True
    if re.search(rf"\bcalloc\s*\(\s*{v}\b", text):
        return True
    return bool(re.search(rf"\brealloc\s*\([^,]+,\s*{v}\b", text))


def _int_atoi(lines, rel, funcs, out) -> None:
    """atoi/atol/atoll result used as index or allocation size without bounds."""
    for fn in funcs:
        body = fn.body
        parsed = _atoi_vars(body)
        if not parsed:
            continue
        body_lines = body.splitlines()
        start = fn.span[0]
        for var in sorted(parsed):
            if _has_bounds_guard(var, body):
                continue
            for i, ln in enumerate(body_lines):
                if not _atoi_index_or_size_use(var, ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="INT-ATOI",
                    message=f"{var} from atoi/atol/atoll used as index or "
                    f"allocation size without a range check",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))
                break


_NAMED_ENUM = re.compile(
    r"\benum\s+(?P<tag>[A-Za-z_]\w*)\s*\{(?P<body>[^{}]*)\}"
)
_ENUM_PARAM = re.compile(r"\benum\s+([A-Za-z_]\w*)\b")
_ENUM_LOCAL = re.compile(
    r"\benum\s+(?P<tag>[A-Za-z_]\w*)\s+(?P<name>[A-Za-z_]\w*)\b"
)
_SWITCH_IDENT = re.compile(r"\bswitch\s*\(\s*([A-Za-z_]\w*)\s*\)")
_CASE_ENUM_IDENT = re.compile(r"\bcase\s+([A-Za-z_]\w*)\s*:")
_DEFAULT_LABEL = re.compile(r"\bdefault\s*:")


def _named_enum_members(text: str) -> dict[str, list[str]]:
    """`enum Tag { A, B = 3, C }` → {Tag: [A, B, C]}. Anonymous enums omitted."""
    out: dict[str, list[str]] = {}
    for m in _lit_finditer(_NAMED_ENUM, text):
        members: list[str] = []
        for part in m.group("body").split(","):
            part = " ".join(part.split())
            if not part:
                continue
            name = part.split("=", 1)[0].strip()
            if _rx(r"[A-Za-z_]\w*").fullmatch(name):
                members.append(name)
        if members:
            out[m.group("tag")] = members
    return out


def _enum_vars_in_fn(fn, tags: dict[str, list[str]]) -> dict[str, str]:
    """Identifier → enum tag for params and locals typed `enum Tag`."""
    vars_: dict[str, str] = {}
    for typ, name in fn.params:
        if not name:
            continue
        m = _ENUM_PARAM.search(typ)
        if m and m.group(1) in tags:
            vars_[name] = m.group(1)
    for m in _lit_finditer(_ENUM_LOCAL, fn.body):
        tag, name = m.group("tag"), m.group("name")
        if tag in tags and name not in _KW:
            vars_[name] = tag
    return vars_


def _ident_switch_bodies(text: str) -> list[tuple[str, str, int]]:
    """`switch (ident) { ... }` → (ident, body, 0-based brace line offset)."""
    out: list[tuple[str, str, int]] = []
    for m in _lit_finditer(_SWITCH_IDENT, text):
        var = m.group(1)
        rest = text[m.end():]
        brace = rest.find("{")
        if brace < 0 or ";" in rest[:brace]:
            continue
        depth = 0
        end = None
        for k, ch in enumerate(rest[brace:]):
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    end = brace + k
                    break
        if end is None:
            continue
        body = rest[brace + 1: end]
        line_off = text[: m.end() + brace].count("\n")
        out.append((var, body, line_off))
    return out


def _enum_hole(lines, rel, funcs, out) -> None:
    """Named-enum switch missing an enumerator and a default.

    `default:` is treated as covering the hole. `switch (flags & MASK)` is
    UNINIT-SWITCH, not this. Integer `case 0:` does not count as coverage.
    """
    text = "\n".join(lines)
    tags = _named_enum_members(text)
    if not tags:
        return
    for fn in funcs:
        vars_ = _enum_vars_in_fn(fn, tags)
        if not vars_:
            continue
        start = fn.span[0]
        seen: set[tuple[str, int]] = set()
        for var, body, line_off in _ident_switch_bodies(fn.body):
            tag = vars_.get(var)
            if not tag:
                continue
            if _lit_search(_DEFAULT_LABEL, body):
                continue
            covered = set(_CASE_ENUM_IDENT.findall(body))
            missing = [n for n in tags[tag] if n not in covered]
            if not missing:
                continue
            key = (var, line_off)
            if key in seen:
                continue
            seen.add(key)
            line = start + line_off
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="INT-ENUM-HOLE",
                message=f"switch ({var}) on enum {tag} misses {', '.join(missing)} "
                "and has no default",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))


_STR_NULL_FN: dict[str, list[int]] = {
    "strlen": [0],
    "strcpy": [1],
    "strcmp": [0, 1],
}
_STD_MOVE = re.compile(
    r"std\s*::\s*move\s*\(\s*(?P<name>[A-Za-z_]\w*)\s*\)"
)
_CXX_VIEW_RET = re.compile(r"\b(?:string_view|span)\b")
_CXX_LOCAL_STRING = re.compile(
    r"^\s*(?P<static>static\s+)?(?:std::)?string\s+(?P<name>[A-Za-z_]\w*)\s*[;=]",
    re.M,
)
_CXX_RETURN_VIEW_OF = re.compile(
    r"\breturn\s+(?:std::)?(?:string_view|span)\s*\(\s*(?P<name>[A-Za-z_]\w*)\s*\)\s*;"
)
_CXX_BEGIN_CALL = re.compile(
    r"(?P<container>[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)*)\s*\.\s*begin\s*\("
)
_CXX_CONTAINER_MUTATE = re.compile(
    r"(?P<container>[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)*)\s*\."
    r"(?:push_back|insert|erase|clear)\s*\("
)
_CXX_LOCAL_LINE = re.compile(
    r"^\s*(?:(?:const|static|volatile)\s+)*"
    r"(?:unsigned\s+)?(?:char|short|int|long|float|double|bool|size_t|auto|"
    r"(?:std::)?(?:string|vector|map|set)\b|[A-Za-z_]\w*)\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*(?:=|;)",
)
_CXX_RETURN_LAMBDA = re.compile(r"\breturn\s*\[(?P<capture>[^\]]*)\]")
_CXX_AUTO_LAMBDA = re.compile(
    r"\bauto\s+(?P<name>[A-Za-z_]\w*)\s*=\s*\[(?P<capture>[^\]]*)\]"
)
_CXX_RETURN_NAME = re.compile(r"\breturn\s+(?P<name>[A-Za-z_]\w*)\s*;")
_UNIQUE_PTR_DECL = re.compile(
    r"\b(?:std::)?unique_ptr\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_UNIQUE_RAW_GET = re.compile(
    r"(?P<raw>[A-Za-z_]\w*)\s*=\s*(?P<up>[A-Za-z_]\w*)\s*\.\s*get\s*\(\s*\)"
)
_UNIQUE_RESET = re.compile(r"(?P<up>[A-Za-z_]\w*)\s*\.\s*reset\s*\(")
_UNIQUE_RELEASE = re.compile(r"(?P<up>[A-Za-z_]\w*)\s*\.\s*release\s*\(")
_UNIQUE_NULL = re.compile(
    r"(?P<up>[A-Za-z_]\w*)\s*=\s*(?:nullptr|NULL|0)\s*;"
)
_SHARED_PTR_DECL = re.compile(
    r"\b(?:std::)?shared_ptr\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_AUTO_PTR_USE = re.compile(r"\b(?:std::)?auto_ptr\s*<")
_STR_OWN_DECL = re.compile(
    r"\b(?:std::)?string\s+(?P<name>[A-Za-z_]\w*)\b"
)
_STR_CSTR_GET = re.compile(
    r"(?P<raw>[A-Za-z_]\w*)\s*=\s*(?P<s>[A-Za-z_]\w*)\s*\.\s*"
    r"(?:c_str|data)\s*\(\s*\)"
)
_STR_MUTATE = re.compile(
    r"(?P<s>[A-Za-z_]\w*)\s*(?:\+=|\.\s*(?:clear|append|assign)\s*\()"
)
_SHARED_FROM_THIS = re.compile(r"\bshared_from_this\s*\(")
_ENABLE_SHARED_FROM = re.compile(r"\benable_shared_from_this\b")
_FWD_PUSH_ARG = re.compile(
    r"\.\s*push_back\s*\(\s*(?P<arg>[A-Za-z_]\w*)\s*\)"
)
_OP_BOOL = re.compile(r"\b(?P<ex>explicit\s+)?operator\s+bool\s*\(")
_CONST_LOCAL = re.compile(
    r"^\s*const\s+(?:unsigned\s+)?(?:char|short|int|long|float|double|bool)\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*[=;]",
    re.M,
)
_CONST_CAST_WRITE = re.compile(
    r"\*\s*const_cast\s*<\s*(?P<target>[^>]+?)\s*>\s*\(\s*&\s*(?P<name>[A-Za-z_]\w*)\s*\)\s*="
)
_DYN_CAST_ASSIGN = re.compile(
    r"(?P<var>[A-Za-z_]\w*)\s*=\s*[^;]*\bdynamic_cast\s*<"
)
_REINTERPRET_CAST = re.compile(
    r"reinterpret_cast\s*<\s*(?P<target>[^>]+?)\s*>\s*\(\s*(?P<src>[^)]+?)\s*\)"
)
_BIT_CAST = re.compile(
    r"(?:std\s*::\s*)?bit_cast\s*<\s*(?P<tgt>[^>]+?)\s*>\s*\(\s*(?P<src>[^)]+?)\s*\)"
)
_PLACE_NEW = re.compile(r"\bnew\s*\(\s*(?P<ptr>[A-Za-z_]\w*)\s*\)")
_PLACE_BUF_DECL = re.compile(
    r"(?:unsigned\s+char|(?:std\s*::\s*)?byte)\s+(?P<name>[A-Za-z_]\w*)\s*\[",
)
_PLACE_PTR_DECL = re.compile(
    r"(?P<typ>unsigned\s+char|(?:std\s*::\s*)?byte|void|[A-Za-z_]\w*)\s*\*+\s*"
    r"(?P<name>[A-Za-z_]\w*)\b",
)
_PLACE_SKIP_PTR = {"nothrow"}
_CXX_THREAD_DECL = re.compile(
    r"(?:std\s*::\s*)?\bthread\s+(?P<name>[A-Za-z_]\w*)\s*\("
)
_CXX_OPTIONAL_DECL = re.compile(
    r"(?:std\s*::\s*)?optional\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_VARIANT_GET = re.compile(
    r"(?:std\s*::\s*)?\bget\s*<\s*(?P<ty>[^>]+)\s*>\s*\(\s*(?P<var>[A-Za-z_]\w*)\s*\)"
)
_CXX_RETURN_SPAN = re.compile(
    r"\breturn\s+(?:std\s*::\s*)?\bspan\s*<[^>]+>\s*\(\s*(?P<name>[A-Za-z_]\w*)"
)
_CXX_VEC_STR_DECL = re.compile(
    r"\b(?:std\s*::\s*)?(?:vector\s*<[^>;]+>|(?:basic_)?string)\s+"
    r"(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_SUBSCRIPT = re.compile(
    r"\b(?P<cont>[A-Za-z_]\w*)\s*\[\s*(?P<idx>[^\]]+)\s*\]"
)
_CATCH_ALL_BLOCK = re.compile(r"\bcatch\s*\(\s*\.\.\.\s*\)\s*\{")
_THROW_NEW = re.compile(r"\bthrow\s+new\b")
_CXX_SCALAR_MEMBER = re.compile(
    r"\b(?:int|short|long|char|bool|float|double)\s+(?P<name>[A-Za-z_]\w*)\s*;"
)
_SHALLOW_PTR_ASSIGN = re.compile(
    r"\b(?P<dst>[A-Za-z_]\w*)\s*(?:\.|->)\s*(?P<field>[A-Za-z_]\w*)"
    r"\s*=\s*"
    r"(?P<src>[A-Za-z_]\w*)\s*(?:\.|->)\s*(?P=field)\b"
)
_VOL_LOCAL = re.compile(
    r"(?:^|[;{(])\s*(?:(?:static|extern|auto|register|const)\s+)*"
    r"volatile\s+(?:(?:unsigned|signed)\s+)?"
    r"(?:int|short|long|char|bool|float|double)\s+(?P<name>[A-Za-z_]\w*)",
    re.M,
)
_VOL_CAST_WRITE = re.compile(
    r"\*\s*\(\s*(?:const\s+)?(?:(?:unsigned|signed)\s+)?"
    r"(?:int|short|long|char|bool|void|float|double)\s*\*\s*\)\s*"
    r"&\s*(?P<name>[A-Za-z_]\w*)\s*="
)
_CXX_PTR_DECL_ANY = re.compile(
    r"(?P<typ>(?:(?:unsigned|signed)\s+)?(?:char|short|int|long|float|double|void|"
    r"std::byte|[A-Za-z_]\w*)\s*(?:\s*const\s*)?\*+)\s*"
    r"(?P<name>[A-Za-z_]\w*)\b",
)
_CXX_PTR_DECL_LINE = re.compile(
    r"^\s*(?:(?:const|volatile)\s+)*"
    r"(?P<typ>(?:(?:unsigned|signed)\s+)?(?:char|short|int|long|float|double|void|"
    r"std::byte|[A-Za-z_]\w*)\s*(?:\s*const\s*)?\*+)\s*"
    r"(?P<name>[A-Za-z_]\w*)\s*=",
    re.M,
)
_CXX_SCALAR_LOCAL = re.compile(
    r"^\s*(?:(?:const|volatile)\s+)*"
    r"(?P<typ>(?:(?:unsigned|signed)\s+)?(?:char|short|int|long|float|double|bool|"
    r"[A-Za-z_]\w*))\s+(?P<name>[A-Za-z_]\w*)\s*[=;]",
    re.M,
)
_CATCH_BLOCK = re.compile(r"\bcatch\s*\(\s*(?P<clause>[^)]+)\)\s*\{")
_THROW_IDENT = re.compile(r"\bthrow\s+(?P<ident>[A-Za-z_]\w*)\s*;")
_CXX_EXPECTED_DECL = re.compile(
    r"(?:std\s*::\s*)?expected\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CONST_OBJ_DECL = re.compile(
    r"(?m)(?:^|[;{(])\s*(?:(?:static|extern|auto|register)\s+)*const\s+"
    r"(?:unsigned\s+|signed\s+)*(?:(?:std\s*::\s*)?[A-Za-z_]\w*"
    r"(?:\s*<[^>]*>)?)\s+(?P<name>[A-Za-z_]\w*)\s*[=;]"
)
_REF_RET_FN = re.compile(
    r"(?m)^[ \t]*(?:(?:static|inline|constexpr|extern|const)\s+)*"
    r"(?:const\s+)?(?:std\s*::\s*)?[A-Za-z_]\w*(?:\s*<[^>]*>)?\s*&(?!&)\s*"
    r"(?P<name>[A-Za-z_]\w*)\s*\((?P<params>[^;{}]*?)\)[^{]*\{"
)
_RETURN_CTOR = re.compile(
    r"\breturn\s+(?:std\s*::\s*)?[A-Za-z_]\w*(?:\s*<[^>]*>)?\s*[\({]"
)
_BIND_TMP_LOCAL = re.compile(
    r"const\s+(?:std\s*::\s*)?[A-Za-z_]\w*(?:\s*<[^>]*>)?\s*&"
    r"\s*(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?[A-Za-z_]\w*\s*[\({]"
)
_STD_FMT_CALL = re.compile(
    r"\bstd\s*::\s*(?P<fn>format|print|println)\s*\("
)
_SPACESHIP_DEFAULT = re.compile(
    r"operator\s*<=>\s*\([^)]*\)[^{;]*=\s*default"
)
_PTR_MEMBER = re.compile(
    r"(?:[A-Za-z_]\w*|char|int|void|short|long)\s*\*+\s*"
    r"(?P<name>[A-Za-z_]\w*)\s*;"
)
_LAMBDA_SKIP = _KW | {
    "this", "true", "false", "nullptr", "new", "delete", "sizeof",
    "return", "mutable", "const", "int", "void", "char", "auto",
    "bool", "unsigned", "long", "short", "float", "double",
}
_ASYNC_DISCARDED = re.compile(
    r"^\s*(?:std\s*::\s*)?async\s*\([^;]*\)\s*;\s*$"
)
_CXX_FUTURE_DECL = re.compile(
    r"(?:std\s*::\s*)?future\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_FUTURE_GET = re.compile(
    r"\b(?P<name>[A-Za-z_]\w*)\s*\.\s*get\s*\("
)
_CXX_FUNCTION_DECL = re.compile(
    r"(?:std\s*::\s*)?\bfunction\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_NODISCARD_ATTR = re.compile(r"\[\[\s*nodiscard\s*\]\]")
_NODISCARD_NAME = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
_NODISCARD_SKIP = _LAMBDA_SKIP | {
    "class", "struct", "enum", "union", "static", "inline",
    "constexpr", "extern", "volatile", "signed", "wchar_t",
    "size_t", "nodiscard",
}
_NODISCARD_DISCARDED = re.compile(
    r"^\s*(?:std\s*::\s*)?(?P<name>[A-Za-z_]\w*)\s*\([^;]*\)\s*;\s*$"
)
_CXX_JTHREAD_DECL = re.compile(
    r"(?:std\s*::\s*)?\bjthread\s+(?P<name>[A-Za-z_]\w*)\s*\("
)
_CXX_RETURN_MDSPAN = re.compile(
    r"\breturn\s+(?:std\s*::\s*)?mdspan\s*<[^>]+>\s*\(\s*(?P<name>[A-Za-z_]\w*)"
)
_CXX_RETURN_ATOMIC_REF = re.compile(
    r"\breturn\s+(?:std\s*::\s*)?atomic_ref\s*<[^>]+>\s*"
    r"\(\s*(?P<name>[A-Za-z_]\w*)"
)
_CXX_ATOMIC_REF_CTOR = re.compile(
    r"(?:std\s*::\s*)?atomic_ref\s*<[^>]+>\s+(?P<ref>[A-Za-z_]\w*)\s*"
    r"[\({]\s*(?P<name>[A-Za-z_]\w*)"
)
_CXX_CV_WAIT_BARE = re.compile(
    r"\.\s*wait\s*\(\s*[A-Za-z_]\w*\s*\)"
)
_STD_BIND_CALL = re.compile(r"\bstd::bind\s*\(")
_CXX_ASSUME_FALSE = re.compile(
    r"\[\[\s*assume\s*\(\s*(?:false|0)\s*\)\s*\]\]"
)
_CXX_SHARED_MUTEX_DECL = re.compile(
    r"(?:std\s*::\s*)?shared_mutex\s+(?P<name>[A-Za-z_]\w*)\b"
)
_ANY_CAST_CALL = re.compile(
    r"(?:std\s*::\s*)?any_cast\s*<[^>]+>\s*\(\s*(?P<arg>[^)]+)\s*\)"
)
_FS_REMOVE = re.compile(
    r"(?:std\s*::\s*)?filesystem\s*::\s*remove(?:_all)?\s*"
    r"\(\s*(?P<p>[A-Za-z_]\w*)\s*\)"
)
_FS_USE_AFTER = re.compile(
    r"(?:std\s*::\s*)?filesystem\s*::\s*"
    r"(?:exists|file_size|is_regular_file|is_directory|"
    r"last_write_time|status|canonical|equivalent)\s*"
    r"\(\s*(?P<p>[A-Za-z_]\w*)\s*\)"
)
_REGEX_CTOR = re.compile(
    r"(?:std\s*::\s*)?\bregex\s+(?P<name>[A-Za-z_]\w*)\s*"
    r"[\({]\s*(?P<pat>[^)}]+)"
)
_CXX_LATCH_DECL = re.compile(
    r"(?:std\s*::\s*)?\blatch\s+(?P<name>[A-Za-z_]\w*)\s*\("
)
_FROM_CHARS_OUT = re.compile(
    r"(?:std\s*::\s*)?from_chars\s*\(\s*[^,]+,\s*[^,]+,\s*"
    r"(?P<var>[A-Za-z_]\w*)"
)
_INIT_LIST_TMP_BEGIN = re.compile(
    r"(?:std\s*::\s*)?initializer_list\s*<[^>]+>\s*\{[^;]*\}\s*\.\s*begin\s*\("
)
_CXX_STOP_TOKEN = re.compile(r"(?:std\s*::\s*)?stop_token\b")
_CXX_INF_LOOP = re.compile(
    r"\bwhile\s*\(\s*true\s*\)|\bfor\s*\(\s*;\s*;\s*\)"
)
_CXX_FLAT_MAP_DECL = re.compile(
    r"(?:std\s*::\s*)?(?:flat_map|(?P<map>map))\s*<[^>]+>\s+"
    r"(?P<name>[A-Za-z_]\w*)"
)
_CXX_SEM_DECL = re.compile(
    r"(?:std\s*::\s*)?counting_semaphore(?:\s*<[^>]+>)?\s+"
    r"(?P<name>[A-Za-z_]\w*)"
)
_STACKTRACE_CURRENT = re.compile(
    r"(?:std\s*::\s*)?stacktrace\s*::\s*current\s*\("
)
_STACKTRACE_ASSIGN = re.compile(
    r"(?P<name>[A-Za-z_]\w*)\s*=\s*"
    r"(?:std\s*::\s*)?stacktrace\s*::\s*current\s*\("
)
_STACKTRACE_DIRECT_ZERO = re.compile(
    r"(?:std\s*::\s*)?stacktrace\s*::\s*current\s*\(\s*\)\s*"
    r"(?:\[\s*0\s*\]|\.\s*at\s*\(\s*0\s*\))"
)
_PACK_PRAGMA_1 = re.compile(
    r"#\s*pragma\s+pack\s*\(\s*(?:push\s*,\s*)?1\s*\)"
)
_PACK_INT_MEMBER_ADDR = re.compile(
    r"\b(?:unsigned\s+)?(?:int|short|long)\s*\*\s*"
    r"(?P<ptr>[A-Za-z_]\w*)\s*=\s*"
    r"&\s*(?P<obj>[A-Za-z_]\w*)\s*\.\s*(?P<mem>[A-Za-z_]\w*)"
)
_CXX_FN_REF_TMP = re.compile(
    r"(?:std\s*::\s*)?function_ref\s*<[^>]+>\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*=\s*(?:\[|.+\()"
)
_CXX_FN_REF_RETURN = re.compile(
    r"\breturn\s+(?:std\s*::\s*)?function_ref\s*<"
)
_CXX_MOF_DECL = re.compile(
    r"(?:std\s*::\s*)?move_only_function\s*<[^>]+>\s+"
    r"(?P<name>[A-Za-z_]\w*)\b"
)
_RANGES_TMP_PIPE = re.compile(
    r"(?:std\s*::\s*)?(?:string|vector\s*<[^>]+>)\s*[\({][^;]*"
    r"\|\s*(?:std\s*::\s*)?(?:ranges\s*::\s*)?views\s*::",
)
_RANGES_MATERIALIZE = re.compile(
    r"(?:std\s*::\s*)?(?:vector|string)\s*(?:<[^>]+>)?\s+"
    r"[A-Za-z_]\w*\s*\([^;]*\.begin\s*\("
)
_CHRONO_NOW = re.compile(
    r"(?:std\s*::\s*)?(?:chrono\s*::\s*)?"
    r"(?:system_clock|steady_clock|high_resolution_clock)"
    r"\s*::\s*now\s*\("
)
_CHRONO_PRNG = re.compile(r"\b(?:srand|srandom|mt19937|mt19937_64)\b")
_CXX_INPLACE_DECL = re.compile(
    r"(?:std\s*::\s*)?inplace_vector\s*<[^>]+>\s+"
    r"(?P<name>[A-Za-z_]\w*)"
)
_CXX_FLAT_SET_DECL = re.compile(
    r"(?:std\s*::\s*)?flat_set\s*<[^>]+>\s+"
    r"(?P<name>[A-Za-z_]\w*)"
)
_CXX_COPYABLE_FN_DECL = re.compile(
    r"(?:std\s*::\s*)?copyable_function\s*<[^>]+>\s+"
    r"(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_HIVE_DECL = re.compile(
    r"(?:std\s*::\s*)?hive\s*<[^>]+>\s+"
    r"(?P<name>[A-Za-z_]\w*)"
)
_CXX_HIVE_MUTATE = re.compile(
    r"(?P<container>[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)*)\s*\."
    r"(?:insert|emplace|erase)\s*\("
)
_CXX_BITSET_DECL = re.compile(
    r"(?:std\s*::\s*)?bitset\s*<\s*(?P<n>[^>]+)\s*>\s+"
    r"(?P<name>[A-Za-z_]\w*)"
)
_CXX_BITSET_TEST = re.compile(
    r"\b(?P<name>[A-Za-z_]\w*)\s*\.\s*test\s*\(\s*(?P<idx>[A-Za-z_]\w*)\s*\)"
)
_CXX_SSTREAM_DECL = re.compile(
    r"(?:std\s*::\s*)?(?:basic_)?(?:string|ostring|istring)stream\s+"
    r"(?P<name>[A-Za-z_]\w*)"
)
_CXX_SSTREAM_VIEW = re.compile(
    r"(?:std\s*::\s*)?string_view\s+(?P<v>[A-Za-z_]\w*)\s*=\s*"
    r"(?P<ss>[A-Za-z_]\w*)\s*\.\s*str\s*\("
)
_CXX_SSTREAM_CSTR_TMP = re.compile(
    r"(?P<ss>[A-Za-z_]\w*)\s*\.\s*str\s*\(\s*\)\s*\.\s*c_str\s*\("
)
_CXX_INDIRECT_DECL = re.compile(
    r"(?:std\s*::\s*)?indirect\s*<[^>]+>\s+"
    r"(?P<name>[A-Za-z_]\w*)"
)
_TO_CHARS_BUF = re.compile(
    r"(?:std\s*::\s*)?\bto_chars\s*\(\s*(?P<buf>[A-Za-z_]\w*)"
)
_HAZARD_DECL = re.compile(r"(?:std\s*::\s*)?\bhazard_pointer\b")
_TEXT_ENC_CTOR = re.compile(
    r"(?:std\s*::\s*)?\btext_encoding\s+(?P<name>[A-Za-z_]\w*)\s*"
    r"[\({]\s*(?P<arg>[^)}]+)"
)
_EXPECTED_ERROR_CALL = re.compile(
    r"\b(?P<name>[A-Za-z_]\w*)\s*\.\s*error\s*\("
)
_CXX_SIMD_DECL = re.compile(
    r"(?:(?:std\s*::\s*)?(?:experimental\s*::\s*)?)?simd\s*<[^>]+>\s+"
    r"(?P<name>[A-Za-z_]\w*)"
)
_RCU_OBJ_DECL = re.compile(
    r"(?:std\s*::\s*)?rcu_obj\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)"
)
_LINALG_MATRIX_DECL = re.compile(
    r"(?:std\s*::\s*)?linalg\s*::\s*matrix\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)"
)
_LINALG_SCALED = re.compile(
    r"(?:std\s*::\s*)?linalg\s*::\s*(?:scaled|copied)\s*\("
)
_SYNC_WAIT_DISCARDED = re.compile(
    r"^\s*(?:std\s*::\s*(?:execution\s*::\s*)?)?sync_wait\s*\([^;]*\)\s*;\s*$"
)
_EMBED_DIR = re.compile(r"#\s*embed\b\s*(?P<arg>.*)$")
_CXX_CONTRACT_FALSE = re.compile(
    r"\bcontract_assert\s*\(\s*(?:false|0)\s*\)"
    r"|(?:^|[;{])\s*(?:pre|post)\s*\(\s*(?:false|0)\s*\)"
    r"|\[\[\s*(?:pre|post)\s*:\s*(?:false|0)"
)
_CXX_REFLECT_META = re.compile(r"\^\^|\bstd\s*::\s*meta\s*::")
_CXX_REFLECT_DEFINE = re.compile(r"\bdefine_(?:aggregate|class)\s*\(")
_OUT_PTR_CALL = re.compile(
    r"(?:std\s*::\s*)?(?:inout_ptr|out_ptr)\s*(?:<[^>]+>)?\s*"
    r"\(\s*(?P<ptr>[A-Za-z_]\w*)"
)
_CXX_FLAT_MULTIMAP_DECL = re.compile(
    r"(?:std\s*::\s*)?flat_multimap\s*<[^>]+>\s+"
    r"(?P<name>[A-Za-z_]\w*)"
)
_CXX_SPANSTREAM_DECL = re.compile(
    r"(?:std\s*::\s*)?(?:basic_)?(?:i|o)?spanstream\s+"
    r"(?P<name>[A-Za-z_]\w*)"
)
_CXX_BARRIER_DECL = re.compile(
    r"(?:std\s*::\s*)?\bbarrier(?:\s*<[^>]*>)?\s+"
    r"(?P<name>[A-Za-z_]\w*)"
)
_CXX_TASK_STARTED = re.compile(
    r"(?:std\s*::\s*(?:execution\s*::\s*)?)?task\s*<[^>]+>\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*=(?!=)"
)
_CXX_GENERATOR_ASSIGN = re.compile(
    r"(?:std\s*::\s*)?generator\s*<[^>]+>\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*=(?!=)"
)
_CXX_OSYNC_DECL = re.compile(
    r"(?:std\s*::\s*)?(?:basic_)?osyncstream(?:\s*<[^>]+>)?\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*[\({]"
)
_CXX_PACKAGED_DECL = re.compile(
    r"(?:std\s*::\s*)?packaged_task\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_FLAT_MULTISET_DECL = re.compile(
    r"(?:std\s*::\s*)?flat_multiset\s*<[^>]+>\s+"
    r"(?P<name>[A-Za-z_]\w*)"
)
_CXX_SYNCBUF_DECL = re.compile(
    r"(?:std\s*::\s*)?(?:basic_)?syncbuf(?:\s*<[^>]+>)?\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*[\({]"
)
_CXX_COUNTED_ITER_CTOR = re.compile(
    r"(?:std\s*::\s*)?counted_iterator(?:\s*<[^>]+>)?\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*\(\s*(?P<src>[^,]+),\s*(?P<n>[A-Za-z_]\w*)"
)
_CXX_PROMISE_DECL = re.compile(
    r"(?:std\s*::\s*)?promise\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_WEAK_PTR_DECL = re.compile(
    r"(?:std\s*::\s*)?weak_ptr\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_WEAK_LOCK = re.compile(
    r"(?P<locked>[A-Za-z_]\w*)\s*=\s*"
    r"(?P<weak>[A-Za-z_]\w*)\s*\.\s*lock\s*\("
)
_CXX_EXCEPTION_PTR_DECL = re.compile(
    r"(?:std\s*::\s*)?exception_ptr\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_CORO_HANDLE_DECL = re.compile(
    r"(?:std\s*::\s*)?coroutine_handle(?:\s*<[^>]*>)?\s+"
    r"(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_VALARRAY_DECL = re.compile(
    r"(?:std\s*::\s*)?valarray\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_TO_UNDERLYING = re.compile(
    r"(?:std\s*::\s*)?to_underlying\s*\(\s*(?P<arg>[A-Za-z_]\w*)\s*\)"
)
_CXX_UNEXPECTED_DECL = re.compile(
    r"(?:std\s*::\s*)?unexpected\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_TUPLE_DECL = re.compile(
    r"(?:std\s*::\s*)?tuple\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_TUPLE_GET = re.compile(
    r"(?:std\s*::\s*)?\bget\s*<"
)
_CXX_DEQUE_DECL = re.compile(
    r"(?:std\s*::\s*)?deque\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_FWD_LIST_DECL = re.compile(
    r"(?:std\s*::\s*)?forward_list\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_LIST_DECL = re.compile(
    r"(?:std\s*::\s*)?\blist\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_MAP_DECL = re.compile(
    r"(?:std\s*::\s*)?\bmap\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_UMAP_DECL = re.compile(
    r"(?:std\s*::\s*)?unordered_map\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_SET_DECL = re.compile(
    r"(?:std\s*::\s*)?\bset\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_QUEUE_DECL = re.compile(
    r"(?:std\s*::\s*)?\bqueue\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_STACK_DECL = re.compile(
    r"(?:std\s*::\s*)?\bstack\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_PQUEUE_DECL = re.compile(
    r"(?:std\s*::\s*)?priority_queue\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_ARRAY_DECL = re.compile(
    r"(?:std\s*::\s*)?\barray\s*<\s*[^,>]+,\s*(?P<n>[^>]+)\s*>\s+"
    r"(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_USET_DECL = re.compile(
    r"(?:std\s*::\s*)?unordered_set\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_WSTRING_DECL = re.compile(
    r"^\s*(?P<static>static\s+)?(?:std\s*::\s*)?wstring\s+"
    r"(?P<name>[A-Za-z_]\w*)",
    re.M,
)
_CXX_WSTRING_VIEW_BIND = re.compile(
    r"(?:std\s*::\s*)?wstring_view\s+(?P<name>[A-Za-z_]\w*)\s*="
    r"\s*(?P<src>[A-Za-z_]\w*)"
)
_CXX_RETURN_WVIEW = re.compile(
    r"\breturn\s+(?:std\s*::\s*)?wstring_view\s*\(\s*(?P<name>[A-Za-z_]\w*)\s*\)"
)
_CXX_MULTIMAP_DECL = re.compile(
    r"(?:std\s*::\s*)?\bmultimap\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_MULTISET_DECL = re.compile(
    r"(?:std\s*::\s*)?\bmultiset\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_BINSEM_ZERO = re.compile(
    r"(?:std\s*::\s*)?binary_semaphore\s+(?P<name>[A-Za-z_]\w*)\s*"
    r"\(\s*0\s*\)"
)
_CXX_ERROR_CODE_DECL = re.compile(
    r"(?:std\s*::\s*)?error_code\s+(?P<name>[A-Za-z_]\w*)\b"
)
_BYTESWAP_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?byteswap\s*\("
)
_BYTESWAP_IN_INDEX = re.compile(
    r"\[\s*(?:std\s*::\s*)?byteswap\s*\("
)
_CXX_UMMAP_DECL = re.compile(
    r"(?:std\s*::\s*)?unordered_multimap\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_UMSET_DECL = re.compile(
    r"(?:std\s*::\s*)?unordered_multiset\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_U8STRING_DECL = re.compile(
    r"^\s*(?P<static>static\s+)?(?:std\s*::\s*)?u8string\s+"
    r"(?P<name>[A-Za-z_]\w*)",
    re.M,
)
_CXX_U8STRING_VIEW_BIND = re.compile(
    r"(?:std\s*::\s*)?u8string_view\s+(?P<name>[A-Za-z_]\w*)\s*="
    r"\s*(?P<src>[A-Za-z_]\w*)"
)
_CXX_RETURN_U8VIEW = re.compile(
    r"\breturn\s+(?:std\s*::\s*)?u8string_view\s*\(\s*(?P<name>[A-Za-z_]\w*)\s*\)"
)
_CXX_SHARED_LOCK_DECL = re.compile(
    r"(?:std\s*::\s*)?shared_lock\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_ATOMIC_FLAG_DECL = re.compile(
    r"(?:std\s*::\s*)?atomic_flag\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_CVANY_DECL = re.compile(
    r"(?:std\s*::\s*)?condition_variable_any\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_RECURSIVE_MUTEX_DECL = re.compile(
    r"(?:std\s*::\s*)?recursive_mutex\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_TIMED_MUTEX_DECL = re.compile(
    r"(?:std\s*::\s*)?\btimed_mutex\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_FSTREAM_DECL = re.compile(
    r"(?:std\s*::\s*)?(?:ifstream|ofstream|fstream)\s+"
    r"(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_LOCK_RAII = re.compile(
    r"\b(?:lock_guard|unique_lock|scoped_lock|shared_lock)\b"
)
_CXX_SHARED_TIMED_MUTEX_DECL = re.compile(
    r"(?:std\s*::\s*)?shared_timed_mutex\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_RECURSIVE_TIMED_MUTEX_DECL = re.compile(
    r"(?:std\s*::\s*)?recursive_timed_mutex\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_SYSTEM_ERROR_DECL = re.compile(
    r"(?:std\s*::\s*)?system_error\s+(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_SYSTEM_ERROR_CATCH = re.compile(
    r"\bcatch\s*\(\s*(?:const\s+)?(?:std\s*::\s*)?system_error"
    r"\s*[&*]?\s*(?P<name>[A-Za-z_]\w*)"
)
_CXX_TZDB_CALL = re.compile(
    r"\b(?:current_zone|locate_zone|get_tzdb)\s*\("
)
_CXX_TZDB_BIND = re.compile(
    r"\b(?:auto(?:\s*[&*])?|(?:const\s+)?(?:std\s*::\s*chrono\s*::\s*)?"
    r"time_zone\s*\*|tzdb\s*&)\s+(?P<name>[A-Za-z_]\w*)\s*="
    r"[^;]*(?:current_zone|locate_zone|get_tzdb)\s*\("
)
_CXX_VIEWS_ZIP = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*zip\b|\bzip_view\b)"
)
_CXX_ZIP_AUTO = re.compile(
    r"\bauto\s+(?P<name>[A-Za-z_]\w*)\s*=\s*"
    r"(?:std\s*::\s*)?views\s*::\s*zip\s*\("
)
_CXX_ZIP_VIEW_DECL = re.compile(
    r"(?:std\s*::\s*(?:ranges\s*::\s*)?)?zip_view(?:\s*<[^>]+>)?\s+"
    r"(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_FORMAT_TO = re.compile(
    r"\bformat_to\s*\(\s*(?P<dst>[A-Za-z_]\w*)"
)
_CXX_CHAR_BUF = re.compile(
    r"\bchar\s+(?P<name>[A-Za-z_]\w*)\s*\["
)
_CXX_ERROR_CATEGORY = re.compile(r"\berror_category\b")
_CXX_NESTED_EXC = re.compile(
    r"\b(?:nested_exception|throw_with_nested|rethrow_if_nested)\b"
)
_CXX_ATOMIC_FENCE = re.compile(
    r"\b(?:atomic_thread_fence|atomic_signal_fence)\s*\("
)
_CXX_FENCE_ORDER = re.compile(
    r"\b(?:atomic_thread_fence|atomic_signal_fence)\s*\([^)]*memory_order"
)
_CXX_NOTIFY_EXIT = re.compile(r"\bnotify_all_at_thread_exit\b")
_CXX_CV_WAITER = re.compile(r"\bwait(?:_for|_until)?\s*\(")
_CXX_WSTRING_CONVERT = re.compile(r"\bwstring_convert\b")
_CXX_WCONVERT_XFORM = re.compile(r"\.\s*(?:to_bytes|from_bytes)\s*\(")
_CXX_INVOKE = re.compile(
    r"\bstd\s*::\s*invoke\s*\(\s*(?P<arg>[A-Za-z_]\w*)"
)
_CXX_APPLY = re.compile(
    r"\bstd\s*::\s*apply\s*\(\s*(?P<arg>[A-Za-z_]\w*)"
)
_CXX_REFWRAP_TOKEN = re.compile(
    r"(?:\breference_wrapper\b|\bstd\s*::\s*(?:cref|ref)\s*\()"
)
_CXX_REFWRAP_RETURN = re.compile(
    r"\breturn\s+(?:std\s*::\s*)?(?:cref|ref)\s*\(\s*(?P<arg>[A-Za-z_]\w*)"
)
_CXX_REFWRAP_BIND = re.compile(
    r"\b(?P<w>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?(?:cref|ref)\s*\(\s*(?P<arg>[A-Za-z_]\w*)"
)
_CXX_ENDIAN = re.compile(r"\bstd\s*::\s*endian\b")
_ENDIAN_IN_INDEX = re.compile(r"\[\s*[^\]]*\bstd\s*::\s*endian\b")
_ENDIAN_ASSIGN = re.compile(
    r"\b(?:auto|int|unsigned|(?:std\s*::\s*)?endian)\s+(?P<var>[A-Za-z_]\w*)\s*="
    r"[^;]*\bstd\s*::\s*endian\b"
)
_CXX_BIT_FN = (
    r"(?:std\s*::\s*)?(?:bit_ceil|bit_floor|has_single_bit|popcount)"
)
_CXX_BITCEIL_TOKEN = re.compile(rf"\b{_CXX_BIT_FN}\s*\(")
_BITCEIL_IN_INDEX = re.compile(rf"\[\s*(?:\(int\))?\s*{_CXX_BIT_FN}\s*\(")
_BITCEIL_ASSIGN = re.compile(
    rf"\b(?P<var>[A-Za-z_]\w*)\s*=\s*{_CXX_BIT_FN}\s*\("
)
_BITCEIL_ZERO = re.compile(r"\b(?:std\s*::\s*)?bit_ceil\s*\(\s*0\s*\)")
_CXX_UNCAUGHT = re.compile(r"\buncaught_exceptions\b")
_UNCAUGHT_BOOL_IF = re.compile(
    r"\bif\s*\(\s*(?:std\s*::\s*)?uncaught_exceptions\s*\(\s*\)\s*\)"
)
_UNCAUGHT_SAVED = re.compile(
    r"\b(?:std\s*::\s*)?uncaught_exceptions\s*\(\s*\)\s*(?:==|!=|<|>|<=|>=)"
    r"|"
    r"(?:==|!=|<|>|<=|>=)\s*(?:std\s*::\s*)?uncaught_exceptions\s*\(\s*\)"
)
_CXX_VIEWS_JOIN = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*join\b|\bjoin_view\b)"
)
_CXX_JOIN_AUTO = re.compile(
    r"\bauto\s+(?P<name>[A-Za-z_]\w*)\s*=\s*"
    r"(?:std\s*::\s*)?views\s*::\s*join\s*\("
)
_CXX_JOIN_VIEW_DECL = re.compile(
    r"(?:std\s*::\s*(?:ranges\s*::\s*)?)?join_view(?:\s*<[^>]+>)?\s+"
    r"(?P<name>[A-Za-z_]\w*)\b"
)
_CXX_QUICK_EXIT = re.compile(r"\bquick_exit\s*\(")
_CXX_AT_QUICK_EXIT = re.compile(r"\bat_quick_exit\s*\(")
_CXX_TO_ARRAY = re.compile(r"\bto_array\s*\(")
_TO_ARRAY_BIND = re.compile(
    r"\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?to_array\s*\("
    r"\s*(?P<src>[A-Za-z_]\w*)"
)
_TO_ARRAY_INLINE_IDX = re.compile(
    r"\b(?:std\s*::\s*)?to_array\s*\([^)]*\)\s*\[\s*(?P<idx>[^\]]+)\s*\]"
)
_C_ARRAY_SIZE = re.compile(
    r"\b(?:(?:unsigned|signed|const|volatile)\s+)*"
    r"(?:int|char|short|long|float|double)\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*\[\s*(?P<n>\d+)\s*\]"
)
_CXX_ZONED_TIME = re.compile(r"\bzoned_time\b")
_CXX_ZONE_LOOKUP = re.compile(r"\b(?:locate_zone|current_zone)\b")
_CXX_KILLDEP = re.compile(r"\bkill_dependency\s*\(")
_KILLDEP_IN_INDEX = re.compile(
    r"\[\s*(?:std\s*::\s*)?kill_dependency\s*\("
)
_KILLDEP_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?kill_dependency\s*\("
)
_CXX_ROTL_FN = r"(?:std\s*::\s*)?rot[lr]"
_CXX_ROTL_TOKEN = re.compile(rf"\b{_CXX_ROTL_FN}\s*\(")
_ROTL_IN_INDEX = re.compile(rf"\[\s*{_CXX_ROTL_FN}\s*\(")
_ROTL_ASSIGN = re.compile(
    rf"\b(?P<var>[A-Za-z_]\w*)\s*=\s*{_CXX_ROTL_FN}\s*\("
)
_CXX_CURRENT_EXC = re.compile(r"\bcurrent_exception\s*\(")
_CXX_RETHROW_EXC = re.compile(r"\brethrow_exception\s*\(")
_CUR_EXC_INLINE = re.compile(
    r"\brethrow_exception\s*\(\s*(?:std\s*::\s*)?current_exception\s*\("
)
_CUR_EXC_BIND = re.compile(
    r"\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?current_exception\s*\("
)
_CXX_INT_CAST = r"(?:\(\s*int\s*\))?"
_CXX_BITWIDTH_FN = r"(?:std\s*::\s*)?bit_width"
_CXX_BITWIDTH_TOKEN = re.compile(rf"\b{_CXX_BITWIDTH_FN}\s*\(")
_BITWIDTH_IN_INDEX = re.compile(
    rf"\[\s*{_CXX_INT_CAST}\s*{_CXX_BITWIDTH_FN}\s*\("
)
_BITWIDTH_ASSIGN = re.compile(
    rf"\b(?P<var>[A-Za-z_]\w*)\s*=\s*{_CXX_INT_CAST}\s*{_CXX_BITWIDTH_FN}\s*\("
)
_CXX_LERP_FN = r"(?:std\s*::\s*)?lerp"
_CXX_LERP_TOKEN = re.compile(rf"\b{_CXX_LERP_FN}\s*\(")
_LERP_IN_INDEX = re.compile(rf"\[\s*{_CXX_INT_CAST}\s*{_CXX_LERP_FN}\s*\(")
_LERP_ASSIGN = re.compile(
    rf"\b(?P<var>[A-Za-z_]\w*)\s*=\s*{_CXX_INT_CAST}\s*{_CXX_LERP_FN}\s*\("
)
_CXX_LERP_FINITE = re.compile(r"\b(?:isfinite|isnan|isinf)\s*\(")
_CXX_MIDPOINT_FN = r"(?:std\s*::\s*)?midpoint"
_CXX_MIDPOINT_TOKEN = re.compile(rf"\b{_CXX_MIDPOINT_FN}\s*\(")
_MIDPOINT_IN_INDEX = re.compile(
    rf"\[\s*{_CXX_INT_CAST}\s*{_CXX_MIDPOINT_FN}\s*\("
)
_MIDPOINT_ASSIGN = re.compile(
    rf"\b(?P<var>[A-Za-z_]\w*)\s*=\s*{_CXX_INT_CAST}\s*{_CXX_MIDPOINT_FN}\s*\("
)
_CXX_CMPLESS_FN = (
    r"(?:std\s*::\s*)?(?:cmp_less|in_range)\s*(?:<[^>;]*>)?"
)
_CXX_CMPLESS_TOKEN = re.compile(rf"\b{_CXX_CMPLESS_FN}\s*\(")
_CMPLESS_IN_INDEX = re.compile(rf"\[\s*{_CXX_CMPLESS_FN}\s*\(")
_CMPLESS_ASSIGN = re.compile(
    rf"\b(?P<var>[A-Za-z_]\w*)\s*=\s*{_CXX_CMPLESS_FN}\s*\("
)
_CXX_IN_RANGE_IF = re.compile(
    r"\bif\s*\(\s*!?\s*(?:std\s*::\s*)?in_range\s*(?:<[^>;]*>)?\s*\("
)
_CXX_COUNTL_FN = r"(?:std\s*::\s*)?count[lr]_(?:zero|one)"
_CXX_COUNTL_TOKEN = re.compile(rf"\b{_CXX_COUNTL_FN}\s*\(")
_COUNTL_IN_INDEX = re.compile(rf"\[\s*{_CXX_INT_CAST}\s*{_CXX_COUNTL_FN}\s*\(")
_COUNTL_ASSIGN = re.compile(
    rf"\b(?P<var>[A-Za-z_]\w*)\s*=\s*{_CXX_INT_CAST}\s*{_CXX_COUNTL_FN}\s*\("
)
_CXX_STD_UNREACHABLE = re.compile(r"\bstd\s*::\s*unreachable\s*\(")
_CXX_GCD_FN = r"std\s*::\s*gcd"
_CXX_GCD_TOKEN = re.compile(rf"\b{_CXX_GCD_FN}\s*\(")
_GCD_IN_INDEX = re.compile(rf"\[\s*{_CXX_INT_CAST}\s*{_CXX_GCD_FN}\s*\(")
_GCD_ASSIGN = re.compile(
    rf"\b(?P<var>[A-Za-z_]\w*)\s*=\s*{_CXX_INT_CAST}\s*{_CXX_GCD_FN}\s*\("
)
_CXX_LCM_FN = r"std\s*::\s*lcm"
_CXX_LCM_TOKEN = re.compile(rf"\b{_CXX_LCM_FN}\s*\(")
_LCM_IN_INDEX = re.compile(rf"\[\s*{_CXX_INT_CAST}\s*{_CXX_LCM_FN}\s*\(")
_LCM_ASSIGN = re.compile(
    rf"\b(?P<var>[A-Za-z_]\w*)\s*=\s*{_CXX_INT_CAST}\s*{_CXX_LCM_FN}\s*\("
)
_CXX_CLAMP_FN = r"std\s*::\s*clamp"
_CXX_CLAMP_TOKEN = re.compile(rf"\b{_CXX_CLAMP_FN}\s*\(")
_CLAMP_IN_INDEX = re.compile(rf"\[\s*{_CXX_INT_CAST}\s*{_CXX_CLAMP_FN}\s*\(")
_CLAMP_ASSIGN = re.compile(
    rf"\b(?P<var>[A-Za-z_]\w*)\s*=\s*{_CXX_INT_CAST}\s*{_CXX_CLAMP_FN}\s*\("
)
_CXX_EXCHANGE_TOKEN = re.compile(r"\bstd\s*::\s*exchange\s*\(")
_EXCHANGE_IN_INDEX = re.compile(r"\[\s*std\s*::\s*exchange\s*\(")
_EXCHANGE_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*std\s*::\s*exchange\s*\("
    r"\s*(?P<obj>[A-Za-z_]\w*)"
)
_EXCHANGE_CALL = re.compile(
    r"\bstd\s*::\s*exchange\s*\(\s*(?P<obj>[A-Za-z_]\w*)"
)
_CXX_TO_ADDRESS = re.compile(r"\bto_address\s*\(")
_TO_ADDR_BIND = re.compile(
    r"\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?to_address\s*\("
    r"\s*(?P<src>[A-Za-z_]\w*)"
)
_TO_ADDR_INLINE_IDX = re.compile(
    r"\b(?:std\s*::\s*)?to_address\s*\([^)]*\)\s*\[\s*(?P<idx>[^\]]+)\s*\]"
)
_CXX_ICE_TOKEN = re.compile(r"\bis_constant_evaluated\s*\(")
_CXX_ADDRESSOF = re.compile(r"\baddressof\s*\(")
_ADDRESSOF_IN_INDEX = re.compile(
    r"\[\s*\*\s*(?:std\s*::\s*)?\baddressof\s*\("
)
_ADDRESSOF_BIND = re.compile(
    r"\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?\baddressof\s*\("
    r"\s*(?P<src>[A-Za-z_]\w*)"
)
_ADDRESSOF_INLINE_RET = re.compile(
    r"\breturn\s+(?:std\s*::\s*)?\baddressof\s*\("
)
_CXX_ASSUME_ALIGNED = re.compile(
    r"\bassume_aligned\s*(?:<\s*[^>]*\s*>)?\s*\("
)
_ASMALIGN_INLINE_IDX = re.compile(
    r"\b(?:std\s*::\s*)?\bassume_aligned\s*(?:<\s*[^>]*\s*>)?\s*\([^)]*\)\s*"
    r"\[\s*(?P<idx>[^\]]+)\s*\]"
)
_ASMALIGN_BIND = re.compile(
    r"\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?\bassume_aligned"
    r"\s*(?:<\s*[^>]*\s*>)?\s*\("
)
_CXX_AS_CONST = re.compile(r"\bas_const\s*\(")
_AS_CONST_BIND = re.compile(
    r"\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?\bas_const\s*\("
    r"\s*(?P<src>[A-Za-z_]\w*)"
)
_AS_CONST_CAST_WRITE = re.compile(
    r"const_cast\s*<[^>]+>\s*\(\s*(?P<name>[A-Za-z_]\w*)\s*\)\s*="
)
_CXX_EXCLUSIVE_SCAN = re.compile(r"\bexclusive_scan\s*\(")
_EXSCAN_CALL = re.compile(
    r"\bexclusive_scan\s*\(\s*(?P<src>[A-Za-z_]\w*)\s*,"
    r"[^,]+,\s*(?P<dst>[A-Za-z_]\w*)"
)
_CXX_MAKE_EPTR = re.compile(r"\bmake_exception_ptr\s*\(")
_MAKE_EPTR_INLINE = re.compile(
    r"\brethrow_exception\s*\(\s*(?:std\s*::\s*)?make_exception_ptr\s*\("
)
_MAKE_EPTR_BIND = re.compile(
    r"\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?make_exception_ptr\s*\("
)
_CXX_SET_TERMINATE = re.compile(r"\b(?:set_terminate|get_terminate)\s*\(")
_SET_TERM_CALL = re.compile(r"\bset_terminate\s*\(")
_GET_TERM_BIND = re.compile(
    r"\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?get_terminate\s*\("
)
_CXX_INCLUSIVE_SCAN = re.compile(r"\binclusive_scan\s*\(")
_INSCAN_CALL = re.compile(
    r"\binclusive_scan\s*\(\s*(?P<src>[A-Za-z_]\w*)\s*,"
    r"[^,]+,\s*(?P<dst>[A-Za-z_]\w*)"
)
_CXX_TRANSFORM_REDUCE = re.compile(r"\btransform_reduce\s*\(")
_CXX_TRED_FN = r"(?:std\s*::\s*)?transform_reduce"
_CXX_TRED_TOKEN = re.compile(rf"\b{_CXX_TRED_FN}\s*\(")
_TRED_IN_INDEX = re.compile(rf"\[\s*{_CXX_INT_CAST}\s*{_CXX_TRED_FN}\s*\(")
_TRED_ASSIGN = re.compile(
    rf"\b(?P<var>[A-Za-z_]\w*)\s*=\s*{_CXX_INT_CAST}\s*{_CXX_TRED_FN}\s*\("
)
_CXX_REDUCE_FN = r"std\s*::\s*reduce"
_CXX_REDUCE_TOKEN = re.compile(rf"\b{_CXX_REDUCE_FN}\s*\(")
_REDUCE_IN_INDEX = re.compile(rf"\[\s*{_CXX_INT_CAST}\s*{_CXX_REDUCE_FN}\s*\(")
_REDUCE_ASSIGN = re.compile(
    rf"\b(?P<var>[A-Za-z_]\w*)\s*=\s*{_CXX_INT_CAST}\s*{_CXX_REDUCE_FN}\s*\("
)
_CXX_UNINITIALIZED_COPY = re.compile(
    r"\b(?:uninitialized_copy|uninitialized_move)\s*\("
)
_UICOPY_CALL = re.compile(
    r"\b(?:uninitialized_copy|uninitialized_move)\s*\(\s*(?P<src>[A-Za-z_]\w*)\s*,"
    r"[^,]+,\s*(?P<dst>[A-Za-z_]\w*)"
)
_CXX_CONSTRUCT_AT = re.compile(r"\b(?:construct_at|destroy_at)\s*\(")
_CONSTRUCT_AT_CALL = re.compile(
    r"\bconstruct_at\s*(?:<\s*[^>]*\s*>)?\s*\(\s*(?P<ptr>[A-Za-z_]\w*)"
)
_DESTROY_AT_CALL = re.compile(
    r"\bdestroy_at\s*(?:<\s*[^>]*\s*>)?\s*\(\s*(?P<ptr>[A-Za-z_]\w*)"
)
_CXX_FWDLIKE_FN = r"(?:std\s*::\s*)?forward_like\s*(?:<\s*[^>]*\s*>)?"
_CXX_FWDLIKE_TOKEN = re.compile(rf"\b{_CXX_FWDLIKE_FN}\s*\(")
_FWDLIKE_IN_INDEX = re.compile(
    rf"\[\s*{_CXX_INT_CAST}\s*{_CXX_FWDLIKE_FN}\s*\("
)
_FWDLIKE_ASSIGN = re.compile(
    rf"\b(?P<var>[A-Za-z_]\w*)\s*=\s*{_CXX_INT_CAST}\s*{_CXX_FWDLIKE_FN}\s*\("
)
_FWDLIKE_SRC = re.compile(
    rf"\b{_CXX_FWDLIKE_FN}\s*\(\s*(?P<src>[A-Za-z_]\w*)"
)
_CXX_UNINITIALIZED_FILL = re.compile(
    r"\b(?:uninitialized_fill|uninitialized_default_construct)\s*\("
)
_UIFILL_CALL = re.compile(
    r"\b(?:uninitialized_fill|uninitialized_default_construct)\s*\(\s*"
    r"(?P<dst>[A-Za-z_]\w*)"
)
_CXX_DESTROY_N = re.compile(r"\bdestroy_n\s*\(")
_DESTROY_N_CALL = re.compile(
    r"\bdestroy_n\s*(?:<\s*[^>]*\s*>)?\s*\(\s*(?P<ptr>[A-Za-z_]\w*)"
    r"\s*,\s*(?P<n>[^,\)]+)"
)
_CXX_ADDSAT_FN = (
    r"(?:std\s*::\s*)?(?:add_sat|sub_sat|mul_sat|div_sat|saturate_cast)"
    r"\s*(?:<\s*[^>]*\s*>)?"
)
_CXX_ADDSAT_TOKEN = re.compile(rf"\b{_CXX_ADDSAT_FN}\s*\(")
_ADDSAT_IN_INDEX = re.compile(rf"\[\s*{_CXX_INT_CAST}\s*{_CXX_ADDSAT_FN}\s*\(")
_ADDSAT_ASSIGN = re.compile(
    rf"\b(?P<var>[A-Za-z_]\w*)\s*=\s*{_CXX_INT_CAST}\s*{_CXX_ADDSAT_FN}\s*\("
)
_CXX_TRANSFORM_SCAN = re.compile(
    r"\b(?:transform_inclusive_scan|transform_exclusive_scan)\s*\("
)
_TSCAN_CALL = re.compile(
    r"\b(?:transform_inclusive_scan|transform_exclusive_scan)\s*\(\s*"
    r"(?P<src>[A-Za-z_]\w*)\s*,[^,]+,\s*(?P<dst>[A-Za-z_]\w*)"
)
_CXX_TYPE_IDENTITY = re.compile(r"\btype_identity(?:_t)?\s*<")
_TYPEIDENT_ALIAS = re.compile(
    r"\busing\s+(?P<alias>[A-Za-z_]\w*)\s*=\s*"
    r"(?:typename\s+)?(?:std\s*::\s*)?type_identity"
)
_TYPEIDENT_DIRECT_PTR = re.compile(
    r"(?:std\s*::\s*)?type_identity(?:_t)?\s*<[^>;]+>"
    r"(?:\s*::\s*type)?\s*\*\s*(?P<ptr>[A-Za-z_]\w*)"
)
_CXX_NONTYPE = re.compile(r"\bnontype\b")
_CXX_LAYOUT_COMPATIBLE = re.compile(r"\bis_layout_compatible\b")
_CXX_PTR_INTERCONV = re.compile(
    r"\bis_pointer_interconvertible_(?:with_class|base_of)\b"
)
_CXX_UNINITIALIZED_VALUE = re.compile(
    r"\buninitialized_value_construct\s*\("
)
_UVALUE_CALL = re.compile(
    r"\buninitialized_value_construct\s*\(\s*(?P<dst>[A-Za-z_]\w*)"
)
_CXX_BASIC_CONST_ITER = re.compile(r"\bbasic_const_iterator\b")
_BCITER_DECL = re.compile(
    r"(?:std\s*::\s*)?basic_const_iterator\s*<[^>]+>\s+"
    r"(?P<name>[A-Za-z_]\w*)"
)
_BCITER_BIND = re.compile(
    r"\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?basic_const_iterator\b"
)
_CXX_CORR_MEMBER = re.compile(r"\bis_corresponding_member\b")
_CXX_RANGES_TO = re.compile(r"\branges\s*::\s*to\s*(?:<|\()")
_RANGES_TO_BIND = re.compile(
    r"\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?ranges\s*::\s*to\b"
)
_CXX_ENUMERATE = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*enumerate\b|\benumerate_view\b)"
)
_CXX_CARTESIAN = re.compile(r"\bcartesian_product\b")
_CXX_CHUNK = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*chunk\b|\bchunk_by\b|\bchunk_view\b)"
)
_CXX_SLIDE = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*slide\b|\bslide_view\b)"
)
_CXX_ADJACENT = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*adjacent\b"
    r"|\badjacent_transform\b|\badjacent_view\b)"
)
_CXX_JOIN_WITH = re.compile(r"\bjoin_with(?:_view)?\b")
_CXX_ZIP_TRANSFORM = re.compile(r"\bzip_transform(?:_view)?\b")
_CXX_AS_RVALUE = re.compile(r"\bas_rvalue(?:_view)?\b")
_CXX_FROM_RANGE = re.compile(r"\bfrom_range\b")
_CXX_SCOPED_ENUM = re.compile(r"\bis_scoped_enum\b")
_CXX_STRIDE = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*stride\b|\bstride_view\b)"
)
_CXX_REPEAT = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*repeat\b|\brepeat_view\b)"
)
_CXX_TAKE = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*take\b|\btake_view\b)"
)
_CXX_DROP = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*drop\b|\bdrop_view\b)"
)
_CXX_FILTER = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*filter\b|\bfilter_view\b)"
)
_CXX_TRANSFORM_VIEW = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*transform\b|\btransform_view\b)"
)
_CXX_ELEMENTS = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*elements\b|\belements_view\b)"
)
_CXX_IOTA = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*iota\b|\biota_view\b)"
)
_CXX_TAKE_WHILE = re.compile(r"\btake_while(?:_view)?\b")
_CXX_DROP_WHILE = re.compile(r"\bdrop_while(?:_view)?\b")
_CXX_KEYS = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*keys\b|\bkeys_view\b)"
)
_CXX_VALUES = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*values\b|\bvalues_view\b)"
)
_CXX_REVERSE_VIEW = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*reverse\b|\breverse_view\b)"
)
_CXX_COUNTED = re.compile(
    r"(?:(?:std\s*::\s*)?views\s*::\s*counted\b|\bcounted_view\b)"
)


def _pointer_param_names(fn) -> list[str]:
    return [name for typ, name in fn.params if name and "*" in typ]


_PARAM_NULL_TEST: dict[str, re.Pattern[str]] = {}


def _param_null_tested(name: str, body: str) -> bool:
    # Every test below names `name` literally.
    if name not in body:
        return False
    pat = _PARAM_NULL_TEST.get(name)
    if pat is None:
        v = re.escape(name)
        tests = (
            rf"\bif\s*\(\s*{v}\s*==\s*(?:NULL|nullptr|0)\b",
            rf"\bif\s*\(\s*(?:NULL|nullptr|0)\s*==\s*{v}\b",
            rf"\bif\s*\(\s*{v}\s*!=\s*(?:NULL|nullptr|0)\b",
            rf"\bif\s*\(\s*(?:NULL|nullptr|0)\s*!=\s*{v}\b",
            rf"\bif\s*\(\s*!\s*{v}\b",
            # A test inside a larger condition (cJSON 1.7.18 cJSON_SetValuestring:
            # `if (object->valuestring == NULL || valuestring == NULL)`).
            rf"(?:\|\||&&|\()\s*\(?\s*{v}\s*[!=]=\s*(?:NULL|nullptr|0)\b",
            rf"(?:\|\||&&|\()\s*\(?\s*(?:NULL|nullptr|0)\s*[!=]=\s*{v}\b",
            rf"(?:\|\||&&|\()\s*!\s*{v}\b(?!\s*(?:->|\.|\[|\())",
        )
        # One alternation matches somewhere iff one of the tests does.
        pat = re.compile("|".join(f"(?:{t})" for t in tests))
        _PARAM_NULL_TEST[name] = pat
    return bool(pat.search(body))


def _mem_leak(lines, rel, funcs, out) -> None:
    """Local malloc/calloc/realloc freed on some returns but not others."""
    for fn in funcs:
        # UNCHECKED_ALLOC names malloc/calloc/realloc; no alloc, no finding.
        if "alloc" not in fn.body:
            continue
        body_lines = fn.body.splitlines()
        allocs: dict[str, list[int]] = {}
        frees: dict[str, list[int]] = {}
        for i, ln in enumerate(body_lines):
            for m in UNCHECKED_ALLOC.finditer(ln):
                allocs.setdefault(_norm_var(m.group("var")), []).append(i)
            for m in FREE_CALL.finditer(ln):
                frees.setdefault(_norm_var(m.group("var")), []).append(i)
        returns = [
            i for i, ln in enumerate(body_lines)
            if _rx(r"\s*return\b").match(ln)
        ]
        if len(returns) < 2:
            continue
        for var, alloc_hits in allocs.items():
            free_hits = frees.get(var, [])
            if not free_hits:
                continue
            # `if (!p) return 0;` leaves with p NULL: not a path that holds p.
            var_returns = [
                r for r in returns
                if not _null_guarded_return(var, body_lines, r)
            ]
            if len(var_returns) < 2:
                continue
            held_returns = []
            for r in var_returns:
                last_alloc = max((h for h in alloc_hits if h < r), default=-1)
                last_free = max((h for h in free_hits if h < r), default=-1)
                if last_alloc > last_free:
                    held_returns.append(r)
            if held_returns and len(held_returns) < len(var_returns):
                ln = fn.span[0] + held_returns[0]
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=ln, cls="MEM-LEAK",
                    message=f"{var} allocated on {len(alloc_hits)} path(s) but freed "
                    f"before only {len(var_returns) - len(held_returns)} of "
                    f"{len(var_returns)} returns",
                    strength=laws.STRENGTH_FINDS,
                    evidence=body_lines[held_returns[0]].strip(),
                ))


_NULL_GUARD: dict[str, re.Pattern[str]] = {}


def _null_guard_re(var: str) -> re.Pattern[str]:
    pat = _NULL_GUARD.get(var)
    if pat is None:
        v = re.escape(var)
        pat = _NULL_GUARD[var] = re.compile(
            rf"\bif\s*\(\s*(?:!\s*{v}\b|{v}\s*==\s*(?:NULL|nullptr|0)\b"
            rf"|(?:NULL|nullptr|0)\s*==\s*{v}\b"
            rf"|{v}\s*<\s*0\b|{v}\s*==\s*-\s*1\b"
            rf"|\(\s*{v}\s*=(?!=)[^;]*?\)\s*==\s*(?:NULL|nullptr|0)\b)"
        )
    return pat


def _null_guarded_return(var: str, body_lines: list[str], r: int) -> bool:
    """True when the return on line r sits under an `if (!var)`-style test.

    `if (fd < 0)` / `if (fd == -1)` count too: the open failed.

    Looks at the return line, an unbraced `if` on the line before, and the
    head of the innermost block holding the return.
    """
    pat = _null_guard_re(var)
    if pat.search(body_lines[r]):
        return True
    if r > 0:
        prev = body_lines[r - 1].rstrip()
        if prev and prev[-1] not in ";{}" and pat.search(prev):
            return True
    depth = 0
    for j in range(r - 1, -1, -1):
        ln = body_lines[j]
        for k in range(len(ln) - 1, -1, -1):
            c = ln[k]
            if c == "}":
                depth += 1
            elif c == "{":
                depth -= 1
                if depth < 0:
                    head = ln[:k]
                    if not head.strip() and j > 0:
                        head = body_lines[j - 1]
                    return bool(pat.search(head))
    return False


def _param_if_guard(name: str, body: str) -> bool:
    """`if (name …)` or `if (!name)` guard anywhere in the body."""
    v = re.escape(name)
    if re.search(rf"\bif\s*\(\s*!\s*{v}\b", body):
        return True
    return bool(re.search(rf"\bif\s*\(\s*{v}\b", body))


def _ptr_index_arith(p: str, n: str, text: str) -> bool:
    pe, ne = re.escape(p), re.escape(n)
    if re.search(rf"\*\s*\(\s*{pe}\s*\+\s*{ne}\s*\)", text):
        return True
    return bool(re.search(rf"\b{pe}\s*\[\s*{ne}\s*\]", text))


def _ptr_arith(lines, rel, funcs, out) -> None:
    """Pointer param indexed by int param without bound/null guards."""
    for fn in funcs:
        if fn.kind != "POINTER":
            continue
        ptrs = _pointer_param_names(fn)
        ints = [name for name, _ in _integer_param_names(fn)]
        if not ptrs or not ints:
            continue
        body = fn.body
        body_lines = body.splitlines()
        start = fn.span[0]
        reported = False
        for p in ptrs:
            if reported:
                break
            for n in ints:
                if not _ptr_index_arith(p, n, body):
                    continue
                if _param_if_guard(p, body) and _param_if_guard(n, body):
                    continue
                pe, ne = re.escape(p), re.escape(n)
                for i, ln in enumerate(body_lines):
                    if not (
                        re.search(rf"\*\s*\(\s*{pe}\s*\+\s*{ne}\s*\)", ln)
                        or re.search(rf"\b{pe}\s*\[\s*{ne}\s*\]", ln)
                    ):
                        continue
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="MEM-PTR-ARITH",
                        message=f"{p}[{n}] or *({p}+{n}) without guard on "
                        f"{p} or {n}",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else "",
                    ))
                    reported = True
                    break


_FLOAT_ZERO_DIV = re.compile(
    r"/\s*0\.0(?:[fF])?\b"
    r"|/\s*0\.(?![0-9])",
)


def _float_ub(lines, rel, funcs, out) -> None:
    """Divide by float/double zero literal (SCALAR/VOID only)."""
    for fn in funcs:
        if fn.kind not in ("SCALAR", "VOID"):
            continue
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            if not _FLOAT_ZERO_DIV.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="FLOAT-UB",
                message="division by floating zero literal",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))


def _str_null_arg(lines, rel, funcs, out) -> None:
    """String API on a pointer parameter never tested against NULL."""
    for fn in funcs:
        if fn.kind != "POINTER":
            continue
        ptr_params = _pointer_param_names(fn)
        if not ptr_params:
            continue
        unchecked = [
            p for p in ptr_params if not _param_null_tested(p, fn.body)
        ]
        if not unchecked:
            continue
        start = fn.span[0]
        reported = False
        for i, ln in enumerate(fn.body.splitlines()):
            if reported:
                break
            for callee, arg_idxs in _STR_NULL_FN.items():
                args = _find_call_args(ln, callee)
                if args is None:
                    continue
                for idx in arg_idxs:
                    if idx >= len(args):
                        continue
                    arg = args[idx].strip()
                    if not _rx(r"^[A-Za-z_]\w*$").match(arg):
                        continue
                    if arg not in unchecked:
                        continue
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="STR-NULL-ARG",
                        message=f"{callee}() called with unchecked pointer "
                        f"parameter {arg}",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else "",
                    ))
                    reported = True
                    break
                if reported:
                    break


def _move_use_after(name: str, ln: str) -> bool:
    stripped = _STD_MOVE.sub("", ln)
    v = re.escape(name)
    if re.search(rf"\b{v}\.", stripped):
        return True
    if re.search(rf"\b{v}\s*\(", stripped):
        return True
    return bool(re.search(rf"\b{v}\s*=(?!=)", stripped))


def _cxx_local_strings(fn) -> set[str]:
    params = {name for typ, name in fn.params if name}
    names: set[str] = set()
    for m in _lit_finditer(_CXX_LOCAL_STRING, fn.body):
        if m.group("static"):
            continue
        name = m.group("name")
        if name in params or name in _KW:
            continue
        names.add(name)
    return names


def _cxx_dangling_ref(lines, rel, funcs, out) -> None:
    """Return of string_view/span built from a local string that dies."""
    for fn in funcs:
        if not _CXX_VIEW_RET.search(fn.return_type or ""):
            continue
        local_str = _cxx_local_strings(fn)
        if not local_str:
            continue
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            hit_name = None
            m = _RETURN_VAR.search(ln)
            if m and m.group(1) in local_str:
                hit_name = m.group(1)
            else:
                m = _CXX_RETURN_VIEW_OF.search(ln)
                if m and m.group("name") in local_str:
                    hit_name = m.group("name")
            if not hit_name:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-DANGLING-REF",
                message=f"return borrows local string {hit_name}",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))
            break


def _hive_container_names(fn) -> set[str]:
    names = {m.group("name") for m in _CXX_HIVE_DECL.finditer(fn.body or "")}
    for typ, pname in fn.params:
        if pname and _rx(r"\bhive\s*<").search(typ or ""):
            names.add(pname)
    return names


def _cxx_iter_invalid(lines, rel, funcs, out) -> None:
    """Iterator from .begin() used after container push_back/insert/erase/clear.

    Hive plants stay on CXX-HIVE (`std::hive<int>`), not this class.
    """
    for fn in funcs:
        hive_names = _hive_container_names(fn)
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        begin_containers: set[str] = set()
        for i, ln in enumerate(body_lines):
            if "=" in ln and ".begin(" in ln:
                for m in _CXX_BEGIN_CALL.finditer(ln):
                    begin_containers.add(_norm_var(m.group("container")))
            for m in _CXX_CONTAINER_MUTATE.finditer(ln):
                cont = _norm_var(m.group("container"))
                if cont not in begin_containers:
                    continue
                if cont in hive_names:
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-ITERATOR-INVALID",
                    message=f"{cont} mutated after .begin() iterator taken",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))
                return


def _cxx_use_after_move(lines, rel, funcs, out) -> None:
    """Use of a value after std::move in the same function."""
    for fn in funcs:
        body_lines = fn.body.splitlines()
        last_move: dict[str, int] = {}
        for i, ln in enumerate(body_lines):
            for m in _STD_MOVE.finditer(ln):
                last_move[m.group("name")] = i
        for name, move_i in last_move.items():
            for j in range(move_i + 1, len(body_lines)):
                if not _move_use_after(name, body_lines[j]):
                    continue
                line = fn.span[0] + j
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-USE-AFTER-MOVE",
                    message=f"{name} used after std::move",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))
                break


def _lambda_ref_locals(capture: str, locals_before: set[str]) -> set[str]:
    cap = _rx(r"\s+").sub("", capture)
    if "&" not in cap:
        return set()
    if cap.startswith("=") and "&" not in cap[1:]:
        return set()
    if cap.startswith("&"):
        if cap == "&" or cap.startswith("&,"):
            val_only: set[str] = set()
            if cap.startswith("&,"):
                for part in cap[2:].split(","):
                    if not part or part.startswith("&"):
                        continue
                    val_only.add(re.split(r"[=:]", part)[0])
            return locals_before - val_only
        m = _rx(r"^&([A-Za-z_]\w*)").match(cap)
        if m and m.group(1) in locals_before:
            return {m.group(1)}
    refs: set[str] = set()
    for m in _rx(r",&([A-Za-z_]\w*)").finditer(cap):
        if m.group(1) in locals_before:
            refs.add(m.group(1))
    return refs


def _cxx_lambda_dangle(lines, rel, funcs, out) -> None:
    """Return of a lambda capturing a local by reference."""
    for fn in funcs:
        params = {name for typ, name in fn.params if name}
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        locals_so_far: set[str] = set()
        returned_lambdas: dict[str, set[str]] = {}
        for i, ln in enumerate(body_lines):
            m = _CXX_LOCAL_LINE.match(ln)
            if m:
                name = m.group("name")
                if name not in params and name not in _KW:
                    locals_so_far.add(name)
            m = _CXX_AUTO_LAMBDA.search(ln)
            if m:
                refs = _lambda_ref_locals(m.group("capture"), locals_so_far)
                if refs:
                    returned_lambdas[m.group("name")] = refs
            m = _CXX_RETURN_LAMBDA.search(ln)
            if m:
                refs = _lambda_ref_locals(m.group("capture"), locals_so_far)
                if refs:
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="CXX-LAMBDA-DANGLE",
                        message="returned lambda captures local by reference",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else "",
                    ))
                    break
            m = _CXX_RETURN_NAME.search(ln)
            if m and m.group("name") in returned_lambdas:
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-LAMBDA-DANGLE",
                    message="returned lambda captures local by reference",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))
                break


def _unique_ptr_used(raw: str, ln: str) -> bool:
    v = re.escape(raw)
    if _UNIQUE_RAW_GET.search(ln):
        return False
    if re.search(rf"(?<!\w)\*\s*{v}\b", ln):
        if re.search(
            rf"^\s*(?:const\s+)?(?:unsigned\s+)?"
            rf"(?:int|char|void|auto|short|long|float|double|"
            rf"size_t|std::\w+)\s+\*\s*{v}\s*=",
            ln,
        ):
            return False
        return True
    if re.search(rf"\b{v}\s*->", ln):
        return True
    if re.search(rf"\b{v}\s*\[", ln):
        return True
    if re.search(rf"\([^)]*\b{v}\b[^)]*\)", ln):
        return True
    return False


def _cxx_unique_reset(lines, rel, funcs, out) -> None:
    """Use of raw pointer from unique_ptr::get after reset/null assignment."""
    for fn in funcs:
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        unique_ptrs: set[str] = set()
        raw_owner: dict[str, str] = {}
        reset_at: dict[str, int] = {}
        for i, ln in enumerate(body_lines):
            for m in _UNIQUE_PTR_DECL.finditer(ln):
                unique_ptrs.add(m.group("name"))
            sm = _UNIQUE_RAW_GET.search(ln)
            if sm and sm.group("up") in unique_ptrs:
                raw_owner[sm.group("raw")] = sm.group("up")
            for m in _UNIQUE_RESET.finditer(ln):
                up = m.group("up")
                if up in unique_ptrs:
                    reset_at.setdefault(up, i)
            sm = _UNIQUE_NULL.search(ln)
            if sm and sm.group("up") in unique_ptrs:
                reset_at.setdefault(sm.group("up"), i)
            for raw, up in raw_owner.items():
                rline = reset_at.get(up)
                if rline is None or i <= rline:
                    continue
                if not _unique_ptr_used(raw, ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-UNIQUE-RESET",
                    message=f"{raw} from {up}.get() used after reset",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))
                return


def _const_objects(fn) -> set[str]:
    objs: set[str] = set()
    for typ, name in fn.params:
        if name and _rx(r"\bconst\b").search(typ):
            objs.add(name)
    for m in _lit_finditer(_CONST_LOCAL, fn.body):
        objs.add(m.group("name"))
    return objs


def _cxx_const_cast(lines, rel, funcs, out) -> None:
    """const_cast strips const from a const object then writes through it."""
    for fn in funcs:
        const_objs = _const_objects(fn)
        if not const_objs:
            continue
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            m = _CONST_CAST_WRITE.search(ln)
            if not m:
                continue
            name = m.group("name")
            if name not in const_objs:
                continue
            if _rx(r"\bconst\b").search(m.group("target")):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-CONST-CAST",
                message=f"write through const_cast of const object {name}",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))
            break


def _dyn_cast_used(var: str, ln: str) -> bool:
    if _null_test_for(var, ln):
        return False
    v = re.escape(var)
    if re.search(rf"\b{v}\s*->", ln):
        return True
    if re.search(rf"\*\s*{v}\b", ln):
        return True
    if re.search(rf"\b{v}\s*\[", ln):
        return True
    return bool(re.search(rf"\([^)]*\b{v}\b", ln))


def _cxx_dynamic_cast_null(lines, rel, funcs, out) -> None:
    """dynamic_cast result dereferenced or passed on without a NULL/nullptr test."""
    for fn in funcs:
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            m = _DYN_CAST_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            use_j = None
            for j in range(i + 1, len(body_lines)):
                if _dyn_cast_used(var, body_lines[j]):
                    use_j = j
                    break
            if use_j is None:
                continue
            between = "\n".join(body_lines[i:use_j])
            if _null_test_for(var, between):
                continue
            line = start + use_j
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-DYNAMIC-CAST-NULL",
                message=f"{var} from dynamic_cast used without a null test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))
            break


def _cxx_pointee(typ: str) -> str:
    t = _rx(r"\b(?:const|volatile)\b").sub("", typ)
    t = _rx(r"\s+").sub(" ", t).strip()
    if "*" in t:
        t = t[: t.index("*")].strip()
    elif t.endswith("&"):
        t = t[:-1].strip()
    return t.lower()


def _allowed_reinterp_pointee(typ: str) -> bool:
    p = _cxx_pointee(typ)
    if p == "void":
        return True
    return p in {"char", "signed char", "unsigned char"}


def _cxx_ptr_decls(fn) -> dict[str, str]:
    decls: dict[str, str] = {}
    for typ, name in fn.params:
        if name and "*" in typ:
            decls[name] = typ
    for m in _lit_finditer(_CXX_PTR_DECL_LINE, fn.body):
        decls[m.group("name")] = m.group("typ").strip()
    return decls


def _cxx_scalar_decls(fn) -> dict[str, str]:
    decls: dict[str, str] = {}
    for typ, name in fn.params:
        if name and "*" not in typ and "&" not in typ:
            decls[name] = typ
    for m in _lit_finditer(_CXX_SCALAR_LOCAL, fn.body):
        decls[m.group("name")] = m.group("typ").strip()
    return decls


def _reinterp_source_type(
    src: str, ptr_decls: dict[str, str], scalar_decls: dict[str, str],
) -> str | None:
    src = src.strip()
    m = _rx(r"&\s*([A-Za-z_]\w*)").match(src)
    if m:
        name = m.group(1)
        if name in scalar_decls:
            return scalar_decls[name] + " *"
        return None
    m = _rx(r"([A-Za-z_]\w*)").match(src)
    if m:
        name = m.group(1)
        if name in ptr_decls:
            return ptr_decls[name]
    return None


def _cxx_reinterpret(lines, rel, funcs, out) -> None:
    """reinterpret_cast between distinct object pointer types (not void/char*)."""
    for fn in funcs:
        ptr_decls = _cxx_ptr_decls(fn)
        scalar_decls = _cxx_scalar_decls(fn)
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            for m in _REINTERPRET_CAST.finditer(ln):
                target = m.group("target").strip()
                if _allowed_reinterp_pointee(target):
                    continue
                src_typ = _reinterp_source_type(
                    m.group("src"), ptr_decls, scalar_decls,
                )
                if src_typ is None or _allowed_reinterp_pointee(src_typ):
                    continue
                if _cxx_pointee(target) == _cxx_pointee(src_typ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-REINTERPRET",
                    message="reinterpret_cast type-puns between object pointers",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))
                break


def _catch_ref_name(clause: str) -> str | None:
    clause = clause.strip()
    if "..." in clause:
        return None
    if "&" not in clause and "*" not in clause:
        return None
    m = _rx(r"([A-Za-z_]\w*)\s*$").search(clause)
    return m.group(1) if m else None


def _cxx_throw_copy(lines, rel, funcs, out) -> None:
    """`throw e;` inside catch copies/slices the caught exception."""
    for fn in funcs:
        body = fn.body
        start = fn.span[0]
        pos = 0
        while True:
            m = _CATCH_BLOCK.search(body, pos)
            if not m:
                break
            name = _catch_ref_name(m.group("clause"))
            brace = m.end() - 1
            close = _match_brace(body, brace)
            if close < 0:
                pos = m.end()
                continue
            catch_body = body[brace + 1: close]
            if name:
                catch_start = start + body[: brace + 1].count("\n")
                for i, ln in enumerate(catch_body.splitlines()):
                    tm = _THROW_IDENT.search(ln)
                    if tm and tm.group("ident") == name:
                        line = catch_start + i
                        out.append(Finding(
                            stage="lints", status=laws.FAILED, file=rel,
                            function=fn.name, line=line, cls="CXX-THROW-COPY",
                            message=f"throw {name} copies caught exception; use throw;",
                            strength=laws.STRENGTH_FINDS,
                            evidence=lines[line - 1].strip()
                            if 0 < line <= len(lines) else "",
                        ))
                        break
            pos = close + 1


def _bit_cast_src_is_obj_ptr(src: str, ptr_decls: dict[str, str]) -> bool:
    src = src.strip()
    if "*" in src or src.startswith("&"):
        src_typ = _reinterp_source_type(src, ptr_decls, {})
        if src_typ is None:
            return True
        return not _allowed_reinterp_pointee(src_typ)
    m = _rx(r"([A-Za-z_]\w*)").match(src)
    if not m:
        return False
    name = m.group(1)
    if name not in ptr_decls:
        return False
    return not _allowed_reinterp_pointee(ptr_decls[name])


def _cxx_bit_cast(lines, rel, funcs, out) -> None:
    """bit_cast between object pointers or pointer and integer."""
    for fn in funcs:
        ptr_decls = dict(_cxx_ptr_decls(fn))
        for m in _lit_finditer(_CXX_PTR_DECL_ANY, fn.body):
            ptr_decls.setdefault(m.group("name"), m.group("typ").strip())
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            for m in _BIT_CAST.finditer(ln):
                tgt = m.group("tgt").strip()
                src = m.group("src").strip()
                tgt_obj_ptr = "*" in tgt and not _allowed_reinterp_pointee(tgt)
                src_obj_ptr = _bit_cast_src_is_obj_ptr(src, ptr_decls)
                if not tgt_obj_ptr and not src_obj_ptr:
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-BIT-CAST",
                    message="bit_cast type-puns an object pointer",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))
                return


def _place_storage_ok(name: str, fn) -> bool | None:
    """True if placement storage is unsigned char/byte buffer or void*.

    False if `name` is a typed object pointer. None if unknown.
    """
    blob = fn.body or ""
    for typ, pname in fn.params:
        if pname != name:
            continue
        if "*" not in typ:
            return None
        return _allowed_reinterp_pointee(typ)
    if any(m.group("name") == name for m in _PLACE_BUF_DECL.finditer(blob)):
        return True
    last_typ = None
    for m in _PLACE_PTR_DECL.finditer(blob):
        if m.group("name") == name:
            last_typ = m.group("typ")
    if last_typ is None:
        return None
    t = _rx(r"\s+").sub(" ", last_typ).strip().lower()
    t = t.replace("std::", "")
    if t in {"unsigned char", "byte", "void"}:
        return True
    return False


def _cxx_placement_new(lines, rel, funcs, out) -> None:
    """placement new (p) where p is a typed object pointer, not a byte buffer."""
    for fn in funcs:
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            m = _PLACE_NEW.search(ln)
            if not m:
                continue
            ptr = m.group("ptr")
            if ptr in _PLACE_SKIP_PTR:
                continue
            ok = _place_storage_ok(ptr, fn)
            if ok is not False:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-PLACEMENT-NEW",
                message=f"placement new into typed object pointer {ptr}",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))
            return


def _cxx_std_thread(lines, rel, funcs, out) -> None:
    """std::thread local not join()'d or detach()'d."""
    for fn in funcs:
        body = fn.body or ""
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            m = _CXX_THREAD_DECL.search(ln)
            if not m:
                continue
            name = m.group("name")
            n = re.escape(name)
            if re.search(rf"\b{n}\s*\.\s*(?:join|detach)\s*\(", body):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-STD-THREAD",
                message=f"{name} is neither join()'d nor detach()'d",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))
            return


def _optional_has_guard(name: str, body: str) -> bool:
    n = re.escape(name)
    if re.search(rf"\b{n}\s*\.\s*has_value\s*\(", body):
        return True
    if re.search(rf"\bif\s*\(\s*!?\s*{n}\s*\)", body):
        return True
    if re.search(rf"\bif\s*\(\s*{n}\s*\.\s*operator\s*bool", body):
        return True
    return False


def _optional_deref(name: str, ln: str) -> bool:
    """operator* / operator-> only. `.value()` is CXX-OPTIONAL-VALUE."""
    n = re.escape(name)
    if re.search(rf"(?<!\w)\*\s*{n}\b", ln):
        return True
    return bool(re.search(rf"\b{n}\s*->", ln))


def _cxx_optional_null(lines, rel, funcs, out) -> None:
    """Dereference of std::optional without has_value()/operator bool."""
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_OPTIONAL_DECL, body)}
        if not names:
            continue
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            for name in sorted(names):
                if _optional_has_guard(name, body):
                    continue
                if not _optional_deref(name, ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-OPTIONAL-NULL",
                    message=f"{name} dereferenced without has_value()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))
                return


def _cxx_variant_get(lines, rel, funcs, out) -> None:
    """std::get<T>(v) without holds_alternative<T>(v)."""
    for fn in funcs:
        body = fn.body or ""
        if _rx(r"\bemplace\s*<").search(body):
            continue
        if _rx(r"\bvalueless_by_exception\s*\(").search(body):
            continue
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            if _rx(r"\bany_cast\s*<").search(ln):
                continue
            for m in _CXX_VARIANT_GET.finditer(ln):
                var = m.group("var")
                ty = re.escape(m.group("ty").strip())
                v = re.escape(var)
                if re.search(
                    rf"\bholds_alternative\s*<\s*{ty}\s*>\s*\(\s*{v}\s*\)",
                    body,
                ):
                    continue
                if re.search(rf"\bholds_alternative\s*<[^>]+>\s*\(\s*{v}\s*\)", body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-VARIANT-GET",
                    message=f"std::get without holds_alternative on {var}",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))
                return


def _cxx_span_dangle(lines, rel, funcs, out) -> None:
    """return span<T>(local) / span<T>(local, n) of a local array or string."""
    for fn in funcs:
        ret = fn.return_type or ""
        _, arrays = _locals_in_fn(fn)
        locals_ok = set(arrays) | _cxx_local_strings(fn)
        if not locals_ok:
            continue
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            if not _rx(r"\bspan\b").search(ln) and not _rx(r"\bspan\b").search(ret):
                continue
            m = _CXX_RETURN_SPAN.search(ln)
            if not m:
                continue
            hit = m.group("name")
            if hit not in locals_ok:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-SPAN-DANGLE",
                message=f"return span constructed from local {hit}",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))
            return


def _vec_containers(fn) -> set[str]:
    names: set[str] = set()
    for typ, pname in fn.params:
        if pname and _rx(r"\b(?:vector\s*<|string)\b").search(typ):
            names.add(pname)
    for m in _CXX_VEC_STR_DECL.finditer(fn.body or ""):
        names.add(m.group("name"))
    return names


def _has_vec_size_guard(cont: str, idx: str, body: str) -> bool:
    c, i = re.escape(cont), re.escape(idx.strip())
    if re.search(
        rf"\bif\s*\(\s*{i}\s*<\s*{c}\s*\.\s*(?:size|length)\s*\(\s*\)",
        body,
    ):
        return True
    if re.search(
        rf"\bif\s*\(\s*{c}\s*\.\s*(?:size|length)\s*\(\s*\)\s*>\s*{i}",
        body,
    ):
        return True
    return False


def _cxx_vector_index(lines, rel, funcs, out) -> None:
    """v[i] on a local std::vector/string without if (i < v.size()) in the fn."""
    for fn in funcs:
        containers = _vec_containers(fn)
        if not containers:
            continue
        body = fn.body or ""
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                cont = m.group("cont")
                if cont not in containers:
                    continue
                idx = m.group("idx")
                if _has_vec_size_guard(cont, idx, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-VECTOR-INDEX",
                    message=f"{cont}[{idx.strip()}] without a size()/length() guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_catch_all(lines, rel, funcs, out) -> None:
    """Empty `catch (...) { }` swallows; `catch (...) { throw; }` is ok."""
    for fn in funcs:
        body = fn.body or ""
        start = fn.span[0]
        pos = 0
        while True:
            m = _CATCH_ALL_BLOCK.search(body, pos)
            if not m:
                break
            brace = m.end() - 1
            close = _match_brace(body, brace)
            if close < 0:
                pos = m.end()
                continue
            inner = body[brace + 1: close]
            if inner.strip():
                pos = close + 1
                continue
            line = start + body[: m.start()].count("\n")
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-CATCH-ALL",
                message="catch (...) with an empty body swallows exceptions",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))
            return


def _cxx_throw_new(lines, rel, funcs, out) -> None:
    """`throw new T` allocates the exception; `throw T()` is the ok twin."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _THROW_NEW)):
            if not _THROW_NEW.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-THROW-NEW",
                message="throw new allocates the exception object",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_uninit_member(stripped, lines, rel, funcs, out) -> None:
    """Empty ctor `C() {}` with an `int` member not in an init-list.

    extract_functions misses `C() {}` (no return type). The finding is
    attached to the next extracted function that constructs `C` and
    reads the member (`uninit_mem_bad` / `return c.x`), not the ctor
    name itself.
    """
    reported: set[str] = set()
    for m in _lit_finditer(_CLASS_DEF, stripped):
        cls = m.group("name")
        brace = m.end() - 1
        close = _match_brace(stripped, brace)
        if close < 0:
            continue
        body = stripped[brace + 1: close]
        members = [mm.group("name") for mm in _lit_finditer(_CXX_SCALAR_MEMBER, body)]
        if not members:
            continue
        ctor = re.search(
            rf"\b{re.escape(cls)}\s*\(\s*\)\s*(?::(?P<init>[^{{]*))?\s*\{{",
            body,
        )
        if not ctor:
            continue
        init = ctor.group("init") or ""
        missing = [
            mem for mem in members
            if not re.search(rf"\b{re.escape(mem)}\s*\(", init)
        ]
        if not missing:
            continue
        ctor_brace = ctor.end() - 1
        ctor_close = _match_brace(body, ctor_brace)
        ctor_body = body[ctor_brace + 1: ctor_close] if ctor_close >= 0 else ""
        still = [
            mem for mem in missing
            if not re.search(rf"\b{re.escape(mem)}\s*=", ctor_body)
        ]
        if not still:
            continue
        ctor_line = stripped[: brace + 1 + ctor.start()].count("\n") + 1
        hit_fn = None
        hit_line = ctor_line
        for fn in funcs:
            if not re.search(rf"\b{re.escape(cls)}\s+[A-Za-z_]\w*", fn.body or ""):
                continue
            start = fn.span[0]
            for i, ln in enumerate(fn.body.splitlines()):
                if any(re.search(rf"\.\s*{re.escape(mem)}\b", ln) for mem in still):
                    hit_fn = fn.name
                    hit_line = start + i
                    break
            if hit_fn:
                break
            hit_fn = fn.name
        if hit_fn is None:
            for fn in funcs:
                if fn.line >= ctor_line:
                    hit_fn = fn.name
                    break
        if hit_fn is None or hit_fn in reported:
            continue
        reported.add(hit_fn)
        mems = ", ".join(still)
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=hit_fn, line=hit_line, cls="CXX-UNINIT-MEMBER",
            message=f"{cls}() leaves scalar member {mems} uninitialised",
            strength=laws.STRENGTH_FINDS,
            evidence=lines[hit_line - 1].strip()
            if 0 < hit_line <= len(lines) else "",
        ))


def _cxx_ptr_member_names(text: str) -> set[str]:
    names: set[str] = set()
    for m in _lit_finditer(_CLASS_DEF, text):
        brace = m.end() - 1
        close = _match_brace(text, brace)
        if close < 0:
            continue
        body = text[brace + 1: close]
        for pm in _rx(r"(?:[A-Za-z_]\w*|char|int|void|short|long)\s*\*+\s*"
            r"(?P<name>[A-Za-z_]\w*)\s*;").finditer(body):
            names.add(pm.group("name"))
    return names


def _cxx_copy_assign_ptr(lines, rel, funcs, out) -> None:
    """Shallow `self.p = o.p` of a raw pointer member without `new`."""
    members = _cxx_ptr_member_names("\n".join(lines))
    if not members:
        return
    for fn in funcs:
        if _has_self_assign_guard(
            _first_ref_ptr_param(fn) or "", fn.body or "",
        ):
            continue
        start = fn.span[0]
        for i, ln in enumerate(fn.body.splitlines()):
            if _rx(r"\b(?:unique_ptr|shared_ptr|make_unique)\b").search(ln):
                continue
            if _rx(r"\bnew\b").search(ln):
                continue
            m = _SHALLOW_PTR_ASSIGN.search(ln)
            if not m:
                continue
            field = m.group("field")
            if field not in members:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-COPY-ASSIGN-PTR",
                message=f"{m.group('dst')}.{field} = {m.group('src')}.{field} "
                "shallow-copies a raw pointer member",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_volatile_cast(lines, rel, funcs, out) -> None:
    """Write through `(int*)&vol` after stripping volatile; `vol = 1` is ok."""
    for fn in funcs:
        vols = {m.group("name") for m in _VOL_LOCAL.finditer(fn.body or "")}
        if not vols:
            continue
        start = fn.span[0]
        for i, ln in enumerate(fn.body.splitlines()):
            m = _VOL_CAST_WRITE.search(ln)
            if not m or m.group("name") not in vols:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-VOLATILE-CAST",
                message=f"write through C-style cast of volatile {m.group('name')}",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_shared_get(lines, rel, funcs, out) -> None:
    """Raw pointer from shared_ptr::get used after reset/null assignment."""
    for fn in funcs:
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        shared_ptrs: set[str] = set()
        raw_owner: dict[str, str] = {}
        reset_at: dict[str, int] = {}
        hit = False
        for i, ln in enumerate(body_lines):
            for m in _SHARED_PTR_DECL.finditer(ln):
                shared_ptrs.add(m.group("name"))
            sm = _UNIQUE_RAW_GET.search(ln)
            if sm and sm.group("up") in shared_ptrs:
                raw_owner[sm.group("raw")] = sm.group("up")
            for m in _UNIQUE_RESET.finditer(ln):
                up = m.group("up")
                if up in shared_ptrs:
                    reset_at.setdefault(up, i)
            sm = _UNIQUE_NULL.search(ln)
            if sm and sm.group("up") in shared_ptrs:
                reset_at.setdefault(sm.group("up"), i)
            for raw, up in raw_owner.items():
                rline = reset_at.get(up)
                if rline is None or i <= rline:
                    continue
                if not _unique_ptr_used(raw, ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-SHARED-PTR-GET",
                    message=f"{raw} from {up}.get() used after reset",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))
                hit = True
                break
            if hit:
                break


def _cxx_auto_ptr(lines, rel, funcs, out) -> None:
    """std::auto_ptr is deprecated; unique_ptr is the ok twin."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _AUTO_PTR_USE)):
            if not _AUTO_PTR_USE.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-AUTO-PTR",
                message="std::auto_ptr is deprecated; use unique_ptr",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            break


def _cxx_string_data(lines, rel, funcs, out) -> None:
    """Pointer from string c_str()/data() used after clear/+=/append/assign."""
    for fn in funcs:
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        strings: set[str] = set()
        raw_owner: dict[str, str] = {}
        mutate_at: dict[str, int] = {}
        hit = False
        for i, ln in enumerate(body_lines):
            for m in _STR_OWN_DECL.finditer(ln):
                strings.add(m.group("name"))
            sm = _STR_CSTR_GET.search(ln)
            if sm and sm.group("s") in strings:
                raw_owner[sm.group("raw")] = sm.group("s")
            for m in _STR_MUTATE.finditer(ln):
                s = m.group("s")
                if s in strings:
                    mutate_at.setdefault(s, i)
            for raw, s in raw_owner.items():
                mline = mutate_at.get(s)
                if mline is None or i <= mline:
                    continue
                if not _unique_ptr_used(raw, ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-STRING-DATA",
                    message=f"{raw} from {s}.c_str()/data() used after mutation",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))
                hit = True
                break
            if hit:
                break


def _cxx_enable_shared(lines, rel, funcs, out) -> None:
    """shared_from_this() without enable_shared_from_this in the function."""
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_SHARED_FROM_THIS, body):
            continue
        if _lit_search(_ENABLE_SHARED_FROM, body):
            continue
        start = fn.span[0]
        hit_i = 0
        for i, ln in enumerate(fn.body.splitlines()):
            if _SHARED_FROM_THIS.search(ln):
                hit_i = i
                break
        line = start + hit_i
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=fn.name, line=line, cls="CXX-ENABLE-SHARED",
            message="shared_from_this() without enable_shared_from_this",
            strength=laws.STRENGTH_FINDS,
            evidence=lines[line - 1].strip()
            if 0 < line <= len(lines) else "",
        ))


def _cxx_fwd_ref(lines, rel, funcs, out) -> None:
    """T&& argument pushed into a container without std::forward / std::move."""
    for fn in funcs:
        rrefs: set[str] = set()
        for typ, name in fn.params:
            if name and "&&" in (typ or ""):
                rrefs.add(name)
        if not rrefs:
            continue
        body = fn.body or ""
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _FWD_PUSH_ARG)):
            m = _FWD_PUSH_ARG.search(ln)
            if not m:
                continue
            arg = m.group("arg")
            if arg not in rrefs:
                continue
            v = re.escape(arg)
            if re.search(
                rf"\bstd\s*::\s*(?:move|forward)\s*(?:<[^>]*>)?\s*\(\s*{v}\s*\)",
                body,
            ):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-FORWARDING-REF",
                message=f"{arg} is T&& but push_back copies it without "
                "std::forward/std::move",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            break


def _classes_implicit_bool(stripped: str) -> set[str]:
    """Class/struct names with operator bool() that is not explicit."""
    bad: set[str] = set()
    for m in _lit_finditer(_CLASS_DEF, stripped):
        name = m.group("name")
        brace = m.end() - 1
        close = _match_brace(stripped, brace)
        if close < 0:
            continue
        body = stripped[brace + 1: close]
        implicit = False
        for om in _lit_finditer(_OP_BOOL, body):
            if om.group("ex"):
                implicit = False
                break
            implicit = True
        if implicit:
            bad.add(name)
    return bad


def _cxx_explicit_ctor(stripped, lines, rel, funcs, out) -> None:
    """operator bool() conversion without explicit on the class."""
    bad = _classes_implicit_bool(stripped)
    if not bad:
        return
    for fn in funcs:
        body = fn.body or ""
        hit_cls = next((c for c in bad if re.search(rf"\b{re.escape(c)}\b", body)), None)
        if not hit_cls:
            continue
        start = fn.span[0]
        hit_i = 0
        for i, ln in enumerate(fn.body.splitlines()):
            if re.search(rf"\b{re.escape(hit_cls)}\b", ln):
                hit_i = i
                break
        line = start + hit_i
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=fn.name, line=line, cls="CXX-EXPLICIT-CTOR",
            message=f"{hit_cls} has operator bool() without explicit",
            strength=laws.STRENGTH_FINDS,
            evidence=lines[line - 1].strip()
            if 0 < line <= len(lines) else "",
        ))


def _const_object_names(fn) -> set[str]:
    names: set[str] = set()
    for typ, name in fn.params:
        if not name or not _rx(r"\bconst\b").search(typ or ""):
            continue
        stripped_ty = _rx(r"\b(?:const|volatile)\b").sub("", typ)
        if "*" in stripped_ty:
            continue
        names.add(name)
    for m in _CONST_OBJ_DECL.finditer(fn.body or ""):
        names.add(m.group("name"))
    for m in _CONST_LOCAL.finditer(fn.body or ""):
        names.add(m.group("name"))
    return names


def _cxx_move_const(lines, rel, funcs, out) -> None:
    """std::move of a const object / const T. Non-const twin is ok."""
    for fn in funcs:
        consts = _const_object_names(fn)
        if not consts:
            continue
        start = fn.span[0]
        for i, ln in enumerate(fn.body.splitlines()):
            hit = None
            for m in _STD_MOVE.finditer(ln):
                name = m.group("name")
                if name in consts:
                    hit = name
                    break
            if not hit:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-MOVE-CONST",
                message=f"std::move of const object {hit}",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _emit_bind_tmp(lines, rel, out, fn_name: str, line: int, msg: str) -> None:
    out.append(Finding(
        stage="lints", status=laws.FAILED, file=rel,
        function=fn_name, line=line, cls="CXX-BIND-TMP",
        message=msg,
        strength=laws.STRENGTH_FINDS,
        evidence=lines[line - 1].strip()
        if 0 < line <= len(lines) else "",
    ))


def _cxx_bind_tmp(stripped, lines, rel, funcs, out) -> None:
    """Reference bound to a temporary then returned. Distinct from views."""
    reported: set[str] = set()
    for m in _lit_finditer(_REF_RET_FN, stripped):
        name = m.group("name")
        brace = m.end() - 1
        close = _match_brace(stripped, brace)
        if close < 0:
            continue
        body = stripped[brace + 1: close]
        hit_line = None
        msg = None
        rm = _lit_search(_RETURN_CTOR, body)
        if rm:
            hit_line = stripped[: brace + 1 + rm.start()].count("\n") + 1
            msg = "return of a temporary bound to a reference"
        else:
            bm = _lit_search(_BIND_TMP_LOCAL, body)
            if bm:
                nm = bm.group("name")
                if re.search(rf"\breturn\s+{re.escape(nm)}\s*;", body):
                    hit_line = stripped[: brace + 1 + bm.start()].count("\n") + 1
                    msg = f"return of {nm} bound to a temporary"
        if not hit_line or msg is None or name in reported:
            continue
        reported.add(name)
        _emit_bind_tmp(lines, rel, out, name, hit_line, msg)
    for fn in funcs:
        if fn.name in reported:
            continue
        body = fn.body or ""
        bm = _lit_search(_BIND_TMP_LOCAL, body)
        if not bm:
            continue
        nm = bm.group("name")
        if not re.search(rf"\breturn\s+{re.escape(nm)}\s*;", body):
            continue
        line = fn.span[0] + body[: bm.start()].count("\n")
        reported.add(fn.name)
        _emit_bind_tmp(
            lines, rel, out, fn.name, line,
            f"return of {nm} bound to a temporary",
        )


def _cxx_expected_null(lines, rel, funcs, out) -> None:
    """Dereference of std::expected without has_value()/operator bool."""
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_EXPECTED_DECL, body)}
        if not names:
            continue
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            for name in sorted(names):
                if _optional_has_guard(name, body):
                    continue
                if not _optional_deref(name, ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-EXPECTED-NULL",
                    message=f"{name} dereferenced without has_value()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))
                return


def _std_fmt_first_args(ln: str):
    for m in _STD_FMT_CALL.finditer(ln):
        start = m.end()
        depth = 1
        i = start
        while i < len(ln) and depth:
            if ln[i] == "(":
                depth += 1
            elif ln[i] == ")":
                depth -= 1
            i += 1
        if depth != 0:
            continue
        args = _split_call_args(ln[start: i - 1])
        yield m.group("fn"), args


def _cxx_std_format(lines, rel, funcs, out) -> None:
    """std::format / std::print / std::println with a non-literal format."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(fn.body.splitlines()):
            for fname, args in _std_fmt_first_args(ln):
                if not args:
                    continue
                if _is_string_literal(args[0].strip()):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-STD-FORMAT",
                    message=f"std::{fname}() format argument is not a "
                    "string literal",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _lambda_body_from(text: str, capture_end: int) -> str | None:
    rest = text[capture_end:]
    m = _rx(r"\s*(?:\([^;{}]*\))?\s*(?:mutable\s*)?\{").match(rest)
    if not m:
        return None
    brace = capture_end + m.end() - 1
    close = _match_brace(text, brace)
    if close < 0:
        return None
    return text[brace + 1: close]


def _lambda_capture_kind(capture: str) -> str:
    cap = _rx(r"\s+").sub("", capture)
    if cap == "this" or cap.startswith("this,"):
        return "this"
    if cap == "=" or cap.startswith("=,"):
        return "eq"
    return "other"


def _lambda_idents(body: str) -> set[str]:
    names: set[str] = set()
    for m in _rx(r"\b([A-Za-z_]\w*)\b(?!\s*\()").finditer(body):
        ident = m.group(1)
        if ident not in _LAMBDA_SKIP:
            names.add(ident)
    return names


def _fn_class_members(fn, stripped: str) -> set[str] | None:
    """Member names when fn is defined inside a class/struct, else None."""
    for m in _lit_finditer(_CLASS_DEF, stripped):
        brace = m.end() - 1
        close = _match_brace(stripped, brace)
        if close < 0:
            continue
        start_line = stripped[: brace].count("\n") + 1
        end_line = stripped[: close].count("\n") + 1 if close >= 0 else start_line
        if not (start_line <= fn.line <= end_line):
            continue
        body = stripped[brace + 1: close]
        names: set[str] = set()
        for pm in _lit_finditer(_PTR_MEMBER, body):
            names.add(pm.group("name"))
        for sm in _lit_finditer(_CXX_SCALAR_MEMBER, body):
            names.add(sm.group("name"))
        return names
    return None


def _cxx_this_capture_hit(capture: str, lam: str | None, locals_ok: set[str],
                          members: set[str] | None) -> bool:
    kind = _lambda_capture_kind(capture)
    if kind == "this":
        return True
    if kind != "eq" or lam is None:
        return False
    if _rx(r"\bthis\b").search(lam):
        return True
    if members is None:
        return False
    used = _lambda_idents(lam)
    return bool((used - locals_ok) & members)


def _cxx_this_capture(stripped, lines, rel, funcs, out) -> None:
    """Returned lambda with [this] or [=] capturing a class member.

    Distinct from CXX-LAMBDA-DANGLE (`[&]` / explicit `&local`).
    """
    reported: set[str] = set()
    for fn in funcs:
        body = fn.body or ""
        # A hit needs `return [..]` or a stored `auto f = [..]`; without
        # either both loops below find nothing.
        if "[" not in body or (
            not _lit_search(_CXX_RETURN_LAMBDA, body) and not _lit_search(_CXX_AUTO_LAMBDA, body)
        ):
            continue
        params = {name for typ, name in fn.params if name}
        members = _fn_class_members(fn, stripped)
        locals_ok: set[str] = set(params)
        for ln in body.splitlines():
            mloc = _CXX_LOCAL_LINE.match(ln)
            if not mloc:
                continue
            name = mloc.group("name")
            if name not in params and name not in _KW:
                locals_ok.add(name)
        stored: dict[str, tuple[str, str]] = {}
        for m in _lit_finditer(_CXX_AUTO_LAMBDA, body):
            stored[m.group("name")] = (
                m.group("capture"), _lambda_body_from(body, m.end()) or "",
            )
        hit_off = None
        for m in _lit_finditer(_CXX_RETURN_LAMBDA, body):
            lam = _lambda_body_from(body, m.end())
            if _cxx_this_capture_hit(m.group("capture"), lam, locals_ok, members):
                hit_off = m.start()
                break
        if hit_off is None:
            for m in _lit_finditer(_CXX_RETURN_NAME, body):
                stored_cap = stored.get(m.group("name"))
                if not stored_cap:
                    continue
                cap, lam = stored_cap
                if _cxx_this_capture_hit(cap, lam, locals_ok, members):
                    hit_off = m.start()
                    break
        if hit_off is None:
            continue
        line = fn.span[0] + body[: hit_off].count("\n")
        reported.add(fn.name)
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=fn.name, line=line, cls="CXX-THIS-CAPTURE",
            message="returned lambda captures this by [=] or [this]",
            strength=laws.STRENGTH_FINDS,
            evidence=lines[line - 1].strip()
            if 0 < line <= len(lines) else "",
        ))

    for cm in _lit_finditer(_CLASS_DEF, stripped):
        cls = cm.group("name")
        brace = cm.end() - 1
        close = _match_brace(stripped, brace)
        if close < 0:
            continue
        cbody = stripped[brace + 1: close]
        members = {pm.group("name") for pm in _lit_finditer(_PTR_MEMBER, cbody)}
        members.update(sm.group("name") for sm in _lit_finditer(_CXX_SCALAR_MEMBER, cbody))
        for m in _lit_finditer(_CXX_RETURN_LAMBDA, cbody):
            lam = _lambda_body_from(cbody, m.end())
            if not _cxx_this_capture_hit(m.group("capture"), lam, set(), members):
                continue
            hit_fn = cls
            for hm in _rx(r"\b(?P<name>[A-Za-z_]\w*)\s*\([^;{}]*\)[^{]*\{").finditer(cbody[: m.start()]):
                hit_fn = hm.group("name")
            if hit_fn in reported:
                continue
            line = stripped[: brace + 1 + m.start()].count("\n") + 1
            reported.add(hit_fn)
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=hit_fn, line=line, cls="CXX-THIS-CAPTURE",
                message="returned lambda captures this by [=] or [this]",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))


def _cxx_spaceship_default(stripped, lines, rel, funcs, out) -> None:
    """Defaulted operator<=> in a class that has a pointer member."""
    reported: set[str] = set()
    for m in _lit_finditer(_CLASS_DEF, stripped):
        cls = m.group("name")
        brace = m.end() - 1
        close = _match_brace(stripped, brace)
        if close < 0:
            continue
        body = stripped[brace + 1: close]
        if not _lit_search(_PTR_MEMBER, body):
            continue
        sm = _lit_search(_SPACESHIP_DEFAULT, body)
        if not sm:
            continue
        op_line = stripped[: brace + 1 + sm.start()].count("\n") + 1
        hit_fn = None
        hit_line = op_line
        for fn in funcs:
            if re.search(rf"\b{re.escape(cls)}\b", fn.body or ""):
                hit_fn = fn.name
                start = fn.span[0]
                for i, ln in enumerate(fn.body.splitlines()):
                    if re.search(rf"\b{re.escape(cls)}\b", ln):
                        hit_line = start + i
                        break
                break
        if hit_fn is None:
            for fn in funcs:
                if fn.line >= op_line:
                    hit_fn = fn.name
                    break
        if hit_fn is None:
            hit_fn = cls
        if hit_fn in reported:
            continue
        reported.add(hit_fn)
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=hit_fn, line=hit_line, cls="CXX-SPACESHIP-DEFAULT",
            message=f"{cls} defaulted operator<=> shallow-compares a "
            "pointer member",
            strength=laws.STRENGTH_FINDS,
            evidence=lines[hit_line - 1].strip()
            if 0 < hit_line <= len(lines) else "",
        ))


def _cxx_std_async(lines, rel, funcs, out) -> None:
    """Discarded std::async() — not assigned, not .wait/.get (CWE-252)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _ASYNC_DISCARDED)):
            if not _ASYNC_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-STD-ASYNC",
                message="std::async result is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _future_has_value(name: str, body: str) -> bool:
    n = re.escape(name)
    if re.search(rf"\b{n}\s*=(?!=)", body):
        return True
    if re.search(rf"\b{n}\s*\.\s*valid\s*\(", body):
        return True
    return False


def _cxx_future_get(lines, rel, funcs, out) -> None:
    """fut.get() / f.get() on a default-constructed std::future (CWE-394).

    Distinct from CXX-PROMISE (`get_future` then `.get()` without set_value).
    """
    for fn in funcs:
        body = fn.body or ""
        if _rx(r"\bget_future\s*\(").search(body):
            continue
        if _rx(r"\b(?:std\s*::\s*)?promise\s*<").search(body):
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_FUTURE_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for i, ln in enumerate(fn.body.splitlines()):
            for m in _FUTURE_GET.finditer(ln):
                name = m.group("name")
                if name not in names:
                    continue
                if _future_has_value(name, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-FUTURE-GET",
                    message=f"{name}.get() without a prior future assignment",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _function_invoked(name: str, ln: str) -> bool:
    if _CXX_FUNCTION_DECL.search(ln):
        return False
    n = re.escape(name)
    return bool(re.search(rf"\b{n}\s*\(", ln))


def _cxx_function_null(lines, rel, funcs, out) -> None:
    """std::function invoked without if (f) / if (fn) (CWE-476)."""
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_FUNCTION_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for i, ln in enumerate(fn.body.splitlines()):
            for name in sorted(names):
                if _optional_has_guard(name, body):
                    continue
                if not _function_invoked(name, ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-STD-FUNCTION-NULL",
                    message=f"{name} invoked without a truth test",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _nodiscard_callees(lines) -> set[str]:
    blob = "\n".join(lines)
    names: set[str] = set()
    for m in _NODISCARD_ATTR.finditer(blob):
        after = blob[m.end(): m.end() + 160]
        for nm in _NODISCARD_NAME.finditer(after):
            ident = nm.group(1)
            if ident in _NODISCARD_SKIP:
                continue
            names.add(ident)
            break
    return names


def _cxx_nodiscard(lines, rel, funcs, out) -> None:
    """Call to a nearby [[nodiscard]] function whose result is discarded."""
    callees = _nodiscard_callees(lines)
    if not callees:
        return
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _NODISCARD_DISCARDED)):
            m = _NODISCARD_DISCARDED.match(ln)
            if not m:
                continue
            name = m.group("name")
            if name not in callees:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-NODISCARD",
                message=f"[[nodiscard]] result of {name}() is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_std_jthread(lines, rel, funcs, out) -> None:
    """std::jthread constructed with a callable and no request_stop."""
    for fn in funcs:
        body = fn.body or ""
        if _rx(r"\brequest_stop\s*\(").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(_gated_lines(fn.body, _CXX_JTHREAD_DECL)):
            m = _CXX_JTHREAD_DECL.search(ln)
            if not m:
                continue
            name = m.group("name")
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-STD-JTHREAD",
                message=f"{name} is constructed without request_stop()",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_mdspan_dangle(lines, rel, funcs, out) -> None:
    """return mdspan<T>(local) of a local array. Distinct from CXX-SPAN-DANGLE."""
    for fn in funcs:
        ret = fn.return_type or ""
        _, arrays = _locals_in_fn(fn)
        locals_ok = set(arrays) | _cxx_local_strings(fn)
        if not locals_ok:
            continue
        start = fn.span[0]
        for i, ln in enumerate(fn.body.splitlines()):
            if "mdspan" not in ln and "mdspan" not in ret:
                continue
            m = _CXX_RETURN_MDSPAN.search(ln)
            if not m:
                continue
            hit = m.group("name")
            if hit not in locals_ok:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-MDSPAN-DANGLE",
                message=f"return mdspan constructed from local {hit}",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_atomic_ref_sources(fn) -> set[str]:
    params = {name for typ, name in fn.params if name}
    scalars, arrays = _locals_in_fn(fn)
    names = set(scalars) | set(arrays)
    for m in _CXX_SCALAR_LOCAL.finditer(fn.body or ""):
        n = m.group("name")
        if n and n not in params:
            names.add(n)
    return names - params


def _cxx_atomic_ref(lines, rel, funcs, out) -> None:
    """atomic_ref of a local then returned. Ok twin takes atomic_ref as a param."""
    for fn in funcs:
        locals_ok = _cxx_atomic_ref_sources(fn)
        body_lines = (fn.body or "").splitlines()
        start = fn.span[0]
        ref_from_local: dict[str, str] = {}
        for i, ln in enumerate(body_lines):
            for m in _CXX_ATOMIC_REF_CTOR.finditer(ln):
                src = m.group("name")
                if src in locals_ok:
                    ref_from_local[m.group("ref")] = src
            sm = _CXX_RETURN_ATOMIC_REF.search(ln)
            if sm and sm.group("name") in locals_ok:
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-ATOMIC-REF",
                    message="return atomic_ref constructed from local "
                    f"{sm.group('name')}",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            sm = _CXX_RETURN_NAME.search(ln)
            if sm and sm.group("name") in ref_from_local:
                src = ref_from_local[sm.group("name")]
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-ATOMIC-REF",
                    message=f"return atomic_ref of local {src}",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_condition_wait(lines, rel, funcs, out) -> None:
    """cv.wait(lk) without a predicate. Ok twin uses wait(lk, pred)."""
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"\b(?:std\s*::\s*)?condition_variable(?:_any)?\b").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_CV_WAIT_BARE.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-CONDITION-WAIT",
                message="cv.wait(lk) without a predicate lambda",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_std_bind(lines, rel, funcs, out) -> None:
    """std::bind(; prefer a lambda. POSIX bind() is not this class."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate((fn.body or "").splitlines()):
            if not _STD_BIND_CALL.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-STD-BIND",
                message="std::bind(); prefer a lambda",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            break


def _cxx_assume(lines, rel, funcs, out) -> None:
    """[[assume(false)]] / [[assume(0)]] in a function that still returns."""
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"\breturn\b").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_ASSUME_FALSE.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-ASSUME",
                message="[[assume(false)]] / [[assume(0)]] is an unreachable lie",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_generator(lines, rel, funcs, out) -> None:
    """generator that yields a local array / generator<T&>. Ok twin yields a value."""
    for fn in funcs:
        ret = fn.return_type or ""
        body = fn.body or ""
        blob = f"{ret}\n{body}"
        if not _rx(r"\b(?:std\s*::\s*)?generator\b").search(blob):
            continue
        ref_yield = bool(
            _rx(r"\b(?:std\s*::\s*)?generator\s*<[^>]*&").search(blob)
        )
        _, arrays = _locals_in_fn(fn)
        local_yield = bool(arrays) and bool(_rx(r"\bco_yield\b").search(body))
        if not ref_yield and not local_yield:
            continue
        start = fn.span[0]
        hit_i = 0
        for i, ln in enumerate(body.splitlines()):
            if _rx(r"\bco_yield\b").search(ln) or _rx(r"\b(?:std\s*::\s*)?generator\s*<[^>]*&").search(ln):
                hit_i = i
                break
        line = start + hit_i
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=fn.name, line=line, cls="CXX-GENERATOR",
            message="std::generator yields a pointer/reference to a local",
            strength=laws.STRENGTH_FINDS,
            evidence=lines[line - 1].strip()
            if 0 < line <= len(lines) else "",
        ))


def _cxx_shared_mutex(lines, rel, funcs, out) -> None:
    """shared_mutex.lock() without unlock or lock_guard/shared_lock."""
    for fn in funcs:
        body = fn.body or ""
        if _rx(r"\b(?:lock_guard|shared_lock|unique_lock|scoped_lock)\b").search(body):
            continue
        names = {
            m.group("name") for m in _lit_finditer(_CXX_SHARED_MUTEX_DECL, body)
        }
        start = fn.span[0]
        body_lines = body.splitlines()
        for name in sorted(names):
            n = re.escape(name)
            if re.search(rf"\b{n}\s*\.\s*unlock\s*\(", body):
                continue
            for i, ln in enumerate(body_lines):
                if not re.search(rf"\b{n}\s*\.\s*lock\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-SHARED-MUTEX",
                    message=f"{name}.lock() without unlock or lock_guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_any_cast(lines, rel, funcs, out) -> None:
    """any_cast<T>(v) by value without type()/has_value/try or pointer form."""
    for fn in funcs:
        body = fn.body or ""
        if _rx(r"\btry\b").search(body) and _rx(r"\bcatch\b").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if _rx(r"\(\s*void\s*\)\s*(?:std\s*::\s*)?any_cast\s*<").search(ln):
                continue
            for m in _ANY_CAST_CALL.finditer(ln):
                arg = m.group("arg").strip()
                if arg.startswith("&"):
                    continue
                var_m = _rx(r"^([A-Za-z_]\w*)\s*$").match(arg)
                if var_m:
                    v = re.escape(var_m.group(1))
                    if re.search(rf"\b{v}\s*\.\s*(?:type|has_value)\s*\(", body):
                        continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-ANY-CAST",
                    message="any_cast by value without type() or pointer form",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_filesystem(lines, rel, funcs, out) -> None:
    """filesystem::remove(p) then exists/file_size(p) on the same path."""
    for fn in funcs:
        body_lines = (fn.body or "").splitlines()
        start = fn.span[0]
        removed: dict[str, int] = {}
        for i, ln in enumerate(body_lines):
            for m in _FS_REMOVE.finditer(ln):
                removed[m.group("p")] = i
            for m in _FS_USE_AFTER.finditer(ln):
                p = m.group("p")
                prev = removed.get(p)
                if prev is None or i <= prev:
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-FILESYSTEM",
                    message=f"filesystem::remove({p}) then use of the same path",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_regex(lines, rel, funcs, out) -> None:
    """std::regex constructed from a non-literal pattern variable."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate((fn.body or "").splitlines()):
            m = _REGEX_CTOR.search(ln)
            if not m:
                continue
            pat = m.group("pat").strip()
            if _is_string_literal(pat):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-REGEX",
                message="std::regex pattern is not a string literal",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_latch(lines, rel, funcs, out) -> None:
    """latch.wait() without count_down in the same function."""
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_LATCH_DECL, body)}
        start = fn.span[0]
        body_lines = body.splitlines()
        for name in sorted(names):
            n = re.escape(name)
            if re.search(rf"\b{n}\s*\.\s*count_down\s*\(", body):
                continue
            for i, ln in enumerate(body_lines):
                if not re.search(rf"\b{n}\s*\.\s*wait\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-LATCH",
                    message=f"{name}.wait() without count_down()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_from_chars(lines, rel, funcs, out) -> None:
    """from_chars out-parameter used without checking r.ec / errc / ptr."""
    for fn in funcs:
        body = fn.body or ""
        if _rx(r"\.(?:ec|ptr)\b|\berrc\b").search(body):
            continue
        start = fn.span[0]
        outs: dict[str, int] = {}
        body_lines = body.splitlines()
        for i, ln in enumerate(body_lines):
            for m in _FROM_CHARS_OUT.finditer(ln):
                outs[m.group("var")] = i
        if not outs:
            continue
        for i, ln in enumerate(body_lines):
            for var, prev in outs.items():
                if i <= prev:
                    continue
                v = re.escape(var)
                if not re.search(rf"\b{v}\b", ln):
                    continue
                if _FROM_CHARS_OUT.search(ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-FROM-CHARS",
                    message=f"{var} used after from_chars without checking ec",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_init_list_dangle(lines, rel, funcs, out) -> None:
    """pointer from a temporary initializer_list{...}.begin() (CWE-562)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate((fn.body or "").splitlines()):
            if not _INIT_LIST_TMP_BEGIN.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-INIT-LIST-DANGLE",
                message="pointer from a temporary initializer_list.begin()",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_stop_token(lines, rel, funcs, out) -> None:
    """stop_token in while(true)/for(;;) without stop_requested() (CWE-833)."""
    for fn in funcs:
        blob = f"{fn.signature or ''}\n{fn.body or ''}"
        if not _CXX_STOP_TOKEN.search(blob):
            continue
        body = fn.body or ""
        if _rx(r"\bstop_requested\s*\(").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_INF_LOOP.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-STOP-TOKEN",
                message="stop_token used in an infinite loop without "
                "stop_requested()",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_flat_map(lines, rel, funcs, out) -> None:
    """flat_map.at() without contains/count/find in the same function (CWE-125).

    `std::map` `.at(` is only this class when the type or name includes
    `flat_map`, so ordinary map plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        names: set[str] = set()
        for m in _lit_finditer(_CXX_FLAT_MAP_DECL, body):
            name = m.group("name")
            if m.group("map") is not None:
                if "flat_map" not in m.group(0) and "flat_map" not in name:
                    continue
            names.add(name)
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            n = re.escape(name)
            if re.search(
                rf"\b{n}\s*\.\s*(?:contains|count|find)\s*\(", body
            ):
                continue
            for i, ln in enumerate(body.splitlines()):
                if not re.search(rf"\b{n}\s*\.\s*at\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-FLAT-MAP",
                    message=f"{name}.at() without contains()/count()/find()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_semaphore(lines, rel, funcs, out) -> None:
    """counting_semaphore.acquire() without release in the same function."""
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_SEM_DECL, body)}
        start = fn.span[0]
        body_lines = body.splitlines()
        for name in sorted(names):
            n = re.escape(name)
            if re.search(rf"\b{n}\s*\.\s*release\s*\(", body):
                continue
            for i, ln in enumerate(body_lines):
                if not re.search(rf"\b{n}\s*\.\s*acquire\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-SEMAPHORE",
                    message=f"{name}.acquire() without release()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _stacktrace_has_guard(name: str, body: str) -> bool:
    n = re.escape(name)
    if re.search(rf"\b{n}\s*\.\s*(?:empty|size)\s*\(", body):
        return True
    return bool(re.search(rf"\bif\s*\(\s*!\s*{n}\b", body))


def _cxx_stacktrace(lines, rel, funcs, out) -> None:
    """stacktrace::current() [0]/at(0) without empty/size check (CWE-125)."""
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_STACKTRACE_CURRENT, body):
            continue
        start = fn.span[0]
        assigned = {
            m.group("name") for m in _lit_finditer(_STACKTRACE_ASSIGN, body)
        }
        for i, ln in enumerate(body.splitlines()):
            if _STACKTRACE_DIRECT_ZERO.search(ln):
                if _rx(r"\.(?:empty|size)\s*\(").search(body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-STACKTRACE",
                    message="stacktrace::current()[0] without empty()/size()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for name in sorted(assigned):
                if _stacktrace_has_guard(name, body):
                    continue
                n = re.escape(name)
                if not (
                    re.search(rf"\b{n}\s*\[\s*0\s*\]", ln)
                    or re.search(rf"\b{n}\s*\.\s*at\s*\(\s*0\s*\)", ln)
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-STACKTRACE",
                    message=f"{name}[0] from stacktrace::current() without "
                    "empty()/size()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_unique_release(lines, rel, funcs, out) -> None:
    """unique_ptr.get() after release(); reset() is CXX-UNIQUE-RESET."""
    for fn in funcs:
        body_lines = (fn.body or "").splitlines()
        start = fn.span[0]
        unique_ptrs: set[str] = set()
        released_at: dict[str, int] = {}
        for i, ln in enumerate(body_lines):
            for m in _UNIQUE_PTR_DECL.finditer(ln):
                unique_ptrs.add(m.group("name"))
            for m in _UNIQUE_RELEASE.finditer(ln):
                up = m.group("up")
                if up in unique_ptrs:
                    released_at.setdefault(up, i)
            for up, rline in list(released_at.items()):
                if i <= rline:
                    continue
                if not re.search(rf"\b{re.escape(up)}\s*\.\s*get\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-UNIQUE-RELEASE",
                    message=f"{up}.get() used after release()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_pack_pragma(lines, rel, funcs, out) -> None:
    """#pragma pack(1) plus address of a non-char packed-struct member."""
    file_text = "\n".join(lines)
    if not _PACK_PRAGMA_1.search(file_text):
        return
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate((fn.body or "").splitlines()):
            if _rx(r"\bmemcpy\s*\(").search(ln):
                continue
            if not _PACK_INT_MEMBER_ADDR.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-PACK-PRAGMA",
                message="address of a non-char member of a #pragma pack(1) "
                "struct",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_function_ref(lines, rel, funcs, out) -> None:
    """function_ref bound to a temporary lambda/call then used (CWE-416/562).

    A named function or named lvalue lambda is the ok twin. Distinct from
    CXX-LAMBDA-DANGLE (returned `[&]` lambda) and CXX-STD-FUNCTION-NULL.
    """
    for fn in funcs:
        body = fn.body or ""
        start = fn.span[0]
        if _lit_search(_CXX_FN_REF_RETURN, body):
            for i, ln in enumerate(body.splitlines()):
                if not _CXX_FN_REF_RETURN.search(ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-FUNCTION-REF",
                    message="function_ref bound to a temporary then returned",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_FN_REF_TMP.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-FUNCTION-REF",
                message="function_ref bound to a temporary lambda or call",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _mof_invoked(name: str, ln: str) -> bool:
    if _CXX_MOF_DECL.search(ln):
        return False
    n = re.escape(name)
    return bool(re.search(rf"\b{n}\s*\(", ln))


def _cxx_move_only_function(lines, rel, funcs, out) -> None:
    """move_only_function invoked without if (f) (CWE-476).

    Distinct from CXX-STD-FUNCTION-NULL (`std::function`).
    """
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_MOF_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for name in sorted(names):
                if _optional_has_guard(name, body):
                    continue
                if not _mof_invoked(name, ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-MOVE-ONLY-FUNCTION",
                    message=f"{name} invoked without a truth test",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_ranges_dangle(lines, rel, funcs, out) -> None:
    """views pipeline whose first range is a temporary string/vector (CWE-416).

    Prefer this class over CXX-SPAN-DANGLE / CXX-DANGLING-REF: the plant
    returns an element, not a span or string_view of a local.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_RANGES_TMP_PIPE, body):
            continue
        if _lit_search(_RANGES_MATERIALIZE, body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not (
                _RANGES_TMP_PIPE.search(ln)
                or _rx(r"\|\s*(?:std\s*::\s*)?(?:ranges\s*::\s*)?views\s*::").search(ln)
            ):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-RANGES-DANGLE",
                message="views pipeline over a temporary range",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_chrono_seed(lines, rel, funcs, out) -> None:
    """chrono clock::now() used to seed srand/mt19937 (CWE-330).

    Prefer mt19937 so CRYPTO-SRAND (`srand(time())`) stays silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CHRONO_NOW, body):
            continue
        if not _lit_search(_CHRONO_PRNG, body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not (_CHRONO_NOW.search(ln) or _CHRONO_PRNG.search(ln)):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-CHRONO-SEED",
                message="PRNG seeded from chrono clock::now()",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _inplace_has_guard(name: str, body: str) -> bool:
    n = re.escape(name)
    return bool(re.search(
        rf"\b{n}\s*\.\s*(?:size|capacity|try_push_back)\s*\(",
        body,
    ))


def _cxx_inplace_vector(lines, rel, funcs, out) -> None:
    """inplace_vector push_back/emplace_back without a capacity guard (CWE-787)."""
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_INPLACE_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            if _inplace_has_guard(name, body):
                continue
            n = re.escape(name)
            for i, ln in enumerate(body.splitlines()):
                if not re.search(
                    rf"\b{n}\s*\.\s*(?:push_back|emplace_back)\s*\(", ln
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-INPLACE-VECTOR",
                    message=f"{name}.push_back without size()/capacity()/"
                    "try_push_back()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _flat_set_has_end_guard(name: str, body: str) -> bool:
    n = re.escape(name)
    return bool(re.search(rf"\b{n}\s*\.\s*end\s*\(", body))


def _cxx_flat_set(lines, rel, funcs, out) -> None:
    """flat_set find() dereference without != end() (CWE-476).

    Distinct from CXX-FLAT-MAP (that is `.at` without contains).
    """
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_FLAT_SET_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            if _flat_set_has_end_guard(name, body):
                continue
            n = re.escape(name)
            for i, ln in enumerate(body.splitlines()):
                if not (
                    re.search(rf"\*\s*{n}\s*\.\s*find\s*\(", ln)
                    or re.search(rf"\b{n}\s*\.\s*find\s*\(", ln)
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-FLAT-SET",
                    message=f"{name}.find() dereferenced without != end()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _copyable_fn_invoked(name: str, ln: str) -> bool:
    if _CXX_COPYABLE_FN_DECL.search(ln):
        return False
    n = re.escape(name)
    return bool(re.search(rf"\b{n}\s*\(", ln))


def _cxx_copyable_function(lines, rel, funcs, out) -> None:
    """copyable_function invoked without if (f) (CWE-476).

    Distinct from CXX-STD-FUNCTION-NULL (`std::function`) and
    CXX-MOVE-ONLY-FUNCTION (`move_only_function`).
    """
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_COPYABLE_FN_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for name in sorted(names):
                if _optional_has_guard(name, body):
                    continue
                if not _copyable_fn_invoked(name, ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-COPYABLE-FUNCTION",
                    message=f"{name} invoked without a truth test",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_hive(lines, rel, funcs, out) -> None:
    """hive iterator used after insert/emplace/erase in the same function.

    Distinct from CXX-ITERATOR-INVALID (vector/string/map / Vec plants).
    """
    for fn in funcs:
        hives = _hive_container_names(fn)
        if not hives:
            continue
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        begun: set[str] = set()
        for i, ln in enumerate(body_lines):
            if "=" in ln and ".begin(" in ln:
                for m in _CXX_BEGIN_CALL.finditer(ln):
                    cont = _norm_var(m.group("container"))
                    if cont in hives:
                        begun.add(cont)
            for m in _CXX_HIVE_MUTATE.finditer(ln):
                cont = _norm_var(m.group("container"))
                if cont not in hives or cont not in begun:
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-HIVE",
                    message=f"{cont} mutated after .begin() iterator taken",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _bitset_index_guarded(idx: str, nlit: str, name: str, body: str) -> bool:
    i, lit, b = re.escape(idx.strip()), re.escape(nlit.strip()), re.escape(name)
    if re.search(rf"\bif\s*\(\s*{i}\s*<\s*{lit}\b", body):
        return True
    if re.search(rf"\bif\s*\(\s*{i}\s*>=\s*{lit}\b", body):
        return True
    if re.search(rf"\bif\s*\(\s*{i}\s*<\s*{b}\s*\.\s*size\s*\(\s*\)", body):
        return True
    if re.search(rf"\bif\s*\(\s*{i}\s*>=\s*{b}\s*\.\s*size\s*\(\s*\)", body):
        return True
    if re.search(
        rf"\bif\s*\(\s*{b}\s*\.\s*size\s*\(\s*\)\s*>\s*{i}",
        body,
    ):
        return True
    return False


def _cxx_bitset_index(lines, rel, funcs, out) -> None:
    """bitset .test(n) / [n] on a parameter without n < size()/N (CWE-125)."""
    for fn in funcs:
        decls = list(_CXX_BITSET_DECL.finditer(fn.body or ""))
        if not decls:
            continue
        params = {name for _, name in fn.params if name}
        body = fn.body or ""
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            hits = list(_CXX_BITSET_TEST.finditer(ln))
            for m in _CXX_SUBSCRIPT.finditer(ln):
                hits.append(m)
            for m in hits:
                name = m.group("name") if "name" in m.re.groupindex else m.group("cont")
                idx = m.group("idx")
                decl = next((d for d in decls if d.group("name") == name), None)
                if decl is None:
                    continue
                if idx.strip() not in params:
                    continue
                if _bitset_index_guarded(idx, decl.group("n"), name, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-BITSET-INDEX",
                    message=f"{name}.test/{name}[{idx.strip()}] without a "
                    "size()/N guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_sstream_view(lines, rel, funcs, out) -> None:
    """string_view / c_str of stringstream::str() after the full-expression.

    Distinct from CXX-STRING-DATA (mutation after c_str on a string).
    """
    for fn in funcs:
        body = fn.body or ""
        streams = {m.group("name") for m in _lit_finditer(_CXX_SSTREAM_DECL, body)}
        if not streams:
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            view = _CXX_SSTREAM_VIEW.search(ln)
            if view and view.group("ss") in streams:
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-SSTREAM-VIEW",
                    message=f"string_view of {view.group('ss')}.str() "
                    "temporary",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            cstr = _CXX_SSTREAM_CSTR_TMP.search(ln)
            if cstr and cstr.group("ss") in streams:
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-SSTREAM-VIEW",
                    message=f"{cstr.group('ss')}.str().c_str() of a temporary",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_optional_value(lines, rel, funcs, out) -> None:
    """optional.value() without has_value()/if (o) (CWE-476).

    Distinct from CXX-OPTIONAL-NULL (`operator*` / `operator->`).
    """
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_OPTIONAL_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for name in sorted(names):
                if _optional_has_guard(name, body):
                    continue
                n = re.escape(name)
                if not re.search(rf"\b{n}\s*\.\s*value\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-OPTIONAL-VALUE",
                    message=f"{name}.value() without has_value()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _indirect_has_value(name: str, body: str) -> bool:
    n = re.escape(name)
    if re.search(
        rf"(?:std\s*::\s*)?indirect\s*<[^>]+>\s+{n}\s*[\({{]\s*\S",
        body,
    ):
        return True
    if re.search(rf"\b{n}\s*=", body):
        return True
    return False


def _cxx_indirect(lines, rel, funcs, out) -> None:
    """Dereference of a default-constructed empty std::indirect (CWE-476).

    Avoids std::move so CXX-USE-AFTER-MOVE stays silent on these plants.
    """
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_INDIRECT_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for name in sorted(names):
                if _indirect_has_value(name, body):
                    continue
                n = re.escape(name)
                if not (
                    re.search(rf"(?<!\w)\*\s*{n}\b", ln)
                    or re.search(rf"\b{n}\s*->", ln)
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-INDIRECT",
                    message=f"{name} dereferenced while empty",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_to_chars(lines, rel, funcs, out) -> None:
    """to_chars buffer used without checking r.ec / errc / ptr (CWE-252).

    Distinct from CXX-FROM-CHARS (`from_chars` out-parameter).
    """
    for fn in funcs:
        body = fn.body or ""
        if _rx(r"\.(?:ec|ptr)\b|\berrc\b").search(body):
            continue
        start = fn.span[0]
        outs: dict[str, int] = {}
        body_lines = body.splitlines()
        for i, ln in enumerate(body_lines):
            for m in _TO_CHARS_BUF.finditer(ln):
                outs[m.group("buf")] = i
        if not outs:
            continue
        for i, ln in enumerate(body_lines):
            for var, prev in outs.items():
                if i <= prev:
                    continue
                v = re.escape(var)
                if not re.search(rf"\b{v}\b", ln):
                    continue
                if _TO_CHARS_BUF.search(ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-TO-CHARS",
                    message=f"{var} used after to_chars without checking ec",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_hazard_pointer(lines, rel, funcs, out) -> None:
    """hazard_pointer in scope but atomic load dereferenced without protect.

    Distinct from unused-decl honesty plants (no .load() / no deref).
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_HAZARD_DECL, body):
            continue
        if _rx(r"\bprotect\s*\(|\bhazard_pointer_for\s*\(").search(body):
            continue
        if not _rx(r"\.load\s*\(").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _rx(r"->").search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-HAZARD-POINTER",
                message="atomic load used without hazard_pointer::protect()",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_text_encoding(lines, rel, funcs, out) -> None:
    """text_encoding constructed from a non-literal name (CWE-176)."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate((fn.body or "").splitlines()):
            m = _TEXT_ENC_CTOR.search(ln)
            if not m:
                continue
            arg = m.group("arg").strip()
            if _is_string_literal(arg):
                continue
            if _rx(r"\butf8\s*\(").search(arg):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-TEXT-ENCODING",
                message="std::text_encoding name is not a string literal",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _expected_error_guarded(name: str, body: str) -> bool:
    n = re.escape(name)
    if re.search(rf"\bif\s*\(\s*!\s*{n}\s*\)", body):
        return True
    if re.search(rf"\bif\s*\(\s*!\s*{n}\s*\.\s*has_value\s*\(", body):
        return True
    if re.search(rf"{n}\s*\.\s*has_value\s*\(\s*\)\s*==\s*false", body):
        return True
    return False


def _cxx_expected_error(lines, rel, funcs, out) -> None:
    """expected.error() without !e / !has_value / has_value()==false (CWE-390).

    Distinct from CXX-EXPECTED-NULL (`operator*` / `operator->`).
    """
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_EXPECTED_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _EXPECTED_ERROR_CALL.finditer(ln):
                name = m.group("name")
                if name not in names:
                    continue
                if _expected_error_guarded(name, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-EXPECTED-ERROR",
                    message=f"{name}.error() without a prior !{name} check",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_variant_valueless(lines, rel, funcs, out) -> None:
    """std::get / get_if / .index() after emplace without valueless check.

    Distinct from CXX-VARIANT-GET (get without holds_alternative).
    """
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"\bemplace\s*<").search(body):
            continue
        if _rx(r"\bvalueless_by_exception\s*\(").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not (
                _rx(r"(?:std\s*::\s*)?\bget(?:_if)?\s*<").search(ln)
                or _rx(r"\.\s*index\s*\(").search(ln)
            ):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-VARIANT-VALUELSS",
                message="std::get after emplace without "
                "valueless_by_exception()",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _simd_index_guarded(name: str, idx: str, body: str) -> bool:
    n, i = re.escape(name), re.escape(idx.strip())
    if re.search(rf"\bif\s*\(\s*{i}\s*<\s*{n}\s*\.\s*size\s*\(", body):
        return True
    if re.search(rf"\bif\s*\(\s*{n}\s*\.\s*size\s*\(\s*\)\s*>\s*{i}", body):
        return True
    return False


def _cxx_simd_index(lines, rel, funcs, out) -> None:
    """simd[i] without if (i < x.size()) in the same function (CWE-125)."""
    for fn in funcs:
        names = {m.group("name") for m in _CXX_SIMD_DECL.finditer(fn.body or "")}
        if not names:
            continue
        body = fn.body or ""
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                cont = m.group("cont")
                if cont not in names:
                    continue
                idx = m.group("idx")
                if _simd_index_guarded(cont, idx, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-SIMD-INDEX",
                    message=f"{cont}[{idx.strip()}] without a size() guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_rcu(lines, rel, funcs, out) -> None:
    """rcu_obj update / retire without rcu_synchronize() (CWE-416)."""
    for fn in funcs:
        body = fn.body or ""
        if _rx(r"\brcu_synchronize\s*\(").search(body):
            continue
        names = {m.group("name") for m in _lit_finditer(_RCU_OBJ_DECL, body)}
        has_ctx = bool(names) or bool(_rx(r"\bstd\s*::\s*rcu\b").search(body))
        if not has_ctx:
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            hit = bool(_rx(r"\bretire\s*\(").search(ln))
            for name in sorted(names):
                if not re.search(rf"\b{re.escape(name)}\s*=(?!=)", ln):
                    continue
                if _RCU_OBJ_DECL.search(ln) and "=" not in ln.split("=", 1)[0]:
                    # initializer on the decl still counts as an update
                    pass
                hit = True
                break
            if not hit:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-RCU",
                message="rcu_obj update/retire without rcu_synchronize()",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _linalg_extents_guarded(body: str) -> bool:
    return bool(_rx(r"\b(?:extents|extent|size)\s*\(").search(body))


def _cxx_linalg(lines, rel, funcs, out) -> None:
    """linalg matrix(i,j) / scaled return without extents (CWE-125).

    Distinct from CXX-MDSPAN-DANGLE and CXX-SIMD-INDEX.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"(?:std\s*::\s*)?linalg\s*::").search(body):
            continue
        if _linalg_extents_guarded(body):
            continue
        names = {m.group("name") for m in _lit_finditer(_LINALG_MATRIX_DECL, body)}
        start = fn.span[0]
        body_lines = body.splitlines()
        if _lit_search(_LINALG_SCALED, body) and _rx(r"\breturn\b").search(body):
            hit_i = 0
            for i, ln in enumerate(body_lines):
                if _LINALG_SCALED.search(ln) or _rx(r"\breturn\b").search(ln):
                    hit_i = i
                    break
            line = start + hit_i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-LINALG",
                message="linalg scaled/copied result used without extents",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))
            return
        for i, ln in enumerate(body_lines):
            for name in sorted(names):
                if not re.search(
                    rf"\b{re.escape(name)}\s*\(\s*[^,\)]+\s*,", ln
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-LINALG",
                    message=f"{name}(i, j) without an extents guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_sync_wait(lines, rel, funcs, out) -> None:
    """Discarded sync_wait() — not assigned (CWE-252). Distinct from CXX-STD-ASYNC."""
    for fn in funcs:
        start = fn.span[0]
        for i, ln in enumerate((fn.body or "").splitlines()):
            if not _SYNC_WAIT_DISCARDED.match(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-SYNC-WAIT",
                message="std::execution::sync_wait result is discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _embed_arg_is_literal(arg: str) -> bool:
    rest = _rx(r"//.*").sub("", arg or "").strip().rstrip(";").strip()
    if not rest:
        return False
    return bool(_rx(r'["<]').match(rest))


def _cxx_embed(lines, rel, funcs, out) -> None:
    """#embed of a non-literal token, or pointer into embed storage without sizeof."""
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"#\s*embed\b").search(body):
            continue
        start = fn.span[0]
        body_lines = body.splitlines()
        for i, ln in enumerate(body_lines):
            m = _EMBED_DIR.search(ln)
            if not m:
                continue
            if _embed_arg_is_literal(m.group("arg") or ""):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-EMBED",
                message="#embed path is not a string literal",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return
        if _rx(r"\bsizeof\s*\(").search(body):
            continue
        ret = fn.return_type or ""
        if "*" not in ret and not _rx(r"\bchar\s*\*").search(body):
            continue
        for i, ln in enumerate(body_lines):
            if not _rx(r"\breturn\s+[A-Za-z_]\w*\s*;").search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-EMBED",
                message="#embed storage returned as a pointer without a size",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_contracts(lines, rel, funcs, out) -> None:
    """contract_assert(false)/pre(false) in a function that still returns.

    Distinct from CXX-ASSUME (`[[assume(false)]]`).
    """
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"\breturn\b").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_CONTRACT_FALSE.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-CONTRACTS",
                message="contract_assert(false)/pre(false) is an unreachable lie",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_reflection(lines, rel, funcs, out) -> None:
    """^^ / std::meta:: with define_aggregate/define_class and no namespace."""
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_REFLECT_META, body):
            continue
        if not _lit_search(_CXX_REFLECT_DEFINE, body):
            continue
        if _rx(r"\bnamespace\s+[A-Za-z_]").search(body):
            continue
        start = fn.span[0]
        hit_i = 0
        for i, ln in enumerate(body.splitlines()):
            if _CXX_REFLECT_DEFINE.search(ln) or _CXX_REFLECT_META.search(ln):
                hit_i = i
                break
        line = start + hit_i
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=fn.name, line=line, cls="CXX-REFLECTION",
            message="define_aggregate/define_class without a named namespace",
            strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
            if 0 < line <= len(lines) else "",
        ))


def _cxx_out_ptr(lines, rel, funcs, out) -> None:
    """out_ptr/inout_ptr fill then unique_ptr/shared_ptr used without a null check.

    Distinct from CXX-UNIQUE-RESET (that is .get() after reset()).
    """
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"\b(?:inout_ptr|out_ptr)\s*(?:<|\()").search(body):
            continue
        names = {m.group("name") for m in _lit_finditer(_UNIQUE_PTR_DECL, body)}
        names |= {m.group("name") for m in _lit_finditer(_SHARED_PTR_DECL, body)}
        if not names:
            continue
        filled: dict[str, int] = {}
        body_lines = body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            for m in _OUT_PTR_CALL.finditer(ln):
                filled[m.group("ptr")] = i
        if not filled:
            continue
        for i, ln in enumerate(body_lines):
            for name, prev in filled.items():
                if i <= prev or name not in names:
                    continue
                if _optional_has_guard(name, body):
                    continue
                n = re.escape(name)
                if not (
                    re.search(rf"(?<!\w)\*\s*{n}\b", ln)
                    or re.search(rf"\b{n}\s*->", ln)
                    or re.search(rf"\b{n}\s*\.\s*get\s*\(", ln)
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-OUT-PTR",
                    message=f"{name} used after out_ptr without a null check",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_flat_multimap(lines, rel, funcs, out) -> None:
    """flat_multimap find() dereference without != end() (CWE-476).

    Distinct from CXX-FLAT-MAP (.at without contains) and CXX-FLAT-SET.
    """
    for fn in funcs:
        body = fn.body or ""
        names = {
            m.group("name") for m in _lit_finditer(_CXX_FLAT_MULTIMAP_DECL, body)
        }
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            if _flat_set_has_end_guard(name, body):
                continue
            n = re.escape(name)
            for i, ln in enumerate(body.splitlines()):
                if not (
                    re.search(rf"\*\s*{n}\s*\.\s*find\s*\(", ln)
                    or re.search(rf"\b{n}\s*\.\s*find\s*\(", ln)
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-FLAT-MULTIMAP",
                    message=f"{name}.find() dereferenced without != end()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_spanstream(lines, rel, funcs, out) -> None:
    """string_view / .span().data() of a local spanstream (CWE-416).

    Distinct from CXX-SSTREAM-VIEW (that is stringstream::str()).
    """
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_SPANSTREAM_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for name in sorted(names):
                n = re.escape(name)
                if re.search(
                    rf"\breturn\s+{n}\s*\.\s*span\s*\(\s*\)\s*\.\s*data\s*\(",
                    ln,
                ) or re.search(
                    rf"(?:std\s*::\s*)?string_view\b[^;]*"
                    rf"{n}\s*\.\s*span\s*\(",
                    ln,
                ):
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="CXX-SPANSTREAM",
                        message=f"{name}.span() view/data outlives the stream",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else ln.strip(),
                    ))
                    return


def _cxx_barrier(lines, rel, funcs, out) -> None:
    """barrier.arrive() without wait / arrive_and_wait in the same function.

    Distinct from CXX-LATCH (wait without count_down) and CXX-SEMAPHORE.
    """
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_BARRIER_DECL, body)}
        start = fn.span[0]
        body_lines = body.splitlines()
        for name in sorted(names):
            n = re.escape(name)
            if re.search(rf"\b{n}\s*\.\s*arrive_and_wait\s*\(", body):
                continue
            if re.search(rf"\b{n}\s*\.\s*wait\s*\(", body):
                continue
            for i, ln in enumerate(body_lines):
                if not re.search(rf"\b{n}\s*\.\s*arrive\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-BARRIER",
                    message=f"{name}.arrive() without wait()/arrive_and_wait()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_task(lines, rel, funcs, out) -> None:
    """std::task / execution::task started without sync_wait / co_await.

    Distinct from CXX-SYNC-WAIT (discarded sync_wait result) and CXX-STD-ASYNC.
    """
    for fn in funcs:
        body = fn.body or ""
        if _rx(r"\bsync_wait\s*\(|\bco_await\b").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_TASK_STARTED.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-TASK",
                message="std::task started without sync_wait/co_await",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_generator_discard(lines, rel, funcs, out) -> None:
    """std::generator constructed from a call and not iterated (CWE-252).

    Distinct from CXX-GENERATOR (that yields a pointer/reference to a local).
    """
    for fn in funcs:
        body = fn.body or ""
        if _rx(r"\bfor\s*\(").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_GENERATOR_ASSIGN.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-GENERATOR-DISCARD",
                message="std::generator constructed and not iterated",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _osync_has_emit(name: str, body: str) -> bool:
    n = re.escape(name)
    return bool(re.search(rf"\b{n}\s*\.\s*emit\s*\(", body))


def _cxx_osyncstream(lines, rel, funcs, out) -> None:
    """osyncstream constructed then destroyed without .emit() (CWE-662).

    Distinct from CXX-SYNCBUF (`syncbuf` / `basic_syncbuf`).
    """
    for fn in funcs:
        body = fn.body or ""
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            m = _CXX_OSYNC_DECL.search(ln)
            if not m:
                continue
            name = m.group("name")
            if _osync_has_emit(name, body):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-OSYNCSTREAM",
                message=f"{name} destroyed without emit()",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _packaged_invoked(name: str, ln: str) -> bool:
    if _CXX_PACKAGED_DECL.search(ln):
        return False
    n = re.escape(name)
    return bool(re.search(rf"\b{n}\s*\(", ln))


def _cxx_packaged_task(lines, rel, funcs, out) -> None:
    """packaged_task invoked without if (t) (CWE-476).

    Distinct from CXX-STD-FUNCTION-NULL (`std::function`).
    """
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_PACKAGED_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for name in sorted(names):
                if _optional_has_guard(name, body):
                    continue
                if not _packaged_invoked(name, ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-PACKAGED-TASK",
                    message=f"{name} invoked without a truth test",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_flat_multiset(lines, rel, funcs, out) -> None:
    """flat_multiset find() dereference without != end() (CWE-476).

    Distinct from CXX-FLAT-SET and CXX-FLAT-MULTIMAP.
    """
    for fn in funcs:
        body = fn.body or ""
        names = {
            m.group("name") for m in _lit_finditer(_CXX_FLAT_MULTISET_DECL, body)
        }
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            if _flat_set_has_end_guard(name, body):
                continue
            n = re.escape(name)
            for i, ln in enumerate(body.splitlines()):
                if not (
                    re.search(rf"\*\s*{n}\s*\.\s*find\s*\(", ln)
                    or re.search(rf"\b{n}\s*\.\s*find\s*\(", ln)
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-FLAT-MULTISET",
                    message=f"{name}.find() dereferenced without != end()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_syncbuf(lines, rel, funcs, out) -> None:
    """syncbuf / basic_syncbuf destroyed without .emit() (CWE-662).

    Distinct from CXX-OSYNCSTREAM (`osyncstream`).
    """
    for fn in funcs:
        body = fn.body or ""
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            m = _CXX_SYNCBUF_DECL.search(ln)
            if not m:
                continue
            name = m.group("name")
            if _osync_has_emit(name, body):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-SYNCBUF",
                message=f"{name} destroyed without emit()",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _counted_count_guarded(n: str, body: str) -> bool:
    ne = re.escape(n)
    if re.search(rf"\b{ne}\s*(?:<=|<|>=|>)\s*0\b", body):
        return True
    if re.search(rf"\b{ne}\s*(?:<=|<)\s*1\b", body):
        return True
    if re.search(rf"\b0\s*(?:>=|>|<|<=)\s*{ne}\b", body):
        return True
    if re.search(rf"\b{ne}\s*[<>]=?\s*\w+\s*\.\s*(?:size|ssize)\s*\(", body):
        return True
    return False


def _cxx_counted_iterator(lines, rel, funcs, out) -> None:
    """counted_iterator(p, n) / begin() without an n>0 size guard (CWE-125)."""
    for fn in funcs:
        body = fn.body or ""
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            m = _CXX_COUNTED_ITER_CTOR.search(ln)
            if not m:
                continue
            src = m.group("src").strip()
            n = m.group("n")
            if not (
                _rx(r"[A-Za-z_]\w*").fullmatch(src)
                or _rx(r"\.\s*begin\s*\(").search(src)
            ):
                continue
            if _counted_count_guarded(n, body):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-COUNTED-ITERATOR",
                message=f"counted_iterator count {n} has no remaining-size guard",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_promise(lines, rel, funcs, out) -> None:
    """promise.get_future() then .get() without set_value (CWE-394).

    Distinct from CXX-FUTURE-GET (get on a default-constructed future).
    """
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_PROMISE_DECL, body)}
        if not names:
            continue
        if not _rx(r"\bget_future\s*\(").search(body):
            continue
        if _rx(r"\bset_value(?:_at_thread_exit)?\s*\(").search(body):
            continue
        if _rx(r"\bset_exception(?:_at_thread_exit)?\s*\(").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if _rx(r"\.\s*get_future\s*\(").search(ln):
                continue
            if not _rx(r"\.\s*get\s*\(").search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-PROMISE",
                message="promise get_future used without set_value/set_exception",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _enum_range_guarded(arg: str, body: str) -> bool:
    a = re.escape(arg)
    if re.search(rf"\b{a}\s*(?:==|!=)", body):
        return True
    return bool(re.search(rf"(?:==|!=)\s*{a}\b", body))


def _cxx_weak_ptr(lines, rel, funcs, out) -> None:
    """weak_ptr.lock() used without expired() or a truth test (CWE-416).

    Requires `weak_ptr` so shared_ptr plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_WEAK_PTR_DECL, body)}
        if not names:
            continue
        if _rx(r"\bexpired\s*\(").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for name in sorted(names):
                if not re.search(rf"\b{re.escape(name)}\s*\.\s*lock\s*\(", ln):
                    continue
                if _rx(r"\bif\s*\(").search(ln):
                    continue
                m = _CXX_WEAK_LOCK.search(ln)
                if m and m.group("weak") == name:
                    locked = m.group("locked")
                    if locked and (
                        _param_if_guard(locked, body)
                        or _param_null_tested(locked, body)
                    ):
                        continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-WEAK-PTR",
                    message=f"{name}.lock() used without expired() or a "
                    "truth test",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_exception_ptr(lines, rel, funcs, out) -> None:
    """exception_ptr rethrown without a nullptr test (CWE-476).

    Requires `exception_ptr` so CXX-PROMISE / future_get plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_EXCEPTION_PTR_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for name in sorted(names):
                if _param_if_guard(name, body) or _param_null_tested(name, body):
                    continue
                if not re.search(
                    rf"(?:std\s*::\s*)?rethrow_exception\s*\(\s*"
                    rf"{re.escape(name)}\b",
                    ln,
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-EXCEPTION-PTR",
                    message=f"rethrow_exception({name}) without a nullptr test",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_coro_handle(lines, rel, funcs, out) -> None:
    """coroutine_handle.resume() without done() (CWE-762).

    Requires `coroutine_handle` so co_await-only plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        names = {m.group("name") for m in _lit_finditer(_CXX_CORO_HANDLE_DECL, body)}
        if not names:
            continue
        if _rx(r"\.\s*done\s*\(").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for name in sorted(names):
                if not re.search(
                    rf"\b{re.escape(name)}\s*\.\s*resume\s*\(", ln,
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-CORO-HANDLE",
                    message=f"{name}.resume() without done()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_valarray(lines, rel, funcs, out) -> None:
    """valarray operator[] without a size() guard (CWE-125).

    Requires `valarray` so vector / simd / span / array plants stay silent.
    """
    for fn in funcs:
        names = {
            m.group("name") for m in _CXX_VALARRAY_DECL.finditer(fn.body or "")
        }
        if not names:
            continue
        body = fn.body or ""
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                cont = m.group("cont")
                if cont not in names:
                    continue
                if re.search(rf"\b{re.escape(cont)}\s*\.\s*size\s*\(", body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-VALARRAY",
                    message=f"{cont}[{m.group('idx').strip()}] without a "
                    "size() guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_to_underlying(lines, rel, funcs, out) -> None:
    """to_underlying used without an enum-range guard (CWE-704).

    Requires `to_underlying` so static_cast plants stay silent. Skip if the
    argument is compared with == / != anywhere in the function.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"\bto_underlying\s*\(").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            m = _CXX_TO_UNDERLYING.search(ln)
            if not m:
                continue
            if _enum_range_guarded(m.group("arg"), body):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-TO-UNDERLYING",
                message=f"to_underlying({m.group('arg')}) without an "
                "enum-range guard",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_unexpected(lines, rel, funcs, out) -> None:
    """std::unexpected<T> constructed and discarded (CWE-252).

    Distinct from CXX-EXPECTED-ERROR / CXX-EXPECTED-NULL (`expected<` +
    `.error()` / dereference). Matches `unexpected\\s*<` only — not the
    removed C++ exception API `unexpected(`. Skip functions that also call
    has_value / error() or declare expected<.
    """
    for fn in funcs:
        body = fn.body or ""
        if _rx(r"\bhas_value\s*\(").search(body):
            continue
        if _rx(r"\.\s*error\s*\(").search(body):
            continue
        if _rx(r"(?:std\s*::\s*)?\bexpected\s*<").search(body):
            continue
        start = fn.span[0]
        body_lines = body.splitlines()
        for i, ln in enumerate(body_lines):
            m = _CXX_UNEXPECTED_DECL.search(ln)
            if not m:
                continue
            name = m.group("name")
            ne = re.escape(name)
            if any(re.search(rf"\b{ne}\b", later) for later in body_lines[i + 1:]):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-UNEXPECTED",
                message=f"{name} unexpected< constructed and discarded",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_tuple_get(lines, rel, funcs, out) -> None:
    """std::get on tuple without tuple_size / structured binding (CWE-125).

    Requires `tuple` and `std::get` or `get<`. Skip variant-only bodies
    (CXX-VARIANT-GET). Skip `tuple_size` and `auto [` structured bindings.
    """
    for fn in funcs:
        body = fn.body or ""
        if "variant" in body and "tuple" not in body:
            continue
        if "tuple" not in body:
            continue
        if not (_rx(r"std\s*::\s*get").search(body) or _rx(r"get\s*<").search(body)):
            continue
        if not _lit_search(_CXX_TUPLE_DECL, body):
            continue
        if _rx(r"\btuple_size\b").search(body):
            continue
        if _rx(r"\bauto\s*\[").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_TUPLE_GET.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-TUPLE-GET",
                message="std::get on tuple without tuple_size/index guard",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_deque_index(lines, rel, funcs, out) -> None:
    """deque operator[] without a size() guard (CWE-125).

    Requires `deque` so vector / valarray / span / mdspan / simd stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if "deque" not in body:
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_DEQUE_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                cont = m.group("cont")
                if cont not in names:
                    continue
                if re.search(rf"\b{re.escape(cont)}\s*\.\s*size\s*\(", body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-DEQUE-INDEX",
                    message=f"{cont}[{m.group('idx').strip()}] without a "
                    "size() guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_forward_list(lines, rel, funcs, out) -> None:
    """forward_list.front() without empty() (CWE-125).

    Requires `forward_list` so std::list / initializer_list stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if "forward_list" not in body:
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_FWD_LIST_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            n = re.escape(name)
            if re.search(rf"\b{n}\s*\.\s*empty\s*\(", body):
                continue
            for i, ln in enumerate(body.splitlines()):
                if not re.search(rf"\b{n}\s*\.\s*front\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-FORWARD-LIST",
                    message=f"{name}.front() without empty()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_list_front(lines, rel, funcs, out) -> None:
    """std::list.front()/.back() without empty() (CWE-125).

    Requires `std::list<` or `list<`. Skip forward_list and initializer_list.
    """
    for fn in funcs:
        body = fn.body or ""
        if "forward_list" in body or "initializer_list" in body:
            continue
        if not _rx(r"(?:std\s*::\s*)?\blist\s*<").search(body):
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_LIST_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            n = re.escape(name)
            if re.search(rf"\b{n}\s*\.\s*empty\s*\(", body):
                continue
            for i, ln in enumerate(body.splitlines()):
                if not re.search(rf"\b{n}\s*\.\s*(?:front|back)\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-LIST-FRONT",
                    message=f"{name}.front()/.back() without empty()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _map_key_guarded(name: str, body: str) -> bool:
    n = re.escape(name)
    return bool(re.search(
        rf"\b{n}\s*\.\s*(?:contains|count|find)\s*\(", body
    ))


def _cxx_map_at(lines, rel, funcs, out) -> None:
    """std::map .at( without count()/find()/contains() (CWE-125).

    Requires `\\bmap\\s*<`. Skip flat_map / unordered_map / flat_multimap.
    """
    for fn in funcs:
        body = fn.body or ""
        if _rx(r"flat_map|unordered_map|flat_multimap").search(body):
            continue
        if not _rx(r"\bmap\s*<").search(body):
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_MAP_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            if _map_key_guarded(name, body):
                continue
            for i, ln in enumerate(body.splitlines()):
                if not re.search(rf"\b{re.escape(name)}\s*\.\s*at\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-MAP-AT",
                    message=f"{name}.at() without count()/find()/contains()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_unordered_at(lines, rel, funcs, out) -> None:
    """unordered_map .at( without count()/find()/contains() (CWE-125).

    Requires `unordered_map` so std::map / flat_map stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if "unordered_map" not in body:
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_UMAP_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            if _map_key_guarded(name, body):
                continue
            for i, ln in enumerate(body.splitlines()):
                if not re.search(rf"\b{re.escape(name)}\s*\.\s*at\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-UNORDERED-AT",
                    message=f"{name}.at() without count()/find()/contains()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_set_find(lines, rel, funcs, out) -> None:
    """std::set find dereference without end() (CWE-476).

    Requires `\\bstd\\s*::\\s*set\\s*<` or `\\bset\\s*<`. Skip flat_set,
    unordered_set, multiset, flat_multiset.
    """
    for fn in funcs:
        body = fn.body or ""
        if _rx(r"flat_set|unordered_set|multiset|flat_multiset").search(body):
            continue
        if not (
            _rx(r"\bstd\s*::\s*set\s*<").search(body)
            or _rx(r"\bset\s*<").search(body)
        ):
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_SET_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            if _flat_set_has_end_guard(name, body):
                continue
            n = re.escape(name)
            for i, ln in enumerate(body.splitlines()):
                if not (
                    re.search(rf"\*\s*{n}\s*\.\s*find\s*\(", ln)
                    or re.search(rf"\b{n}\s*\.\s*find\s*\(", ln)
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-SET-FIND",
                    message=f"{name}.find() dereferenced without != end()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_queue_front(lines, rel, funcs, out) -> None:
    """std::queue .front()/.pop() without empty() (CWE-125).

    Requires `queue` but not dequeue. Skip priority_queue bodies unless
    `\\bqueue\\s*<` remains after stripping priority_queue.
    """
    for fn in funcs:
        body = fn.body or ""
        if "dequeue" in body:
            continue
        if "queue" not in body:
            continue
        stripped = body.replace("priority_queue", "")
        if not _rx(r"\bqueue\s*<").search(stripped):
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_QUEUE_DECL, stripped)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            n = re.escape(name)
            if re.search(rf"\b{n}\s*\.\s*empty\s*\(", body):
                continue
            for i, ln in enumerate(body.splitlines()):
                if not re.search(rf"\b{n}\s*\.\s*(?:front|pop)\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-QUEUE-FRONT",
                    message=f"{name}.front()/.pop() without empty()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_stack_top(lines, rel, funcs, out) -> None:
    """std::stack .top()/.pop() without empty() (CWE-125).

    Requires `\\bstd\\s*::\\s*stack\\s*<` or `\\bstack\\s*<`. Skip stacktrace.
    """
    for fn in funcs:
        body = fn.body or ""
        if "stacktrace" in body:
            continue
        if not (
            _rx(r"\bstd\s*::\s*stack\s*<").search(body)
            or _rx(r"\bstack\s*<").search(body)
        ):
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_STACK_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            n = re.escape(name)
            if re.search(rf"\b{n}\s*\.\s*empty\s*\(", body):
                continue
            for i, ln in enumerate(body.splitlines()):
                if not re.search(rf"\b{n}\s*\.\s*(?:top|pop)\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-STACK-TOP",
                    message=f"{name}.top()/.pop() without empty()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_priority_queue(lines, rel, funcs, out) -> None:
    """std::priority_queue .top()/.pop() without empty() (CWE-125).

    Requires `priority_queue` so std::queue stays silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if "priority_queue" not in body:
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_PQUEUE_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            n = re.escape(name)
            if re.search(rf"\b{n}\s*\.\s*empty\s*\(", body):
                continue
            for i, ln in enumerate(body.splitlines()):
                if not re.search(rf"\b{n}\s*\.\s*(?:top|pop)\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-PRIORITY-QUEUE",
                    message=f"{name}.top()/.pop() without empty()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _array_index_guarded(name: str, idx: str, n: str, body: str) -> bool:
    ne, i, lit = re.escape(name), re.escape(idx.strip()), re.escape(n.strip())
    if re.search(rf"\b{ne}\s*\.\s*size\s*\(", body):
        return True
    if lit and re.search(rf"\b{i}\s*(?:<|>=|>|<=)\s*{lit}\b", body):
        return True
    if lit and re.search(rf"\b{lit}\s*(?:>|>=|<|<=)\s*{i}\b", body):
        return True
    return False


def _cxx_array_index(lines, rel, funcs, out) -> None:
    """std::array operator[] without a size()/N guard (CWE-125).

    Requires `\\b(?:std\\s*::\\s*)?array\\s*<` so C arrays / vector / valarray
    / simd / deque / bitset stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"\b(?:std\s*::\s*)?array\s*<").search(body):
            continue
        decls = list(_lit_finditer(_CXX_ARRAY_DECL, body))
        if not decls:
            continue
        names = {d.group("name"): d.group("n") for d in decls}
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                cont = m.group("cont")
                if cont not in names:
                    continue
                if _array_index_guarded(cont, m.group("idx"), names[cont], body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-ARRAY-INDEX",
                    message=f"{cont}[{m.group('idx').strip()}] without a "
                    "size()/N guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_unordered_set(lines, rel, funcs, out) -> None:
    """unordered_set find dereference without end() (CWE-476).

    Requires `unordered_set` so std::set / unordered_map stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if "unordered_set" not in body:
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_USET_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            if _flat_set_has_end_guard(name, body):
                continue
            n = re.escape(name)
            for i, ln in enumerate(body.splitlines()):
                if not (
                    re.search(rf"\*\s*{n}\s*\.\s*find\s*\(", ln)
                    or re.search(rf"\b{n}\s*\.\s*find\s*\(", ln)
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-UNORDERED-SET",
                    message=f"{name}.find() dereferenced without != end()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_local_wstrings(fn) -> set[str]:
    params = {name for typ, name in fn.params if name}
    names: set[str] = set()
    for m in _CXX_WSTRING_DECL.finditer(fn.body or ""):
        if m.group("static"):
            continue
        name = m.group("name")
        if name in params or name in _KW:
            continue
        names.add(name)
    return names


def _cxx_wstring_view(lines, rel, funcs, out) -> None:
    """wstring_view / wstring borrows local storage (CWE-416).

    Distinct from CXX-DANGLING-REF (`string_view` / `span` of a local
    `string`). Requires `wstring_view` or `wstring`.
    """
    for fn in funcs:
        blob = f"{fn.signature or ''}\n{fn.return_type or ''}\n{fn.body or ''}"
        if not _rx(r"\bwstring_view\b|\bwstring\b").search(blob):
            continue
        local_ws = _cxx_local_wstrings(fn)
        if not local_ws:
            continue
        view_from_local: set[str] = set()
        for m in _CXX_WSTRING_VIEW_BIND.finditer(fn.body or ""):
            if m.group("src") in local_ws:
                view_from_local.add(m.group("name"))
        ret_is_wview = bool(_rx(r"\bwstring_view\b").search(fn.return_type or ""))
        start = fn.span[0]
        for i, ln in enumerate((fn.body or "").splitlines()):
            hit = None
            sm = _RETURN_VAR.search(ln)
            if sm:
                name = sm.group(1)
                if name in view_from_local:
                    hit = name
                elif name in local_ws and ret_is_wview:
                    hit = name
            if not hit:
                sm = _CXX_RETURN_WVIEW.search(ln)
                if sm and sm.group("name") in local_ws:
                    hit = sm.group("name")
            if not hit:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-WSTRING-VIEW",
                message=f"return borrows local wstring {hit}",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_multimap_find(lines, rel, funcs, out) -> None:
    """std::multimap find dereference without end() (CWE-476).

    Requires `multimap` after skipping `flat_multimap`.
    """
    for fn in funcs:
        body = fn.body or ""
        if "flat_multimap" in body:
            continue
        if not _rx(r"\bmultimap\s*<").search(body):
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_MULTIMAP_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            if _flat_set_has_end_guard(name, body):
                continue
            n = re.escape(name)
            for i, ln in enumerate(body.splitlines()):
                if not (
                    re.search(rf"\*\s*{n}\s*\.\s*find\s*\(", ln)
                    or re.search(rf"\b{n}\s*\.\s*find\s*\(", ln)
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-MULTIMAP-FIND",
                    message=f"{name}.find() dereferenced without != end()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_multiset_find(lines, rel, funcs, out) -> None:
    """std::multiset find dereference without end() (CWE-476).

    Requires `multiset` after skipping `flat_multiset`. Distinct from
    CXX-SET-FIND (std::set) and CXX-FLAT-MULTISET.
    """
    for fn in funcs:
        body = fn.body or ""
        if "flat_multiset" in body:
            continue
        if not _rx(r"\bmultiset\s*<").search(body):
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_MULTISET_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            if _flat_set_has_end_guard(name, body):
                continue
            n = re.escape(name)
            for i, ln in enumerate(body.splitlines()):
                if not (
                    re.search(rf"\*\s*{n}\s*\.\s*find\s*\(", ln)
                    or re.search(rf"\b{n}\s*\.\s*find\s*\(", ln)
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-MULTISET-FIND",
                    message=f"{name}.find() dereferenced without != end()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_binary_semaphore(lines, rel, funcs, out) -> None:
    """binary_semaphore(0).acquire() without try_acquire/release (CWE-667).

    Distinct from CXX-SEMAPHORE (`counting_semaphore`) and CXX-LATCH.
    Requires `binary_semaphore`.
    """
    for fn in funcs:
        body = fn.body or ""
        if "binary_semaphore" not in body:
            continue
        if "counting_semaphore" in body:
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_BINSEM_ZERO, body)}
        if not names:
            continue
        if _rx(r"\.\s*try_acquire\s*\(").search(body):
            continue
        if _rx(r"\.\s*release\s*\(").search(body):
            continue
        start = fn.span[0]
        body_lines = body.splitlines()
        for name in sorted(names):
            n = re.escape(name)
            for i, ln in enumerate(body_lines):
                if not re.search(rf"\b{n}\s*\.\s*acquire\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-BINARY-SEMAPHORE",
                    message=f"{name}.acquire() on binary_semaphore(0) without "
                    "try_acquire()/release()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_error_code(lines, rel, funcs, out) -> None:
    """std::error_code used without if(ec) or .value() (CWE-252).

    Requires `error_code`. Skip if `if (` on the variable or `.value(`
    is checked.
    """
    for fn in funcs:
        body = fn.body or ""
        if "error_code" not in body:
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_ERROR_CODE_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            if _param_if_guard(name, body):
                continue
            n = re.escape(name)
            if re.search(rf"\b{n}\s*\.\s*value\s*\(", body):
                continue
            for i, ln in enumerate(body.splitlines()):
                if not re.search(rf"\b{n}\s*\.", ln):
                    continue
                if re.search(rf"error_code\s+{n}\b", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-ERROR-CODE",
                    message=f"{name} used without if({name}) or .value()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _byteswap_idx_guarded(idx: str, body: str) -> bool:
    i = re.escape(idx.strip())
    if re.search(rf"\b{i}\s*(?:>=|>|<|<=)\s*\d+", body):
        return True
    if re.search(rf"\d+\s*(?:>=|>|<|<=)\s*{i}\b", body):
        return True
    return bool(re.search(
        rf"\b{i}\s*(?:>=|>|<|<=)\s*[A-Za-z_]\w*\s*\.\s*size\s*\(", body
    ))


def _cxx_byteswap(lines, rel, funcs, out) -> None:
    """byteswap result used as an array index without a range check (CWE-125).

    Requires `byteswap`. Inline `a[byteswap(n)]` always fires; a named
    result is skipped when compared against a bound.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"\bbyteswap\s*\(").search(body):
            continue
        assigned: set[str] = set()
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _BYTESWAP_ASSIGN.finditer(ln):
                assigned.add(m.group("var"))
            if _BYTESWAP_IN_INDEX.search(ln):
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-BYTESWAP",
                    message="byteswap() result used as an array index "
                    "without a range check",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for m in _CXX_SUBSCRIPT.finditer(ln):
                idx = m.group("idx").strip()
                if idx not in assigned:
                    continue
                if _byteswap_idx_guarded(idx, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-BYTESWAP",
                    message=f"{m.group('cont')}[{idx}] indexes with "
                    "byteswap() without a range check",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_pmr(lines, rel, funcs, out) -> None:
    """std::pmr container operator[] without size() (CWE-125).

    Requires `pmr::` or `std::pmr` so CXX-VECTOR-INDEX (vec_index.cpp)
    stays silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if "pmr::" not in body and "std::pmr" not in body:
            continue
        if _rx(r"\.\s*size\s*\(").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_SUBSCRIPT.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-PMR",
                message="pmr container operator[] without size() guard",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_local_u8strings(fn) -> set[str]:
    params = {name for typ, name in fn.params if name}
    names: set[str] = set()
    for m in _CXX_U8STRING_DECL.finditer(fn.body or ""):
        if m.group("static"):
            continue
        name = m.group("name")
        if name in params or name in _KW:
            continue
        names.add(name)
    return names


def _cxx_u8string_view(lines, rel, funcs, out) -> None:
    """u8string_view / u8string borrows local storage (CWE-416).

    Distinct from CXX-DANGLING-REF (`string_view`) and CXX-WSTRING-VIEW.
    Requires `u8string_view` or `u8string`.
    """
    for fn in funcs:
        blob = f"{fn.signature or ''}\n{fn.return_type or ''}\n{fn.body or ''}"
        if not _rx(r"\bu8string_view\b|\bu8string\b").search(blob):
            continue
        local_u8 = _cxx_local_u8strings(fn)
        if not local_u8:
            continue
        view_from_local: set[str] = set()
        for m in _CXX_U8STRING_VIEW_BIND.finditer(fn.body or ""):
            if m.group("src") in local_u8:
                view_from_local.add(m.group("name"))
        ret_is_u8view = bool(_rx(r"\bu8string_view\b").search(fn.return_type or ""))
        start = fn.span[0]
        for i, ln in enumerate((fn.body or "").splitlines()):
            hit = None
            sm = _RETURN_VAR.search(ln)
            if sm:
                name = sm.group(1)
                if name in view_from_local:
                    hit = name
                elif name in local_u8 and ret_is_u8view:
                    hit = name
            if not hit:
                sm = _CXX_RETURN_U8VIEW.search(ln)
                if sm and sm.group("name") in local_u8:
                    hit = sm.group("name")
            if not hit:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-U8STRING-VIEW",
                message=f"return borrows local u8string {hit}",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_unordered_multimap(lines, rel, funcs, out) -> None:
    """unordered_multimap find dereference without end() (CWE-476).

    Requires `unordered_multimap` so unordered_map / multimap /
    flat_multimap stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if "unordered_multimap" not in body:
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_UMMAP_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            if _flat_set_has_end_guard(name, body):
                continue
            n = re.escape(name)
            for i, ln in enumerate(body.splitlines()):
                if not (
                    re.search(rf"\*\s*{n}\s*\.\s*find\s*\(", ln)
                    or re.search(rf"\b{n}\s*\.\s*find\s*\(", ln)
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-UNORDERED-MULTIMAP",
                    message=f"{name}.find() dereferenced without != end()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_unordered_multiset(lines, rel, funcs, out) -> None:
    """unordered_multiset find dereference without end() (CWE-476).

    Requires `unordered_multiset` so unordered_set / multiset / set /
    flat_multiset stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if "unordered_multiset" not in body:
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_UMSET_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            if _flat_set_has_end_guard(name, body):
                continue
            n = re.escape(name)
            for i, ln in enumerate(body.splitlines()):
                if not (
                    re.search(rf"\*\s*{n}\s*\.\s*find\s*\(", ln)
                    or re.search(rf"\b{n}\s*\.\s*find\s*\(", ln)
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-UNORDERED-MULTISET",
                    message=f"{name}.find() dereferenced without != end()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_shared_lock(lines, rel, funcs, out) -> None:
    """shared_lock used without owns_lock() / if (lk (CWE-667).

    Requires `shared_lock` so unique_lock / shared_mutex plants stay
    silent. Skip when owns_lock() or if (lk is present.
    """
    for fn in funcs:
        body = fn.body or ""
        if "shared_lock" not in body:
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_SHARED_LOCK_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            n = re.escape(name)
            if re.search(rf"\b{n}\s*\.\s*owns_lock\s*\(", body):
                continue
            if re.search(rf"\bif\s*\(\s*!?{n}\b", body):
                continue
            for i, ln in enumerate(body.splitlines()):
                if not re.search(rf"\b{n}\s*\.\s*(?:mutex|unlock)\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-SHARED-LOCK",
                    message=f"{name} used without owns_lock() or if ({name})",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_atomic_flag(lines, rel, funcs, out) -> None:
    """atomic_flag.test_and_set without a prior clear() (CWE-667).

    Requires `atomic_flag` so atomic_ref / std::atomic< stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if "atomic_flag" not in body:
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_ATOMIC_FLAG_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        body_lines = body.splitlines()
        for name in sorted(names):
            n = re.escape(name)
            seen_clear = False
            for i, ln in enumerate(body_lines):
                if re.search(rf"\b{n}\s*\.\s*clear\s*\(", ln):
                    seen_clear = True
                    continue
                if not re.search(rf"\b{n}\s*\.\s*test_and_set\s*\(", ln):
                    continue
                if seen_clear:
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-ATOMIC-FLAG",
                    message=f"{name}.test_and_set() without a prior clear()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_condvar_any(lines, rel, funcs, out) -> None:
    """condition_variable_any.wait without a lock / predicate (CWE-662).

    Requires `condition_variable_any` so CXX-CONDITION-WAIT plants
    (`condition_variable`) stay on that class.
    """
    for fn in funcs:
        body = fn.body or ""
        if "condition_variable_any" not in body:
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_CVANY_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            n = re.escape(name)
            for i, ln in enumerate(body.splitlines()):
                if not re.search(rf"\b{n}\s*\.\s*wait\s*\(", ln):
                    continue
                if "," in ln:
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-CONDVAR-ANY",
                    message=f"{name}.wait() without a lock / predicate",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_recursive_mutex(lines, rel, funcs, out) -> None:
    """recursive_mutex.lock() without unlock or lock_guard (CWE-667).

    Requires `recursive_mutex` so std::mutex / shared_mutex stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if "recursive_mutex" not in body:
            continue
        if _lit_search(_CXX_LOCK_RAII, body):
            continue
        names = {
            m.group("name") for m in _lit_finditer(_CXX_RECURSIVE_MUTEX_DECL, body)
        }
        if not names:
            continue
        start = fn.span[0]
        body_lines = body.splitlines()
        for name in sorted(names):
            n = re.escape(name)
            if re.search(rf"\b{n}\s*\.\s*unlock\s*\(", body):
                continue
            for i, ln in enumerate(body_lines):
                if not re.search(rf"\b{n}\s*\.\s*lock\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-RECURSIVE-MUTEX",
                    message=f"{name}.lock() without unlock or lock_guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_timed_mutex(lines, rel, funcs, out) -> None:
    """timed_mutex try_lock/lock discarded or lock without unlock (CWE-667).

    `\\btimed_mutex\\b` does not match recursive_timed_mutex /
    shared_timed_mutex. std::mutex plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"\btimed_mutex\b").search(body):
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_TIMED_MUTEX_DECL, body)}
        if not names:
            continue
        has_raii = bool(_lit_search(_CXX_LOCK_RAII, body))
        start = fn.span[0]
        body_lines = body.splitlines()
        for name in sorted(names):
            n = re.escape(name)
            unlocked = bool(re.search(rf"\b{n}\s*\.\s*unlock\s*\(", body))
            for i, ln in enumerate(body_lines):
                discarded_try = bool(
                    re.search(rf"\b{n}\s*\.\s*try_lock\s*\(", ln)
                    and not _rx(r"\bif\s*\(|\bwhile\s*\(|\breturn\b|=").search(ln)
                )
                bare_lock = bool(
                    re.search(rf"\b{n}\s*\.\s*lock\s*\(", ln)
                    and not unlocked
                    and not has_raii
                )
                if not discarded_try and not bare_lock:
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-TIMED-MUTEX",
                    message=f"{name} try_lock/lock discarded or lock "
                    "without unlock",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_fstream(lines, rel, funcs, out) -> None:
    """ifstream/ofstream used without is_open() / truth test (CWE-252).

    Requires ifstream/ofstream/fstream so stringstream / spanstream /
    osyncstream plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"\b(?:ifstream|ofstream|fstream)\b").search(body):
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_FSTREAM_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        for name in sorted(names):
            n = re.escape(name)
            if re.search(rf"\b{n}\s*\.\s*is_open\s*\(", body):
                continue
            if re.search(rf"\bif\s*\(\s*!?{n}\b", body):
                continue
            for i, ln in enumerate(body.splitlines()):
                if not (
                    re.search(
                        rf"\b{n}\s*\.\s*"
                        rf"(?:get|read|write|getline|put|peek)\s*\(",
                        ln,
                    )
                    or re.search(rf"\b{n}\s*(?:<<|>>)", ln)
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-FSTREAM",
                    message=f"{name} used without is_open() or a truth test",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_this_thread(lines, rel, funcs, out) -> None:
    """this_thread::sleep_for as the only sync around a shared store.

    Requires `this_thread` so std::thread / jthread plants stay silent.
    """
    globals_ = _file_scope_int_globals(lines)
    for fn in funcs:
        body = fn.body or ""
        if "this_thread" not in body:
            continue
        if not _rx(r"this_thread\s*::\s*sleep_for\s*\(").search(body):
            continue
        if _rx(r"\b(?:mutex|lock_guard|unique_lock|scoped_lock|atomic)\b").search(body):
            continue
        statics = set(
            _rx(r"\bstatic\s+int\s+([A-Za-z_]\w*)\b").findall(body)
        )
        stores = globals_ | statics
        if not any(
            re.search(rf"\b{re.escape(g)}\s*=", body) for g in stores
        ):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _rx(r"this_thread\s*::\s*sleep_for\s*\(").search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-THIS-THREAD",
                message="this_thread::sleep_for used as the only sync "
                "around a shared store",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_call_once(lines, rel, funcs, out) -> None:
    """std::call_once without an once_flag object in the function (CWE-662).

    Requires `call_once` so pthread_once C plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if "call_once" not in body:
            continue
        if not _rx(r"\bcall_once\s*\(").search(body):
            continue
        if _rx(r"\bonce_flag\b").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _rx(r"\bcall_once\s*\(").search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-CALL-ONCE",
                message="std::call_once invoked without an once_flag "
                "object in scope",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_shared_timed_mutex(lines, rel, funcs, out) -> None:
    """shared_timed_mutex.lock/lock_shared without unlock (CWE-667).

    Requires `\\bshared_timed_mutex\\b` so shared_mutex / shared_lock /
    timed_mutex plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"\bshared_timed_mutex\b").search(body):
            continue
        if _lit_search(_CXX_LOCK_RAII, body):
            continue
        names = {
            m.group("name")
            for m in _lit_finditer(_CXX_SHARED_TIMED_MUTEX_DECL, body)
        }
        if not names:
            continue
        start = fn.span[0]
        body_lines = body.splitlines()
        for name in sorted(names):
            n = re.escape(name)
            unlocked = bool(re.search(rf"\b{n}\s*\.\s*unlock\s*\(", body))
            unshared = bool(
                re.search(rf"\b{n}\s*\.\s*unlock_shared\s*\(", body)
            )
            for i, ln in enumerate(body_lines):
                bare_lock = bool(
                    re.search(rf"\b{n}\s*\.\s*lock\s*\(", ln) and not unlocked
                )
                bare_shared = bool(
                    re.search(rf"\b{n}\s*\.\s*lock_shared\s*\(", ln)
                    and not unshared
                )
                if not bare_lock and not bare_shared:
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line,
                    cls="CXX-SHARED-TIMED-MUTEX",
                    message=f"{name}.lock/lock_shared without unlock",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_recursive_timed_mutex(lines, rel, funcs, out) -> None:
    """recursive_timed_mutex.lock() without unlock (CWE-667).

    Requires `\\brecursive_timed_mutex\\b` so recursive_mutex /
    timed_mutex plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"\brecursive_timed_mutex\b").search(body):
            continue
        if _lit_search(_CXX_LOCK_RAII, body):
            continue
        names = {
            m.group("name")
            for m in _lit_finditer(_CXX_RECURSIVE_TIMED_MUTEX_DECL, body)
        }
        if not names:
            continue
        start = fn.span[0]
        body_lines = body.splitlines()
        for name in sorted(names):
            n = re.escape(name)
            if re.search(rf"\b{n}\s*\.\s*unlock\s*\(", body):
                continue
            for i, ln in enumerate(body_lines):
                if not re.search(rf"\b{n}\s*\.\s*lock\s*\(", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line,
                    cls="CXX-RECURSIVE-TIMED-MUTEX",
                    message=f"{name}.lock() without unlock",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_system_error(lines, rel, funcs, out) -> None:
    """std::system_error used without .code() (CWE-252).

    Requires `system_error` so error_code / errc plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if "system_error" not in body:
            continue
        if _rx(r"\.code\s*\(").search(body):
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_SYSTEM_ERROR_DECL, body)}
        names |= {
            m.group("name") for m in _lit_finditer(_CXX_SYSTEM_ERROR_CATCH, body)
        }
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            used = "system_error" in ln or bool(
                _rx(r"\bthrow\s+(?:std\s*::\s*)?system_error\b").search(ln)
            )
            if names and not used:
                used = any(
                    re.search(rf"\b{re.escape(n)}\s*\.", ln) for n in names
                )
            if not used:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-SYSTEM-ERROR",
                message="std::system_error used without a .code() check",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_chrono_tzdb(lines, rel, funcs, out) -> None:
    """current_zone / tzdb used without a null/locate check (CWE-252/476).

    Requires `current_zone` or `tzdb` so generic chrono plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if "current_zone" not in body and "tzdb" not in body:
            continue
        if not _lit_search(_CXX_TZDB_CALL, body):
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_TZDB_BIND, body)}
        checked = any(_param_if_guard(n, body) for n in names)
        checked = checked or any(_param_null_tested(n, body) for n in names)
        if _rx(r"\.empty\s*\(").search(body):
            checked = True
        if _rx(r"\bif\s*\(\s*!\s*(?:std\s*::\s*chrono\s*::\s*)?"
            r"(?:current_zone|locate_zone)\s*\(").search(body):
            checked = True
        if checked:
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_TZDB_CALL.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-CHRONO-TZDB",
                message="current_zone/tzdb used without a null/locate check",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _zip_idx_bound_checks(idx: str, body: str) -> int:
    i = re.escape(idx.strip())
    n = len(re.findall(rf"\b{i}\s*(?:>=|>|<|<=)\s*", body))
    n += len(re.findall(
        rf"\.\s*size\s*\(\s*\)\s*(?:>=|>|<|<=)\s*{i}\b", body
    ))
    return n


def _cxx_ranges_zip(lines, rel, funcs, out) -> None:
    """views::zip / zip_view indexed or iterated without a size guard.

    Requires `views::zip` or `zip_view`, not generic `views::`.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_VIEWS_ZIP, body):
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_ZIP_AUTO, body)}
        names |= {m.group("name") for m in _lit_finditer(_CXX_ZIP_VIEW_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        body_lines = body.splitlines()
        for name in sorted(names):
            n = re.escape(name)
            for i, ln in enumerate(body_lines):
                sub = re.search(
                    rf"\b{n}\s*\[\s*(?P<idx>[^\]]+)\s*\]", ln
                )
                ranged = bool(re.search(rf"\bfor\s*\([^:]*:\s*{n}\b", ln))
                if not sub and not ranged:
                    continue
                if sub:
                    idx = sub.group("idx")
                    if _zip_idx_bound_checks(idx, body) >= 2:
                        continue
                elif ranged and (
                    len(_rx(r"\.\s*size\s*\(").findall(body)) >= 2
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-RANGES-ZIP",
                    message=f"{name} indexed/iterated without a size guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_format_to(lines, rel, funcs, out) -> None:
    """format_to into a raw char[] without size / format_to_n (CWE-120).

    Requires `format_to` so std::format / print plants stay silent.
    `format_to_n` does not match `\\bformat_to\\s*\\(`.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"\bformat_to\s*\(").search(body):
            continue
        bufs = {m.group("name") for m in _lit_finditer(_CXX_CHAR_BUF, body)}
        if not bufs:
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            m = _CXX_FORMAT_TO.search(ln)
            if not m:
                continue
            dst = m.group("dst")
            if dst not in bufs:
                continue
            if _rx(r"\bback_inserter\b").search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-FORMAT-TO",
                message="format_to into a raw char buffer without "
                "size / format_to_n",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_error_category(lines, rel, funcs, out) -> None:
    """error_category used without == / .name() / .message() (CWE-252).

    Requires `error_category` so error_code / system_error plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_ERROR_CATEGORY, body):
            continue
        if _rx(r"==").search(body):
            continue
        if _rx(r"\.\s*(?:name|message)\s*\(").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_ERROR_CATEGORY.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-ERROR-CATEGORY",
                message="error_category used without generic/system "
                "compare or .message()",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_nested_exception(lines, rel, funcs, out) -> None:
    """throw_with_nested / nested_exception without try/catch (CWE-705).

    Requires nested_exception or throw_with_nested or rethrow_if_nested
    so exception_ptr plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_NESTED_EXC, body):
            continue
        if _rx(r"\btry\b").search(body) and _rx(r"\bcatch\b").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_NESTED_EXC.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-NESTED-EXCEPTION",
                message="throw_with_nested / nested_exception without "
                "a try/catch",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_atomic_fence(lines, rel, funcs, out) -> None:
    """atomic_*_fence without memory_order, or as only sync (CWE-362).

    Requires atomic_thread_fence or atomic_signal_fence so atomic_flag /
    atomic_ref / __atomic plants stay silent.
    """
    globals_ = _file_scope_int_globals(lines)
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_ATOMIC_FENCE, body):
            continue
        missing_order = not _lit_search(_CXX_FENCE_ORDER, body)
        has_atomic_obj = bool(_rx(r"\batomic\s*<").search(body))
        statics = set(
            _rx(r"\bstatic\s+int\s+([A-Za-z_]\w*)\b").findall(body)
        )
        stores = globals_ | statics
        has_plain_store = any(
            re.search(rf"\b{re.escape(g)}\s*=", body) for g in stores
        )
        if not missing_order and (has_atomic_obj or not has_plain_store):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_ATOMIC_FENCE.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-ATOMIC-FENCE",
                message="atomic_thread_fence without memory_order or "
                "as only sync around a store",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_notify_thread_exit(lines, rel, funcs, out) -> None:
    """notify_all_at_thread_exit without a waiter (CWE-662).

    Requires the full name so cond_wait / condvar / cvany stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_NOTIFY_EXIT, body):
            continue
        if _lit_search(_CXX_CV_WAITER, body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_NOTIFY_EXIT.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-NOTIFY-THREAD-EXIT",
                message="notify_all_at_thread_exit without a waiting "
                "condition_variable",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_wstring_convert(lines, rel, funcs, out) -> None:
    """wstring_convert without .converted() / .empty() (CWE-416/252).

    Requires wstring_convert so wstring_view / wstring plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_WSTRING_CONVERT, body):
            continue
        if not _lit_search(_CXX_WCONVERT_XFORM, body):
            continue
        if _rx(r"\.\s*converted\s*\(").search(body):
            continue
        if _rx(r"\.\s*empty\s*\(").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not (
                _CXX_WCONVERT_XFORM.search(ln)
                or _CXX_WSTRING_CONVERT.search(ln)
            ):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-WSTRING-CONVERT",
                message="wstring_convert without .converted() or "
                "dangling local converter",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_invoke(lines, rel, funcs, out) -> None:
    """std::invoke on a nullable callable without a truth test (CWE-476).

    Requires std::invoke so bare f() / std::function plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"\bstd\s*::\s*invoke\s*\(").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            m = _CXX_INVOKE.search(ln)
            if not m:
                continue
            arg = m.group("arg")
            if _param_if_guard(arg, body) or _param_null_tested(arg, body):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-INVOKE",
                message="std::invoke on a nullable callable without "
                "a truth test",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_apply(lines, rel, funcs, out) -> None:
    """std::apply without tuple_size/null guard (CWE-476).

    Requires std::apply so std::invoke / function plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _rx(r"\bstd\s*::\s*apply\s*\(").search(body):
            continue
        if _rx(r"\btuple_size\b").search(body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            m = _CXX_APPLY.search(ln)
            if not m:
                continue
            arg = m.group("arg")
            if _param_if_guard(arg, body) or _param_null_tested(arg, body):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-APPLY",
                message="std::apply on a nullable callable or tuple "
                "without a size/tuple_size guard",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_local_scalar_names(fn) -> set[str]:
    params = {name for _, name in fn.params if name}
    names: set[str] = set()
    for m in _rx(r"\b(?:(?:unsigned|signed|const|volatile)\s+)*"
        r"(?:int|short|long|char|bool|float|double)\s+"
        r"(?P<name>[A-Za-z_]\w*)\b").finditer(fn.body or ""):
        n = m.group("name")
        if n not in params:
            names.add(n)
    return names


def _cxx_reference_wrapper(lines, rel, funcs, out) -> None:
    """std::ref/cref of a local that is returned (CWE-416).

    Requires reference_wrapper or std::ref/std::cref so function_ref
    / std::function plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_REFWRAP_TOKEN, body):
            continue
        locals_ = _cxx_local_scalar_names(fn)
        params = {name for _, name in fn.params if name}
        wrapped: set[str] = set()
        for m in _lit_finditer(_CXX_REFWRAP_BIND, body):
            arg = m.group("arg")
            if arg in locals_ and arg not in params:
                wrapped.add(m.group("w"))
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            hit = False
            rm = _CXX_REFWRAP_RETURN.search(ln)
            if rm and rm.group("arg") in locals_ and rm.group("arg") not in params:
                hit = True
            elif rv := _RETURN_VAR.search(ln):
                ret = rv.group(1)
                if ret in wrapped:
                    hit = True
            if not hit:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-REFERENCE-WRAPPER",
                message="std::ref/cref/reference_wrapper wraps a local "
                "that is returned",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_endian(lines, rel, funcs, out) -> None:
    """std::endian used as an index without a little/big range check (CWE-125).

    Requires std::endian so byteswap plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_ENDIAN, body):
            continue
        ranged = bool(_rx(r"\bstd\s*::\s*endian\s*::\s*(?:little|big)\b").search(body))
        assigned = {m.group("var") for m in _lit_finditer(_ENDIAN_ASSIGN, body)}
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if _ENDIAN_IN_INDEX.search(ln):
                if ranged:
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-ENDIAN",
                    message="std::endian used as an array index without "
                    "a range check",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for m in _CXX_SUBSCRIPT.finditer(ln):
                idx = m.group("idx").strip()
                if idx not in assigned:
                    continue
                if ranged or _byteswap_idx_guarded(idx, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-ENDIAN",
                    message=f"{m.group('cont')}[{idx}] indexes with "
                    "std::endian without a range check",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_bit_ceil(lines, rel, funcs, out) -> None:
    """bit_ceil/bit_floor/popcount as an index, or bit_ceil(0) (CWE-125).

    Requires those tokens so byteswap / __builtin_clz plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_BITCEIL_TOKEN, body):
            continue
        assigned: set[str] = set()
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _BITCEIL_ASSIGN.finditer(ln):
                assigned.add(m.group("var"))
            if _BITCEIL_ZERO.search(ln) or _BITCEIL_IN_INDEX.search(ln):
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-BIT-CEIL",
                    message="bit_ceil/bit_floor/popcount used as an index "
                    "without a range check, or bit_ceil(0)",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for m in _CXX_SUBSCRIPT.finditer(ln):
                idx = m.group("idx").strip()
                if idx not in assigned:
                    continue
                if _byteswap_idx_guarded(idx, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-BIT-CEIL",
                    message=f"{m.group('cont')}[{idx}] indexes with "
                    "bit_ceil/popcount without a range check",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_uncaught_exceptions(lines, rel, funcs, out) -> None:
    """uncaught_exceptions() as a boolean destructor-safety check (CWE-705).

    Requires uncaught_exceptions so exception_ptr / nested plants stay silent.
    The classic anti-pattern is if (std::uncaught_exceptions()) throw.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_UNCAUGHT, body):
            continue
        if _lit_search(_UNCAUGHT_SAVED, body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _UNCAUGHT_BOOL_IF.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-UNCAUGHT-EXCEPTIONS",
                message="std::uncaught_exceptions() used as a boolean "
                "destructor-safety check without a saved count",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            break  # one per function


def _cxx_ranges_join(lines, rel, funcs, out) -> None:
    """views::join / join_view indexed or iterated without a size guard.

    Requires views::join or join_view, not views::zip / zip_view.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_VIEWS_JOIN, body):
            continue
        names = {m.group("name") for m in _lit_finditer(_CXX_JOIN_AUTO, body)}
        names |= {m.group("name") for m in _lit_finditer(_CXX_JOIN_VIEW_DECL, body)}
        if not names:
            continue
        start = fn.span[0]
        body_lines = body.splitlines()
        for name in sorted(names):
            n = re.escape(name)
            for i, ln in enumerate(body_lines):
                sub = re.search(
                    rf"\b{n}\s*\[\s*(?P<idx>[^\]]+)\s*\]", ln
                )
                ranged = bool(re.search(rf"\bfor\s*\([^:]*:\s*{n}\b", ln))
                if not sub and not ranged:
                    continue
                if sub:
                    idx = sub.group("idx")
                    if _zip_idx_bound_checks(idx, body) >= 2:
                        continue
                elif ranged and (
                    len(_rx(r"\.\s*size\s*\(").findall(body)) >= 2
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-RANGES-JOIN",
                    message=f"{name} indexed/iterated without a size guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_quick_exit(lines, rel, funcs, out) -> None:
    """quick_exit without at_quick_exit, or from a destructor (CWE-670).

    Requires quick_exit so exit/abort/system/notify_all_at_thread_exit
    plants stay silent. at_quick_exit is a distinct token.
    """
    def _emit(fn_name: str, line: int, ln: str) -> None:
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=fn_name, line=line, cls="CXX-QUICK-EXIT",
            message="std::quick_exit without a prior at_quick_exit "
            "handler, or quick_exit from a destructor",
            strength=laws.STRENGTH_FINDS,
            evidence=lines[line - 1].strip()
            if 0 < line <= len(lines) else ln.strip(),
        ))

    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_QUICK_EXIT, body):
            continue
        is_dtor = fn.name.startswith("~")
        if _lit_search(_CXX_AT_QUICK_EXIT, body) and not is_dtor:
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_QUICK_EXIT.search(ln):
                continue
            _emit(fn.name, start + i, ln)
            return
    text = "\n".join(lines)
    for m in _lit_finditer(_DTOR_HEAD, text):
        brace = m.end() - 1
        close = _match_brace(text, brace)
        if close < 0:
            continue
        body = text[brace + 1: close]
        tm = _lit_search(_CXX_QUICK_EXIT, body)
        if not tm:
            continue
        line = text[: brace + 1 + tm.start()].count("\n") + 1
        _emit(f"~{m.group('name')}", line, body.splitlines()[0] if body else "")
        return


def _cxx_to_array(lines, rel, funcs, out) -> None:
    """to_array result OOB-indexed, or source mutated while used (CWE-125/416).

    Requires to_array so std::array / array_index plants stay silent.
    A constant index below the source C-array size is allowed.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_TO_ARRAY, body):
            continue
        sizes = {
            m.group("name"): int(m.group("n"))
            for m in _lit_finditer(_C_ARRAY_SIZE, body)
        }
        binds: dict[str, str] = {
            m.group("name"): m.group("src")
            for m in _lit_finditer(_TO_ARRAY_BIND, body)
        }
        start = fn.span[0]
        body_lines = body.splitlines()
        mutated_srcs: set[str] = set()
        for i, ln in enumerate(body_lines):
            for src in set(binds.values()):
                s = re.escape(src)
                if re.search(rf"\b{s}\s*\[", ln) and re.search(
                    rf"\b{s}\s*\[[^\]]+\]\s*=", ln
                ):
                    mutated_srcs.add(src)
                elif re.search(rf"\b{s}\s*=(?!=)", ln) and not _TO_ARRAY_BIND.search(ln):
                    mutated_srcs.add(src)
            inline = _TO_ARRAY_INLINE_IDX.search(ln)
            if inline and _toarr_idx_bad(inline.group("idx").strip(), None, body):
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-TO-ARRAY",
                    message="to_array result indexed without a size guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for m in _CXX_SUBSCRIPT.finditer(ln):
                name = m.group("cont")
                if name not in binds:
                    continue
                src = binds[name]
                idx = m.group("idx").strip()
                src_mutated = src in mutated_srcs
                if not src_mutated and not _toarr_idx_bad(
                    idx, sizes.get(src), body
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-TO-ARRAY",
                    message=f"{name} from to_array indexed without size "
                    "or after the source was mutated",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _toarr_idx_bad(idx: str, size: int | None, body: str) -> bool:
    if _rx(r"\d+").fullmatch(idx):
        k = int(idx)
        if size is not None:
            return k >= size
        return k >= 4
    return not _byteswap_idx_guarded(idx, body)


def _cxx_zoned_time(lines, rel, funcs, out) -> None:
    """zoned_time without locate_zone/current_zone null check (CWE-476).

    Requires zoned_time so tzdb / current_zone / chrono_seed plants stay
    silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_ZONED_TIME, body):
            continue
        if _lit_search(_CXX_ZONE_LOOKUP, body):
            names = {m.group("name") for m in _lit_finditer(_CXX_TZDB_BIND, body)}
            if names and all(
                _param_if_guard(n, body) or _param_null_tested(n, body)
                for n in names
            ):
                continue
            if not names and _rx(r"\bif\s*\(\s*!").search(body):
                continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_ZONED_TIME.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-ZONED-TIME",
                message="std::chrono::zoned_time constructed without "
                "a zone / locate_zone null check",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_kill_dependency(lines, rel, funcs, out) -> None:
    """kill_dependency as only sync, or as an unguarded index (CWE-362/125).

    Requires kill_dependency so POSIX kill / atomic_flag / fence plants
    stay silent.
    """
    globals_ = _file_scope_int_globals(lines)
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_KILLDEP, body):
            continue
        assigned: set[str] = set()
        start = fn.span[0]
        only_sync = False
        statics = set(
            _rx(r"\bstatic\s+int\s+([A-Za-z_]\w*)\b").findall(body)
        )
        stores = globals_ | statics
        has_plain_store = any(
            re.search(rf"\b{re.escape(g)}\s*=", body) for g in stores
        )
        has_real_sync = bool(_rx(r"\b(?:atomic\s*<|mutex|atomic_thread_fence|atomic_signal_fence"
            r"|memory_order)\b").search(body))
        if has_plain_store and not has_real_sync:
            only_sync = True
        for i, ln in enumerate(body.splitlines()):
            for m in _KILLDEP_ASSIGN.finditer(ln):
                assigned.add(m.group("var"))
            hit = bool(only_sync and _CXX_KILLDEP.search(ln))
            if _KILLDEP_IN_INDEX.search(ln):
                hit = True
            if not hit:
                for m in _CXX_SUBSCRIPT.finditer(ln):
                    idx = m.group("idx").strip()
                    if idx not in assigned:
                        continue
                    if _byteswap_idx_guarded(idx, body):
                        continue
                    hit = True
                    break
            if not hit:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-KILL-DEPENDENCY",
                message="std::kill_dependency used as only sync or as "
                "an index without a range check",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_rotl(lines, rel, funcs, out) -> None:
    """std::rotl/rotr result used as an array index without a range check.

    Requires rotl/rotr so byteswap / bit_ceil plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_ROTL_TOKEN, body):
            continue
        assigned: set[str] = set()
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _ROTL_ASSIGN.finditer(ln):
                assigned.add(m.group("var"))
            if _ROTL_IN_INDEX.search(ln):
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-ROTL",
                    message="std::rotl/rotr result used as an array index "
                    "without a range check",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for m in _CXX_SUBSCRIPT.finditer(ln):
                idx = m.group("idx").strip()
                if idx not in assigned:
                    continue
                if _byteswap_idx_guarded(idx, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-ROTL",
                    message=f"{m.group('cont')}[{idx}] indexes with "
                    "std::rotl/rotr without a range check",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_current_exception(lines, rel, funcs, out) -> None:
    """current_exception rethrown without a nullptr test (CWE-476).

    Requires current_exception so exception_ptr / nested / uncaught
    plants stay silent. rethrow_if_nested is not rethrow_exception.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_CURRENT_EXC, body):
            continue
        if not _lit_search(_CXX_RETHROW_EXC, body):
            continue
        bound = {m.group("name") for m in _lit_finditer(_CUR_EXC_BIND, body)}
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if _CUR_EXC_INLINE.search(ln):
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-CURRENT-EXCEPTION",
                    message="std::current_exception() rethrown without "
                    "a nullptr test",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for name in sorted(bound):
                if _param_if_guard(name, body) or _param_null_tested(name, body):
                    continue
                if not re.search(
                    rf"(?:std\s*::\s*)?rethrow_exception\s*\(\s*"
                    rf"{re.escape(name)}\b",
                    ln,
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-CURRENT-EXCEPTION",
                    message=f"rethrow_exception({name}) from "
                    "current_exception without a nullptr test",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_index_token_scan(
    lines, rel, funcs, out, *, token_re, in_index_re, assign_re,
    cls: str, message: str,
) -> None:
    """Token result used as an array index without a range check (CWE-125)."""
    for fn in funcs:
        body = fn.body or ""
        if not token_re.search(body):
            continue
        assigned: set[str] = set()
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in assign_re.finditer(ln):
                assigned.add(m.group("var"))
            if in_index_re.search(ln):
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls=cls,
                    message=message,
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for m in _CXX_SUBSCRIPT.finditer(ln):
                idx = m.group("idx").strip()
                if idx not in assigned:
                    continue
                if _byteswap_idx_guarded(idx, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls=cls,
                    message=f"{m.group('cont')}[{idx}] indexes with "
                    f"{cls} without a range check",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_bit_width(lines, rel, funcs, out) -> None:
    """std::bit_width result used as an array index (CWE-125).

    Requires bit_width so bit_ceil / rotl plants stay silent.
    """
    _cxx_index_token_scan(
        lines, rel, funcs, out,
        token_re=_CXX_BITWIDTH_TOKEN,
        in_index_re=_BITWIDTH_IN_INDEX,
        assign_re=_BITWIDTH_ASSIGN,
        cls="CXX-BIT-WIDTH",
        message="std::bit_width result used as an array index "
        "without a range check",
    )


def _cxx_lerp(lines, rel, funcs, out) -> None:
    """std::lerp as an index, or lerp with no finite bounds (CWE-125).

    Requires lerp so midpoint plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_LERP_TOKEN, body):
            continue
        assigned: set[str] = set()
        start = fn.span[0]
        fired = False
        for i, ln in enumerate(body.splitlines()):
            for m in _LERP_ASSIGN.finditer(ln):
                assigned.add(m.group("var"))
            hit = bool(_LERP_IN_INDEX.search(ln))
            if not hit:
                for m in _CXX_SUBSCRIPT.finditer(ln):
                    idx = m.group("idx").strip()
                    if idx not in assigned:
                        continue
                    if _byteswap_idx_guarded(idx, body):
                        continue
                    hit = True
                    break
            if not hit:
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-LERP",
                message="std::lerp result used as an array index "
                "without a range check",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            fired = True
            return
        if fired:
            return
        if _lit_search(_CXX_LERP_FINITE, body):
            continue
        if any(_byteswap_idx_guarded(v, body) for v in assigned):
            continue
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_LERP_TOKEN.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-LERP",
                message="std::lerp with no finite bounds",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_midpoint(lines, rel, funcs, out) -> None:
    """std::midpoint result used as an array index (CWE-125).

    Requires midpoint so lerp plants stay silent.
    """
    _cxx_index_token_scan(
        lines, rel, funcs, out,
        token_re=_CXX_MIDPOINT_TOKEN,
        in_index_re=_MIDPOINT_IN_INDEX,
        assign_re=_MIDPOINT_ASSIGN,
        cls="CXX-MIDPOINT",
        message="std::midpoint result used as an array index "
        "without a range check",
    )


def _cxx_cmp_less(lines, rel, funcs, out) -> None:
    """cmp_less/in_range as an index, or in_range ignored (CWE-125).

    Requires cmp_less or in_range so ordinary < / spaceship stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_CMPLESS_TOKEN, body):
            continue
        assigned: set[str] = set()
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CMPLESS_ASSIGN.finditer(ln):
                assigned.add(m.group("var"))
            if _CMPLESS_IN_INDEX.search(ln):
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-CMP-LESS",
                    message="std::cmp_less/in_range result used as an "
                    "array index",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for m in _CXX_SUBSCRIPT.finditer(ln):
                idx = m.group("idx").strip()
                if idx not in assigned:
                    continue
                if _byteswap_idx_guarded(idx, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-CMP-LESS",
                    message=f"{m.group('cont')}[{idx}] indexes with "
                    "cmp_less/in_range without a range check",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
        if _lit_search(_CXX_IN_RANGE_IF, body):
            continue
        if not _rx(r"\bin_range\s*(?:<[^>;]*>)?\s*\(").search(body):
            continue
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_SUBSCRIPT.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-CMP-LESS",
                message="std::in_range ignored before a subscript",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_countl_zero(lines, rel, funcs, out) -> None:
    """countl/countr zero/one result used as an array index (CWE-125).

    Requires those tokens so rotl / bit_ceil / byteswap plants stay silent.
    """
    _cxx_index_token_scan(
        lines, rel, funcs, out,
        token_re=_CXX_COUNTL_TOKEN,
        in_index_re=_COUNTL_IN_INDEX,
        assign_re=_COUNTL_ASSIGN,
        cls="CXX-COUNTL-ZERO",
        message="std::countl_zero/countr result used as an array index "
        "without a range check",
    )


def _cxx_unreachable(lines, rel, funcs, out) -> None:
    """std::unreachable() used as a recoverable path (CWE-670).

    Requires std::unreachable so __builtin_unreachable / [[assume]] stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_STD_UNREACHABLE, body):
            continue
        if not (_lit_search(_CXX_SUBSCRIPT, body) or _rx(r"\breturn\b").search(body)):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if not _CXX_STD_UNREACHABLE.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-UNREACHABLE",
                message="std::unreachable() used as a recoverable path "
                "or in a function that also returns",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_gcd(lines, rel, funcs, out) -> None:
    """std::gcd result used as an array index (CWE-125).

    Requires std::gcd so lcm / bit_width / rotl plants stay silent.
    """
    _cxx_index_token_scan(
        lines, rel, funcs, out,
        token_re=_CXX_GCD_TOKEN,
        in_index_re=_GCD_IN_INDEX,
        assign_re=_GCD_ASSIGN,
        cls="CXX-GCD",
        message="std::gcd result used as an array index "
        "without a range check",
    )


def _cxx_lcm(lines, rel, funcs, out) -> None:
    """std::lcm result used as an array index (CWE-125).

    Requires std::lcm so gcd plants stay silent.
    """
    _cxx_index_token_scan(
        lines, rel, funcs, out,
        token_re=_CXX_LCM_TOKEN,
        in_index_re=_LCM_IN_INDEX,
        assign_re=_LCM_ASSIGN,
        cls="CXX-LCM",
        message="std::lcm result used as an array index "
        "without a range check",
    )


def _cxx_clamp(lines, rel, funcs, out) -> None:
    """std::clamp result used as an array index (CWE-125).

    Requires std::clamp so midpoint / lerp plants stay silent.
    """
    _cxx_index_token_scan(
        lines, rel, funcs, out,
        token_re=_CXX_CLAMP_TOKEN,
        in_index_re=_CLAMP_IN_INDEX,
        assign_re=_CLAMP_ASSIGN,
        cls="CXX-CLAMP",
        message="std::clamp result used as an array index "
        "without a range check",
    )


def _cxx_exchange(lines, rel, funcs, out) -> None:
    """std::exchange result or object used as an unguarded index (CWE-672/125).

    Requires std::exchange so std::move / use-after-move plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_EXCHANGE_TOKEN, body):
            continue
        assigned: set[str] = set()
        exchanged: set[str] = set()
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _EXCHANGE_ASSIGN.finditer(ln):
                assigned.add(m.group("var"))
                exchanged.add(m.group("obj"))
            for m in _EXCHANGE_CALL.finditer(ln):
                exchanged.add(m.group("obj"))
            if _EXCHANGE_IN_INDEX.search(ln):
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-EXCHANGE",
                    message="std::exchange result used as an array index "
                    "without a range check",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for m in _CXX_SUBSCRIPT.finditer(ln):
                idx = m.group("idx").strip()
                if idx not in assigned and idx not in exchanged:
                    continue
                if _byteswap_idx_guarded(idx, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-EXCHANGE",
                    message=f"{m.group('cont')}[{idx}] indexes with "
                    "std::exchange without a range check",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_to_address(lines, rel, funcs, out) -> None:
    """to_address result used after source reset, or indexed OOB (CWE-416/125).

    Requires to_address so unique_reset / unique_release stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_TO_ADDRESS, body):
            continue
        sizes = {
            m.group("name"): int(m.group("n"))
            for m in _lit_finditer(_C_ARRAY_SIZE, body)
        }
        binds: dict[str, str] = {
            m.group("name"): m.group("src")
            for m in _lit_finditer(_TO_ADDR_BIND, body)
        }
        start = fn.span[0]
        body_lines = body.splitlines()
        reset_srcs: set[str] = set()
        for i, ln in enumerate(body_lines):
            for src in set(binds.values()):
                s = re.escape(src)
                if re.search(
                    rf"\b{s}\s*\.\s*(?:reset|release)\s*\(", ln
                ):
                    reset_srcs.add(src)
            inline = _TO_ADDR_INLINE_IDX.search(ln)
            if inline and _toarr_idx_bad(
                inline.group("idx").strip(), None, body
            ):
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-TO-ADDRESS",
                    message="to_address result indexed without a bound",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for m in _CXX_SUBSCRIPT.finditer(ln):
                name = m.group("cont")
                if name not in binds:
                    continue
                src = binds[name]
                idx = m.group("idx").strip()
                src_dead = src in reset_srcs
                if not src_dead and not _toarr_idx_bad(
                    idx, sizes.get(src), body
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-TO-ADDRESS",
                    message=f"{name} from to_address indexed without a "
                    "bound or after the source was reset",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for raw, src in binds.items():
                if src not in reset_srcs:
                    continue
                if not re.search(
                    rf"(?:\*\s*{re.escape(raw)}\b|{re.escape(raw)}\s*->)",
                    ln,
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-TO-ADDRESS",
                    message=f"{raw} from to_address used after {src} "
                    "was reset or released",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_is_constant_evaluated(lines, rel, funcs, out) -> None:
    """is_constant_evaluated false-path indexes without a bound (CWE-125).

    Requires is_constant_evaluated so assume / unreachable / constexpr
    plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_ICE_TOKEN, body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                idx = m.group("idx").strip()
                if _byteswap_idx_guarded(idx, body):
                    continue
                if _rx(r"\d+").fullmatch(idx) and int(idx) < 4:
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line,
                    cls="CXX-IS-CONSTANT-EVALUATED",
                    message="is_constant_evaluated() runtime path "
                    "indexes without a bound",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _addr_deref_guarded(var: str, body: str) -> bool:
    v = re.escape(var)
    if re.search(rf"\*\s*{v}\s*(?:>=|>|<|<=)\s*\d+", body):
        return True
    return bool(re.search(rf"\d+\s*(?:>=|>|<|<=)\s*\*\s*{v}\b", body))


def _cxx_addressof(lines, rel, funcs, out) -> None:
    """addressof result used as an index, or stored then the object dies.

    Requires `addressof` (word-bounded) so to_address plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_ADDRESSOF, body):
            continue
        binds = {
            m.group("name"): m.group("src")
            for m in _lit_finditer(_ADDRESSOF_BIND, body)
        }
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if _ADDRESSOF_IN_INDEX.search(ln) or _ADDRESSOF_INLINE_RET.search(ln):
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-ADDRESSOF",
                    message="std::addressof result used as an array "
                    "index or stored then the object dies",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for m in _CXX_SUBSCRIPT.finditer(ln):
                idx = m.group("idx").strip()
                star = _rx(r"\*\s*([A-Za-z_]\w*)").fullmatch(idx)
                if star and star.group(1) in binds:
                    if _addr_deref_guarded(star.group(1), body):
                        continue
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="CXX-ADDRESSOF",
                        message="std::addressof result used as an array "
                        "index without a bound",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else ln.strip(),
                    ))
                    return
                if idx in binds and not _byteswap_idx_guarded(idx, body):
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="CXX-ADDRESSOF",
                        message="std::addressof result used as an array "
                        "index without a bound",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else ln.strip(),
                    ))
                    return
            for name, src in binds.items():
                if not re.search(rf"\breturn\s+{re.escape(name)}\b", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-ADDRESSOF",
                    message=f"{name} from addressof({src}) returned "
                    "after the object dies",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_assume_aligned(lines, rel, funcs, out) -> None:
    """assume_aligned result indexed without a bound (CWE-125).

    Requires assume_aligned so [[assume]] / unreachable / ice plants
    stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_ASSUME_ALIGNED, body):
            continue
        binds = {m.group("name") for m in _lit_finditer(_ASMALIGN_BIND, body)}
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            inline = _ASMALIGN_INLINE_IDX.search(ln)
            if inline and _toarr_idx_bad(
                inline.group("idx").strip(), None, body
            ):
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-ASSUME-ALIGNED",
                    message="assume_aligned result indexed without a bound",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for m in _CXX_SUBSCRIPT.finditer(ln):
                if m.group("cont") not in binds:
                    continue
                if not _toarr_idx_bad(m.group("idx").strip(), None, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-ASSUME-ALIGNED",
                    message="assume_aligned result indexed without a bound",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_as_const(lines, rel, funcs, out) -> None:
    """as_const then const_cast write, or dangling as_const view (CWE-119/416).

    Requires as_const so const_cast / cxx_cv_write plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_AS_CONST, body):
            continue
        binds = {
            m.group("name"): m.group("src")
            for m in _lit_finditer(_AS_CONST_BIND, body)
        }
        ret = fn.return_type or ""
        ref_ret = "*" in ret or "&" in ret
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            cm = _AS_CONST_CAST_WRITE.search(ln)
            if cm and (not binds or cm.group("name") in binds):
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-AS-CONST",
                    message="const_cast write through std::as_const view",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            if not ref_ret:
                continue
            if _rx(r"\breturn\s+(?:std\s*::\s*)?\bas_const\s*\(").search(ln):
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-AS-CONST",
                    message="as_const view of a local returned after "
                    "the local dies",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for name, src in binds.items():
                if not re.search(rf"\breturn\s+{re.escape(name)}\b", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-AS-CONST",
                    message=f"as_const view {name} of {src} returned "
                    "after the local dies",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_exclusive_scan(lines, rel, funcs, out) -> None:
    """exclusive_scan dest OOB-indexed or overlapping source (CWE-125).

    Requires exclusive_scan so other algorithm plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_EXCLUSIVE_SCAN, body):
            continue
        dests: set[str] = set()
        overlap: set[str] = set()
        for m in _lit_finditer(_EXSCAN_CALL, body):
            src, dst = m.group("src"), m.group("dst")
            dests.add(dst)
            if src == dst:
                overlap.add(dst)
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if _CXX_EXCLUSIVE_SCAN.search(ln) and overlap:
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-EXCLUSIVE-SCAN",
                    message="exclusive_scan dest overlaps source",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for m in _CXX_SUBSCRIPT.finditer(ln):
                if m.group("cont") not in dests:
                    continue
                if not _toarr_idx_bad(m.group("idx").strip(), None, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-EXCLUSIVE-SCAN",
                    message="exclusive_scan output indexed without a "
                    "size guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_make_exception_ptr(lines, rel, funcs, out) -> None:
    """make_exception_ptr rethrown without a nullptr test (CWE-476).

    Requires make_exception_ptr so current_exception / exception_ptr /
    nested plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_MAKE_EPTR, body):
            continue
        if not _lit_search(_CXX_RETHROW_EXC, body):
            continue
        bound = {m.group("name") for m in _lit_finditer(_MAKE_EPTR_BIND, body)}
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if _MAKE_EPTR_INLINE.search(ln):
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line,
                    cls="CXX-MAKE-EXCEPTION-PTR",
                    message="std::make_exception_ptr() rethrown without "
                    "a nullptr test",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for name in sorted(bound):
                if _param_if_guard(name, body) or _param_null_tested(name, body):
                    continue
                if not re.search(
                    rf"(?:std\s*::\s*)?rethrow_exception\s*\(\s*"
                    rf"{re.escape(name)}\b",
                    ln,
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line,
                    cls="CXX-MAKE-EXCEPTION-PTR",
                    message=f"rethrow_exception({name}) from "
                    "make_exception_ptr without a nullptr test",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_set_terminate(lines, rel, funcs, out) -> None:
    """set_terminate without saving the previous handler, or handler throws.

    Requires set_terminate or get_terminate so unreachable / assume /
    uncaught plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_SET_TERMINATE, body):
            continue
        stored = bool(_lit_search(_GET_TERM_BIND, body))
        throws = bool(_rx(r"\bthrow\b").search(body))
        has_set = bool(_lit_search(_SET_TERM_CALL, body))
        if has_set:
            if not throws and stored:
                continue
        elif stored:
            continue
        start = fn.span[0]
        tok = _SET_TERM_CALL if has_set else _CXX_SET_TERMINATE
        for i, ln in enumerate(body.splitlines()):
            if not tok.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CXX-SET-TERMINATE",
                message="set_terminate/get_terminate without storing "
                "the previous handler, or terminate handler throws",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))
            return


def _cxx_inclusive_scan(lines, rel, funcs, out) -> None:
    """inclusive_scan dest OOB-indexed or overlapping source (CWE-125).

    Requires inclusive_scan so exclusive_scan plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_INCLUSIVE_SCAN, body):
            continue
        dests: set[str] = set()
        overlap: set[str] = set()
        for m in _lit_finditer(_INSCAN_CALL, body):
            src, dst = m.group("src"), m.group("dst")
            dests.add(dst)
            if src == dst:
                overlap.add(dst)
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if _CXX_INCLUSIVE_SCAN.search(ln) and overlap:
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-INCLUSIVE-SCAN",
                    message="inclusive_scan dest overlaps source",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for m in _CXX_SUBSCRIPT.finditer(ln):
                if m.group("cont") not in dests:
                    continue
                if not _toarr_idx_bad(m.group("idx").strip(), None, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-INCLUSIVE-SCAN",
                    message="inclusive_scan output indexed without a "
                    "size guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_transform_reduce(lines, rel, funcs, out) -> None:
    """std::transform_reduce result used as an array index (CWE-125).

    Requires transform_reduce so std::reduce plants stay silent.
    """
    _cxx_index_token_scan(
        lines, rel, funcs, out,
        token_re=_CXX_TRED_TOKEN,
        in_index_re=_TRED_IN_INDEX,
        assign_re=_TRED_ASSIGN,
        cls="CXX-TRANSFORM-REDUCE",
        message="std::transform_reduce result used as an array index "
        "without a range check",
    )


def _cxx_reduce(lines, rel, funcs, out) -> None:
    """std::reduce result used as an array index (CWE-125).

    Requires std::reduce (not transform_reduce) so transform_reduce
    plants stay silent.
    """
    scoped = []
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_REDUCE_TOKEN, body):
            continue
        if _lit_search(_CXX_TRANSFORM_REDUCE, body):
            continue
        scoped.append(fn)
    _cxx_index_token_scan(
        lines, rel, scoped, out,
        token_re=_CXX_REDUCE_TOKEN,
        in_index_re=_REDUCE_IN_INDEX,
        assign_re=_REDUCE_ASSIGN,
        cls="CXX-REDUCE",
        message="std::reduce result used as an array index "
        "without a range check",
    )


def _cxx_uninitialized_copy(lines, rel, funcs, out) -> None:
    """uninitialized_copy/move dest OOB-indexed or overlapping (CWE-125/824).

    Requires uninitialized_copy or uninitialized_move so other algorithm
    plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_UNINITIALIZED_COPY, body):
            continue
        dests: set[str] = set()
        overlap: set[str] = set()
        for m in _lit_finditer(_UICOPY_CALL, body):
            src, dst = m.group("src"), m.group("dst")
            dests.add(dst)
            if src == dst:
                overlap.add(dst)
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            if _CXX_UNINITIALIZED_COPY.search(ln) and overlap:
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line,
                    cls="CXX-UNINITIALIZED-COPY",
                    message="uninitialized_copy/move dest overlaps source",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for m in _CXX_SUBSCRIPT.finditer(ln):
                if m.group("cont") not in dests:
                    continue
                if not _toarr_idx_bad(m.group("idx").strip(), None, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line,
                    cls="CXX-UNINITIALIZED-COPY",
                    message="uninitialized_copy/move dest indexed "
                    "without a size guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_construct_at(lines, rel, funcs, out) -> None:
    """destroy_at then use, or construct_at dest indexed OOB (CWE-416/125).

    Requires construct_at or destroy_at so to_address / addressof plants
    stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_CONSTRUCT_AT, body):
            continue
        dests: set[str] = set()
        destroyed: set[str] = set()
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CONSTRUCT_AT_CALL.finditer(ln):
                ptr = m.group("ptr")
                dests.add(ptr)
                destroyed.discard(ptr)
            for m in _DESTROY_AT_CALL.finditer(ln):
                destroyed.add(m.group("ptr"))
            if not _DESTROY_AT_CALL.search(ln):
                for ptr in sorted(destroyed):
                    v = re.escape(ptr)
                    if not re.search(
                        rf"(?:\*\s*{v}\b|{v}\s*\[|{v}\s*->)",
                        ln,
                    ):
                        continue
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line,
                        cls="CXX-CONSTRUCT-AT",
                        message=f"{ptr} used after destroy_at",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else ln.strip(),
                    ))
                    return
            for m in _CXX_SUBSCRIPT.finditer(ln):
                if m.group("cont") not in dests:
                    continue
                if not _toarr_idx_bad(m.group("idx").strip(), None, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-CONSTRUCT-AT",
                    message="construct_at result indexed without a "
                    "size guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_forward_like(lines, rel, funcs, out) -> None:
    """forward_like result used as an unguarded index, or source reused (CWE-672).

    Requires forward_like so std::forward / std::move / use-after-move
    plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_FWDLIKE_TOKEN, body):
            continue
        assigned: set[str] = set()
        sources: set[str] = set()
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _FWDLIKE_ASSIGN.finditer(ln):
                assigned.add(m.group("var"))
            for m in _FWDLIKE_SRC.finditer(ln):
                sources.add(m.group("src"))
            if _FWDLIKE_IN_INDEX.search(ln):
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-FORWARD-LIKE",
                    message="std::forward_like result used as an array "
                    "index without a range check",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            if _CXX_FWDLIKE_TOKEN.search(ln):
                continue
            for m in _CXX_SUBSCRIPT.finditer(ln):
                idx = m.group("idx").strip()
                if idx not in assigned and idx not in sources:
                    continue
                if _byteswap_idx_guarded(idx, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-FORWARD-LIKE",
                    message=f"{m.group('cont')}[{idx}] indexes with "
                    "std::forward_like without a range check",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for src in sorted(sources):
                if _byteswap_idx_guarded(src, body):
                    continue
                v = re.escape(src)
                if not re.search(rf"\b{v}\b", ln):
                    continue
                if re.search(rf"\b(?:int|auto|auto\s*&&?)\s+{v}\b", ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-FORWARD-LIKE",
                    message=f"{src} used after std::forward_like",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_uninitialized_fill(lines, rel, funcs, out) -> None:
    """uninitialized_fill/default_construct dest indexed OOB (CWE-125).

    Requires uninitialized_fill or uninitialized_default_construct so
    uninitialized_copy plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_UNINITIALIZED_FILL, body):
            continue
        dests = {m.group("dst") for m in _lit_finditer(_UIFILL_CALL, body)}
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                if m.group("cont") not in dests:
                    continue
                if not _toarr_idx_bad(m.group("idx").strip(), None, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line,
                    cls="CXX-UNINITIALIZED-FILL",
                    message="uninitialized_fill dest indexed without a "
                    "size guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_destroy_n(lines, rel, funcs, out) -> None:
    """destroy_n then use, or count is unchecked (CWE-416).

    Requires destroy_n so construct_at / destroy_at plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_DESTROY_N, body):
            continue
        destroyed: set[str] = set()
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _DESTROY_N_CALL.finditer(ln):
                ptr = m.group("ptr")
                n = m.group("n").strip()
                destroyed.add(ptr)
                if not _rx(r"\d+").fullmatch(n) and not _byteswap_idx_guarded(
                    n, body
                ):
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="CXX-DESTROY-N",
                        message="destroy_n count is unchecked",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else ln.strip(),
                    ))
                    return
            if not _DESTROY_N_CALL.search(ln):
                for ptr in sorted(destroyed):
                    v = re.escape(ptr)
                    if not re.search(
                        rf"(?:\*\s*{v}\b|{v}\s*\[|{v}\s*->)",
                        ln,
                    ):
                        continue
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="CXX-DESTROY-N",
                        message=f"{ptr} used after destroy_n",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else ln.strip(),
                    ))
                    return


def _cxx_add_sat(lines, rel, funcs, out) -> None:
    """add_sat/sub_sat/mul_sat/div_sat/saturate_cast as an index (CWE-125).

    Requires those tokens so C add-overflow plants stay silent.
    """
    _cxx_index_token_scan(
        lines, rel, funcs, out,
        token_re=_CXX_ADDSAT_TOKEN,
        in_index_re=_ADDSAT_IN_INDEX,
        assign_re=_ADDSAT_ASSIGN,
        cls="CXX-ADD-SAT",
        message="std::add_sat result used as an array index "
        "without a range check",
    )


def _cxx_transform_scan(lines, rel, funcs, out) -> None:
    """transform_*_scan dest indexed without a size guard (CWE-125).

    Requires transform_inclusive_scan or transform_exclusive_scan so
    inclusive_scan / exclusive_scan plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_TRANSFORM_SCAN, body):
            continue
        dests: set[str] = set()
        for m in _lit_finditer(_TSCAN_CALL, body):
            dests.add(m.group("dst"))
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                if m.group("cont") not in dests:
                    continue
                if not _toarr_idx_bad(m.group("idx").strip(), None, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-TRANSFORM-SCAN",
                    message="transform_inclusive_scan/transform_exclusive_scan "
                    "output indexed without a size guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_type_identity(lines, rel, funcs, out) -> None:
    """type_identity recast pointer indexed OOB (CWE-125).

    Requires type_identity so typeid plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_TYPE_IDENTITY, body):
            continue
        dests = {
            m.group("ptr") for m in _lit_finditer(_TYPEIDENT_DIRECT_PTR, body)
        }
        aliases = {m.group("alias") for m in _lit_finditer(_TYPEIDENT_ALIAS, body)}
        if aliases:
            al = "|".join(re.escape(a) for a in sorted(aliases))
            for m in re.finditer(
                rf"\b(?:{al})\s*\*\s*(?P<ptr>[A-Za-z_]\w*)",
                body,
            ):
                dests.add(m.group("ptr"))
        if not dests:
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                if m.group("cont") not in dests:
                    continue
                if not _toarr_idx_bad(m.group("idx").strip(), None, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-TYPE-IDENTITY",
                    message="type_identity recast pointer indexed "
                    "without a size guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_nontype(lines, rel, funcs, out) -> None:
    """nontype used as an unguarded index or to bypass a bound (CWE-125).

    Requires nontype.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_NONTYPE, body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                idx = m.group("idx").strip()
                if not _toarr_idx_bad(idx, None, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-NONTYPE",
                    message="std::nontype used as an unguarded index "
                    "or to bypass a bound",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_layout_compatible(lines, rel, funcs, out) -> None:
    """is_layout_compatible recast then indexed OOB (CWE-125).

    Requires is_layout_compatible so pointer-interconvertible plants stay
    silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_LAYOUT_COMPATIBLE, body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                if not _toarr_idx_bad(m.group("idx").strip(), None, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line,
                    cls="CXX-LAYOUT-COMPATIBLE",
                    message="is_layout_compatible recast pointer indexed "
                    "without a size guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_ptr_interconvertible(lines, rel, funcs, out) -> None:
    """pointer-interconvertible recast then indexed OOB (CWE-125).

    Requires is_pointer_interconvertible_with_class /
    is_pointer_interconvertible_base_of so is_layout_compatible plants
    stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_PTR_INTERCONV, body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                if not _toarr_idx_bad(m.group("idx").strip(), None, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line,
                    cls="CXX-PTR-INTERCONVERTIBLE",
                    message="pointer-interconvertible recast pointer "
                    "indexed without a size guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_uninitialized_value(lines, rel, funcs, out) -> None:
    """uninitialized_value_construct dest indexed OOB (CWE-125).

    Requires uninitialized_value_construct so uninitialized_fill /
    uninitialized_copy plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_UNINITIALIZED_VALUE, body):
            continue
        dests = {m.group("dst") for m in _lit_finditer(_UVALUE_CALL, body)}
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                if m.group("cont") not in dests:
                    continue
                if not _toarr_idx_bad(m.group("idx").strip(), None, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line,
                    cls="CXX-UNINITIALIZED-VALUE",
                    message="uninitialized_value_construct dest indexed "
                    "without a size guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_const_iterator(lines, rel, funcs, out) -> None:
    """basic_const_iterator written through or indexed OOB (CWE-125/664).

    Requires basic_const_iterator so iterator-invalid / counted_iterator
    plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_BASIC_CONST_ITER, body):
            continue
        names = {m.group("name") for m in _lit_finditer(_BCITER_DECL, body)}
        names |= {m.group("name") for m in _lit_finditer(_BCITER_BIND, body)}
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                if names and m.group("cont") not in names:
                    continue
                if not _toarr_idx_bad(m.group("idx").strip(), None, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-CONST-ITERATOR",
                    message="basic_const_iterator indexed without a "
                    "size guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return
            for name in sorted(names):
                v = re.escape(name)
                if not re.search(
                    rf"(?:\*\s*{v}\b|{v}\s*->)\s*=",
                    ln,
                ):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-CONST-ITERATOR",
                    message=f"{name} written through a basic_const_iterator",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_corresponding_member(lines, rel, funcs, out) -> None:
    """is_corresponding_member recast then indexed OOB (CWE-125).

    Requires is_corresponding_member so is_layout_compatible plants stay
    silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_CORR_MEMBER, body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                if not _toarr_idx_bad(m.group("idx").strip(), None, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line,
                    cls="CXX-CORRESPONDING-MEMBER",
                    message="is_corresponding_member recast pointer "
                    "indexed without a size guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_ranges_to(lines, rel, funcs, out) -> None:
    """ranges::to container indexed without a size guard (CWE-125).

    Requires ranges::to so to_array / to_chars / to_address / addressof
    plants stay silent.
    """
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(_CXX_RANGES_TO, body):
            continue
        dests = {m.group("name") for m in _lit_finditer(_RANGES_TO_BIND, body)}
        if not dests:
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                if m.group("cont") not in dests:
                    continue
                if not _toarr_idx_bad(m.group("idx").strip(), None, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="CXX-RANGES-TO",
                    message="ranges::to container indexed without a "
                    "size guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_token_subscript_unguarded(
    lines, rel, funcs, out, token_re, cls: str, message: str,
) -> None:
    for fn in funcs:
        body = fn.body or ""
        if not _lit_search(token_re, body):
            continue
        start = fn.span[0]
        for i, ln in enumerate(body.splitlines()):
            for m in _CXX_SUBSCRIPT.finditer(ln):
                if not _toarr_idx_bad(m.group("idx").strip(), None, body):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls=cls,
                    message=message,
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                return


def _cxx_enumerate(lines, rel, funcs, out) -> None:
    """views::enumerate / enumerate_view indexed without a size guard (CWE-125).

    Requires views::enumerate or enumerate_view, not the enum keyword or zip.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_ENUMERATE, "CXX-ENUMERATE",
        "enumerate view indexed without a size guard",
    )


def _cxx_cartesian_product(lines, rel, funcs, out) -> None:
    """views::cartesian_product indexed without a size guard (CWE-125).

    Requires cartesian_product so zip plants stay silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_CARTESIAN, "CXX-CARTESIAN-PRODUCT",
        "cartesian_product view indexed without a size guard",
    )


def _cxx_chunk(lines, rel, funcs, out) -> None:
    """views::chunk / chunk_by / chunk_view indexed without a size guard (CWE-125).

    Requires views::chunk, chunk_by, or chunk_view so ranges_dangle stays silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_CHUNK, "CXX-CHUNK",
        "chunk view indexed without a size guard",
    )


def _cxx_slide(lines, rel, funcs, out) -> None:
    """views::slide / slide_view indexed without a size guard (CWE-125).

    Requires views::slide or slide_view.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_SLIDE, "CXX-SLIDE",
        "slide view indexed without a size guard",
    )


def _cxx_adjacent(lines, rel, funcs, out) -> None:
    """views::adjacent / adjacent_transform / adjacent_view unguarded index (CWE-125).

    Requires those tokens so adjtime_api.c (C, no views::adjacent) stays silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_ADJACENT, "CXX-ADJACENT",
        "adjacent view indexed without a size guard",
    )


def _cxx_join_with(lines, rel, funcs, out) -> None:
    """views::join_with / join_with_view indexed without a size guard (CWE-125).

    Requires join_with so views::join / join_view plants stay silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_JOIN_WITH, "CXX-JOIN-WITH",
        "join_with view indexed without a size guard",
    )


def _cxx_zip_transform(lines, rel, funcs, out) -> None:
    """views::zip_transform / zip_transform_view unguarded index (CWE-125).

    Requires zip_transform so views::zip / zip_view plants stay silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_ZIP_TRANSFORM, "CXX-ZIP-TRANSFORM",
        "zip_transform view indexed without a size guard",
    )


def _cxx_as_rvalue(lines, rel, funcs, out) -> None:
    """views::as_rvalue / as_rvalue_view indexed OOB (CWE-125/664).

    Requires as_rvalue so as_const plants stay silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_AS_RVALUE, "CXX-AS-RVALUE",
        "as_rvalue view indexed without a size guard",
    )


def _cxx_from_range(lines, rel, funcs, out) -> None:
    """std::from_range container indexed without a size guard (CWE-125).

    Requires from_range so ranges::to plants stay silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_FROM_RANGE, "CXX-FROM-RANGE",
        "from_range container indexed without a size guard",
    )


def _cxx_scoped_enum(lines, rel, funcs, out) -> None:
    """is_scoped_enum used to recast then indexed OOB (CWE-125).

    Requires is_scoped_enum so enum / enumerate plants stay silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_SCOPED_ENUM, "CXX-SCOPED-ENUM",
        "is_scoped_enum recast indexed without a size guard",
    )


def _cxx_stride(lines, rel, funcs, out) -> None:
    """views::stride / stride_view indexed without a size guard (CWE-125).

    Requires views::stride or stride_view so chunk / slide plants stay silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_STRIDE, "CXX-STRIDE",
        "stride view indexed without a size guard",
    )


def _cxx_repeat(lines, rel, funcs, out) -> None:
    """views::repeat / repeat_view indexed without a size guard (CWE-125).

    Requires views::repeat or repeat_view, not the English word repeat.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_REPEAT, "CXX-REPEAT",
        "repeat view indexed without a size guard",
    )


def _cxx_take(lines, rel, funcs, out) -> None:
    """views::take / take_view indexed without a size guard (CWE-125).

    Requires views::take or take_view, not take_while / chunk / slide / repeat.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_TAKE, "CXX-TAKE",
        "take view indexed without a size guard",
    )


def _cxx_drop(lines, rel, funcs, out) -> None:
    """views::drop / drop_view indexed without a size guard (CWE-125).

    Requires views::drop or drop_view, not drop_while / chunk / slide / repeat.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_DROP, "CXX-DROP",
        "drop view indexed without a size guard",
    )


def _cxx_filter(lines, rel, funcs, out) -> None:
    """views::filter / filter_view indexed without a size guard (CWE-125).

    Requires views::filter or filter_view so take / ranges_dangle stay silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_FILTER, "CXX-FILTER",
        "filter view indexed without a size guard",
    )


def _cxx_transform_view(lines, rel, funcs, out) -> None:
    """views::transform / transform_view indexed without a size guard (CWE-125).

    Requires views::transform or transform_view so transform_reduce /
    transform_*_scan / zip_transform plants stay silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_TRANSFORM_VIEW, "CXX-TRANSFORM-VIEW",
        "transform view indexed without a size guard",
    )


def _cxx_elements(lines, rel, funcs, out) -> None:
    """views::elements / elements_view indexed without a size guard (CWE-125).

    Requires views::elements or elements_view so ranges::to plants stay silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_ELEMENTS, "CXX-ELEMENTS",
        "elements view indexed without a size guard",
    )


def _cxx_iota(lines, rel, funcs, out) -> None:
    """views::iota / iota_view indexed without a size guard (CWE-125).

    Requires views::iota or iota_view so repeat plants stay silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_IOTA, "CXX-IOTA",
        "iota view indexed without a size guard",
    )


def _cxx_take_while(lines, rel, funcs, out) -> None:
    """views::take_while / take_while_view indexed without a size guard (CWE-125).

    Requires take_while so views::take / take_view plants stay silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_TAKE_WHILE, "CXX-TAKE-WHILE",
        "take_while view indexed without a size guard",
    )


def _cxx_drop_while(lines, rel, funcs, out) -> None:
    """views::drop_while / drop_while_view indexed without a size guard (CWE-125).

    Requires drop_while so views::drop / drop_view plants stay silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_DROP_WHILE, "CXX-DROP-WHILE",
        "drop_while view indexed without a size guard",
    )


def _cxx_keys(lines, rel, funcs, out) -> None:
    """views::keys / keys_view indexed without a size guard (CWE-125).

    Requires views::keys or keys_view so map / map_at / flat_map stay silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_KEYS, "CXX-KEYS",
        "keys view indexed without a size guard",
    )


def _cxx_values(lines, rel, funcs, out) -> None:
    """views::values / values_view indexed without a size guard (CWE-125).

    Requires views::values or values_view so map plants stay silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_VALUES, "CXX-VALUES",
        "values view indexed without a size guard",
    )


def _cxx_reverse_view(lines, rel, funcs, out) -> None:
    """views::reverse / reverse_view indexed without a size guard (CWE-125).

    Requires views::reverse or reverse_view so std::reverse /
    std::ranges::reverse plants stay silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_REVERSE_VIEW, "CXX-REVERSE-VIEW",
        "reverse view indexed without a size guard",
    )


def _cxx_counted(lines, rel, funcs, out) -> None:
    """views::counted / counted_view indexed without a size guard (CWE-125).

    Requires views::counted or counted_view so counted_iterator stays silent.
    """
    _cxx_token_subscript_unguarded(
        lines, rel, funcs, out, _CXX_COUNTED, "CXX-COUNTED",
        "counted view indexed without a size guard",
    )


def _is_ref_or_ptr_type(typ: str) -> bool:
    t = _rx(r"\b(?:const|volatile|restrict)\b").sub("", typ)
    return "*" in t or "&" in t or "[" in t


def _first_ref_ptr_param(fn) -> str | None:
    for typ, name in fn.params:
        if name and _is_ref_or_ptr_type(typ):
            return name
    return None


def _copy_from_param_line(param: str, ln: str) -> bool:
    p = re.escape(param)
    return bool(re.search(rf"\b{p}\s*\.", ln)) or bool(
        re.search(rf"\b{p}\s*->", ln)
    )


def _has_self_assign_guard(self_name: str, body: str) -> bool:
    sn = re.escape(self_name)
    if re.search(rf"&\s*{sn}\s*==", body):
        return True
    if re.search(rf"==\s*&\s*{sn}\b", body):
        return True
    if _rx(r"\bthis\s*==\s*&").search(body):
        return True
    if _rx(r"&\s*\w+\s*==\s*this\b").search(body):
        return True
    if _rx(r"\bthis\s*!=").search(body):
        return True
    return False


def _cxx_exception_leak(lines, rel, funcs, out) -> None:
    """new assigned to local, then a call before delete of that pointer."""
    for fn in funcs:
        body_lines = fn.body.splitlines()
        reported: set[str] = set()
        for i, ln in enumerate(body_lines):
            m = _NEW_ASSIGN.search(ln)
            if not m:
                continue
            var = m.group("var")
            if var in reported:
                continue
            if _rx(r"\b(?:unique_ptr|shared_ptr)\b").search(ln):
                continue
            for j in range(i + 1, len(body_lines)):
                later = body_lines[j]
                if re.search(
                    rf"\bdelete\s*(?:\[\s*\])?\s*{re.escape(var)}\b", later
                ):
                    break
                if _rx(r"\b(?:unique_ptr|shared_ptr)\b").search(later):
                    break
                for call_m in _CXX_CALL.finditer(later):
                    callee = call_m.group(1)
                    if callee in _KW or callee == "delete":
                        continue
                    reported.add(var)
                    line = fn.span[0] + j
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="CXX-EXCEPTION-LEAK",
                        message=f"{var} allocated with new but call precedes "
                        f"delete; may leak on throw",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else "",
                    ))
                    break
                if var in reported:
                    break


def _cxx_self_assign(lines, rel, funcs, out) -> None:
    """delete then copy from another object without a self-assignment guard."""
    for fn in funcs:
        body_lines = fn.body.splitlines()
        self_param = _first_ref_ptr_param(fn)
        if not self_param:
            continue
        copy_sources = [
            name for typ, name in fn.params
            if name and name != self_param
            and any(_copy_from_param_line(name, ln) for ln in body_lines)
        ]
        if not copy_sources:
            continue
        if _has_self_assign_guard(self_param, fn.body):
            continue
        delete_i = next(
            (i for i, ln in enumerate(body_lines) if _DELETE.search(ln)),
            None,
        )
        if delete_i is None:
            continue
        copy_i = next(
            (
                i for i, ln in enumerate(body_lines[delete_i + 1:], delete_i + 1)
                if any(_copy_from_param_line(p, ln) for p in copy_sources)
            ),
            None,
        )
        if copy_i is None:
            continue
        line = fn.span[0] + delete_i
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=fn.name, line=line, cls="CXX-SELF-ASSIGN",
            message="delete before copy from other without self-assignment "
            "guard",
            strength=laws.STRENGTH_FINDS,
            evidence=lines[line - 1].strip()
            if 0 < line <= len(lines) else "",
        ))


def _mismatched_free(lines, rel, funcs, out) -> None:
    """malloc/calloc/realloc paired with delete, or new paired with free."""
    for fn in funcs:
        if fn.kind == "POINTER":
            continue
        body_lines = fn.body.splitlines()
        alloc_family: dict[str, str] = {}
        for ln in body_lines:
            m = _C_ALLOC_ASSIGN.search(ln)
            if m:
                alloc_family[m.group("var")] = "c"
            m = _NEW_ASSIGN.search(ln)
            if m:
                alloc_family[m.group("var")] = "cpp"
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(body_lines):
            m = _DELETE.search(ln)
            if m:
                var = m.group("var")
                if alloc_family.get(var) != "c":
                    continue
                key = (var, i)
                if key in seen:
                    continue
                seen.add(key)
                line = fn.span[0] + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="MEM-MISMATCHED-FREE",
                    message=f"{var} allocated with malloc family but freed "
                    f"with delete",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))
            m = FREE_CALL.search(ln)
            if not m:
                continue
            var = _norm_var(m.group("var"))
            if alloc_family.get(var) != "cpp":
                continue
            key = (var, i)
            if key in seen:
                continue
            seen.add(key)
            line = fn.span[0] + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="MEM-MISMATCHED-FREE",
                message=f"{var} allocated with new but freed with free",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))


def _fd_leak(lines, rel, funcs, out) -> None:
    """Local fopen/open closed on some returns but not others."""
    for fn in funcs:
        body_lines = fn.body.splitlines()
        opens: dict[str, list[int]] = {}
        closes: dict[str, list[int]] = {}
        for i, ln in enumerate(body_lines):
            for m in _FD_OPEN.finditer(ln):
                opens.setdefault(m.group("var"), []).append(i)
            for m in _FD_CLOSE.finditer(ln):
                closes.setdefault(m.group("var"), []).append(i)
        returns = [
            i for i, ln in enumerate(body_lines)
            if _rx(r"\s*return\b").match(ln)
        ]
        if len(returns) < 2:
            continue
        for var, open_hits in opens.items():
            close_hits = closes.get(var, [])
            if not close_hits:
                continue
            # `if (fd < 0) return -1;` leaves with nothing open.
            var_returns = [
                r for r in returns
                if not _null_guarded_return(var, body_lines, r)
            ]
            if len(var_returns) < 2:
                continue
            held_returns = []
            for r in var_returns:
                last_open = max((h for h in open_hits if h < r), default=-1)
                last_close = max((h for h in close_hits if h < r), default=-1)
                if last_open > last_close:
                    held_returns.append(r)
            if held_returns and len(held_returns) < len(var_returns):
                ln = fn.span[0] + held_returns[0]
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=ln, cls="RES-FD-LEAK",
                    message=f"{var} open on {len(open_hits)} path(s) but closed "
                    f"before only {len(var_returns) - len(held_returns)} of "
                    f"{len(var_returns)} returns",
                    strength=laws.STRENGTH_FINDS,
                    evidence=body_lines[held_returns[0]].strip(),
                ))


_POPEN_OPEN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?popen\s*\(",
)
_POPEN_CLOSE = re.compile(
    r"\bpclose\s*\(\s*(?P<var>[A-Za-z_]\w*)\s*\)",
)


def _popen_leak(lines, rel, funcs, out) -> None:
    """popen() stream not pclose()'d before every return or function end."""
    for fn in funcs:
        body_lines = fn.body.splitlines()
        opens: dict[str, list[int]] = {}
        closes: dict[str, list[int]] = {}
        for i, ln in enumerate(body_lines):
            for m in _POPEN_OPEN.finditer(ln):
                opens.setdefault(m.group("var"), []).append(i)
            for m in _POPEN_CLOSE.finditer(ln):
                closes.setdefault(m.group("var"), []).append(i)
        if not opens:
            continue
        returns = [
            i for i, ln in enumerate(body_lines)
            if _rx(r"\s*return\b").match(ln)
        ]
        for var, open_hits in opens.items():
            close_hits = closes.get(var, [])
            leaked = False
            leak_ep = open_hits[0]
            for r in returns:
                last_open = max((h for h in open_hits if h < r), default=-1)
                last_close = max((h for h in close_hits if h < r), default=-1)
                if last_open > last_close:
                    leaked = True
                    leak_ep = r
                    break
            if not leaked:
                last_open = max(open_hits)
                last_close = max(close_hits) if close_hits else -1
                if last_open > last_close:
                    leaked = True
                    leak_ep = last_open
            if leaked:
                ln = fn.span[0] + leak_ep
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=ln, cls="API-POPEN",
                    message=f"{var} from popen() not pclose()'d on all exits",
                    strength=laws.STRENGTH_FINDS,
                    evidence=body_lines[leak_ep].strip(),
                ))


_INTENT_INCREMENT = re.compile(
    r"returns\s+\w+\s*\+\s*1"
    r"|return\s+\w+\s*\+\s*1"
    r"|result\s+is\s+\w+\s*\+\s*1"
    r"|adds\s+one"
    r"|increment",
    re.I,
)
_BARE_RETURN = re.compile(r"\breturn\s+(?!\*&)([A-Za-z_]\w*)\s*;")


def _comment_preamble(lines: list[str], body_start: int, *, max_comments: int = 3) -> str:
    """Comment / blank lines immediately above the function (ACSL style)."""
    i = body_start - 1
    in_block = False
    while i >= 0:
        raw = lines[i]
        s = raw.strip()
        if in_block:
            if "/*@" in raw or s.startswith("/*"):
                in_block = False
            i -= 1
            continue
        if not s:
            i -= 1
            continue
        if s.startswith("//"):
            i -= 1
            continue
        if s.endswith("*/") or s.startswith("/*") or s.startswith("*"):
            if "/*" in raw and "*/" in raw:
                i -= 1
                continue
            in_block = True
            i -= 1
            continue
        break
    preamble = "\n".join(lines[i + 1:body_start])
    kept: list[str] = []
    for ln in preamble.splitlines():
        s = ln.strip()
        if not s:
            continue
        if s.startswith("//") or s.startswith("/*") or s.startswith("*") or "*/" in s:
            kept.append(ln)
    return "\n".join(kept[-max_comments:])


def _has_increment_on(name: str, body: str) -> bool:
    v = re.escape(name)
    return bool(
        re.search(rf"\b{v}\s*\+\+", body)
        or re.search(rf"\+\+\s*{v}\b", body)
        or re.search(rf"\b{v}\s*\+", body)
        or re.search(rf"\+\s*{v}\b", body)
    )


_LOCAL_STRUCT_DECL = re.compile(
    r"^\s*(?P<static>static\s+)?struct\s+\w+\s+(?P<name>[A-Za-z_]\w*)\s*(?P<zero>=\s*\{0\})?\s*;",
    re.M,
)
_MEMCPY_ADDR_LOCAL = re.compile(
    r"memcpy\s*\([^,]+,\s*&\s*(?P<var>[A-Za-z_]\w*)\s*,\s*sizeof"
)


def _local_structs(fn) -> set[str]:
    params = {name for typ, name in fn.params if name}
    names: set[str] = set()
    for m in _lit_finditer(_LOCAL_STRUCT_DECL, fn.body):
        if m.group("static") or m.group("zero"):
            continue
        name = m.group("name")
        if name not in params and name not in _KW:
            names.add(name)
    return names


def _local_zeroed(body: str, var: str) -> bool:
    v = re.escape(var)
    if re.search(rf"memset\s*\(\s*&?\s*{v}\s*,\s*0\b", body):
        return True
    return bool(re.search(rf"struct\s+\w+\s+{v}\s*=\s*\{{0\}}", body))


def _infoleak_pad(lines, rel, funcs, out) -> None:
    """Local struct copied with memcpy(&local, sizeof) without memset/{0} first."""
    for fn in funcs:
        structs = _local_structs(fn)
        if not structs:
            continue
        start, end = fn.span
        chunk = lines[start - 1 : end]
        seen: set[tuple[str, int]] = set()
        for i, ln in enumerate(chunk):
            m = _MEMCPY_ADDR_LOCAL.search(ln)
            if not m:
                continue
            var = m.group("var")
            if var not in structs:
                continue
            if _local_zeroed(fn.body, var):
                continue
            key = (var, i)
            if key in seen:
                continue
            seen.add(key)
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="INFOLEAK-PAD",
                message=f"memcpy copies local struct {var} without zeroing padding first",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))


_API_PRECOND_PATTERNS = (
    re.compile(r"requires\s+(\w+)\s*>\s*0", re.I),
    re.compile(r"(\w+)\s+must\s+be\s+positive", re.I),
    re.compile(r"(\w+)\s+is\s+positive", re.I),
    re.compile(r"(\w+)\s*>\s*0", re.I),
)
_IF_PARAM_GUARD = re.compile(r"\bif\s*\(\s*(\w+)")


def _precond_var_from_preamble(preamble: str) -> str | None:
    for pat in _API_PRECOND_PATTERNS:
        m = pat.search(preamble)
        if m:
            return m.group(1)
    return None


def _api_precondition(orig_lines: list[str], rel: str, funcs, out) -> None:
    """Comment requires n>0 but body never tests n in an if guard."""
    for fn in funcs:
        if fn.kind != "SCALAR":
            continue
        fn_line = fn.line or fn.span[0]
        preamble = _comment_preamble(orig_lines, max(0, fn_line - 1))
        if not preamble:
            continue
        var = _precond_var_from_preamble(preamble)
        if not var:
            continue
        guarded = any(
            gm.group(1) == var for gm in _lit_finditer(_IF_PARAM_GUARD, fn.body)
        )
        if guarded:
            continue
        out.append(Finding(
            stage="lints", status=laws.FAILED, file=rel,
            function=fn.name, line=fn_line, cls="API-PRECONDITION",
            message=f"comment requires {var}>0 but {fn.name}() never guards {var}",
            strength=laws.STRENGTH_FINDS,
            evidence=orig_lines[fn_line - 1].strip()
            if 0 < fn_line <= len(orig_lines) else "",
        ))


def _intent_mismatch(orig_lines: list[str], rel: str, funcs, out) -> None:
    """Comment claims increment but body returns a bare identifier unchanged."""
    for fn in funcs:
        if fn.kind not in ("SCALAR", "VOID"):
            continue
        fn_line = fn.line or fn.span[0]
        preamble = _comment_preamble(orig_lines, max(0, fn_line - 1))
        if not preamble or not _INTENT_INCREMENT.search(preamble):
            continue
        start, end = fn.span
        seen = False
        for i, ln in enumerate(_gated_lines(fn.body, _BARE_RETURN)):
            m = _BARE_RETURN.search(ln)
            if not m:
                continue
            var = m.group(1)
            if var in _KW or _has_increment_on(var, fn.body):
                continue
            if seen:
                continue
            seen = True
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="INTENT",
                message=f"comment claims increment but {fn.name}() returns {var} unchanged",
                strength=laws.STRENGTH_FINDS,
                evidence=orig_lines[line - 1].strip()
                if 0 < line <= len(orig_lines) else ln.strip(),
            ))


_PARSE_INPUT_ASSIGN = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?"
    r"(?:atoi|strtol|strtoul)\s*\("
)
_PARSE_SCANF = re.compile(
    r"\bscanf\s*\([^)]*&\s*(?P<var>[A-Za-z_]\w*)"
)


def _parsed_input_vars(body: str) -> set[str]:
    names: set[str] = set()
    for m in _lit_finditer(_PARSE_INPUT_ASSIGN, body):
        names.add(m.group("var"))
    for m in _lit_finditer(_PARSE_SCANF, body):
        names.add(m.group("var"))
    return names


def _has_if_guard(name: str, body: str) -> bool:
    """`if (name …)` guard anywhere in the body."""
    v = re.escape(name)
    return bool(re.search(rf"\bif\s*\(\s*{v}\b", body))


def _unvalidated_index_use(var: str, text: str) -> bool:
    v = re.escape(var)
    if re.search(rf"\[\s*{v}\s*\]", text):
        return True
    return bool(re.search(rf"(?:/|%)\s*{v}\b", text))


def _trust_unvalidated_input(lines, rel, funcs, out) -> None:
    """Parsed atoi/strtol/strtoul/scanf value used as index or divisor unchecked."""
    for fn in funcs:
        body = fn.body
        parsed = _parsed_input_vars(body)
        if not parsed:
            continue
        body_lines = body.splitlines()
        start = fn.span[0]
        for var in sorted(parsed):
            if _has_if_guard(var, body):
                continue
            for i, ln in enumerate(body_lines):
                if not _unvalidated_index_use(var, ln):
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="TRUST-UNVALIDATED-INPUT",
                    message=f"{var} from parsed input used as index or divisor "
                    f"without an if ({var} …) guard",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else "",
                ))
                break


_CRYPTO_FN = re.compile(r"(?:key|crypto)", re.I)
_KEY_RAND_STORE = re.compile(
    r"\bkey\s*\[[^\]]+\]\s*=\s*(?:rand|random)\s*\("
)
_RAND_CALL = re.compile(r"\b(?:rand|random)\s*\(")
_RAND_LOCAL = re.compile(
    r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:rand|random)\s*\("
)


def _ptr_rand_store(ptr: str, line: str) -> bool:
    pe = re.escape(ptr)
    if re.search(rf"\b{pe}\s*\[[^\]]+\]\s*=\s*(?:rand|random)\s*\(", line):
        return True
    return bool(re.search(rf"\*\s*{pe}\s*=\s*(?:rand|random)\s*\(", line))


def _ptr_local_store(ptr: str, local: str, text: str) -> bool:
    pe, le = re.escape(ptr), re.escape(local)
    if re.search(rf"\b{pe}\s*\[[^\]]+\]\s*=\s*{le}\b", text):
        return True
    if re.search(rf"\*\s*{pe}\s*=\s*{le}\b", text):
        return True
    return bool(re.search(
        rf"\b(?:memcpy|memmove)\s*\(\s*{pe}\b[^;]*\b{le}\b",
        text,
    ))


def _crypto_srand(lines, rel, funcs, out) -> None:
    """srand(time()) / srandom(time()) seeds PRNG from the clock (CWE-330)."""
    for fn in funcs:
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            if not _SRAND_TIME.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CRYPTO-SRAND",
                message="srand(time()) is not a CSPRNG seed",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else ln.strip(),
            ))


def _realloc_zero_old_ptr(ln: str) -> str | None:
    """First realloc arg when size is literal 0 and ptr is not self-reassigned."""
    args = _find_call_args(ln, "realloc")
    if not args or len(args) < 2:
        return None
    if _parse_int_literal(args[1]) != 0:
        return None
    ptr = _rx(r"^\([^)]*\)\s*").sub("", args[0].strip()).strip()
    if re.match(
        rf"^{re.escape(ptr)}\s*=\s*(?:\([^)]*\)\s*)*realloc\s*\(",
        ln,
    ):
        return None
    return ptr


def _stale_ptr_use(var: str, ln: str) -> bool:
    v = re.escape(var)
    if re.search(rf"\b{v}\s*\[", ln):
        return True
    if re.search(rf"\*\s*{v}\b", ln) or re.search(rf"\b{v}\s*->", ln):
        return True
    if re.search(rf"\bfree\s*\(\s*(?:\([^)]*\)\s*)*{v}\s*\)", ln):
        return True
    return bool(re.search(rf"\breturn\s+{v}\s*;", ln))


def _mem_realloc_zero(lines, rel, funcs, out) -> None:
    """Old pointer used after realloc(ptr, 0) when result goes elsewhere (CWE-761/416)."""
    for fn in funcs:
        body_lines = fn.body.splitlines()
        start = fn.span[0]
        for i, ln in enumerate(body_lines):
            old = _realloc_zero_old_ptr(ln)
            if not old:
                continue
            for j in range(i + 1, len(body_lines)):
                later = body_lines[j]
                if re.search(rf"\b{re.escape(old)}\s*=", later):
                    break
                if _stale_ptr_use(old, later):
                    line = start + j
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="MEM-REALLOC-ZERO",
                        message=f"{old} used after realloc({old}, 0)",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else later.strip(),
                    ))
                    break


def _crypto_misuse(lines, rel, funcs, out) -> None:
    """rand()/random() fills key material or a crypto/key-named buffer."""
    for fn in funcs:
        body = fn.body
        body_lines = body.splitlines()
        start = fn.span[0]
        reported = False
        for i, ln in enumerate(body_lines):
            if not _KEY_RAND_STORE.search(ln):
                continue
            line = start + i
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="CRYPTO-MISUSE",
                message="rand() used to fill key[]",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))
            reported = True
            break
        if reported:
            continue
        if not _CRYPTO_FN.search(fn.name):
            continue
        ptrs = _pointer_param_names(fn)
        if not ptrs:
            continue
        for i, ln in enumerate(body_lines):
            if not _RAND_CALL.search(ln):
                continue
            for p in ptrs:
                if _ptr_rand_store(p, ln):
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="CRYPTO-MISUSE",
                        message=f"rand() assigned into pointer parameter {p}",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else "",
                    ))
                    reported = True
                    break
            if reported:
                break
        if reported:
            continue
        rand_locals: set[str] = set()
        for ln in body_lines:
            for m in _RAND_LOCAL.finditer(ln):
                rand_locals.add(m.group("var"))
        if not rand_locals:
            continue
        for p in ptrs:
            for local in sorted(rand_locals):
                if not _ptr_local_store(p, local, body):
                    continue
                for i, ln in enumerate(body_lines):
                    if local not in ln or p not in ln:
                        continue
                    if not _ptr_local_store(p, local, ln):
                        continue
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="CRYPTO-MISUSE",
                        message=f"rand() in {local} copied into {p}",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else "",
                    ))
                    reported = True
                    break
                if reported:
                    break
            if reported:
                break


def _constant_bound(size: str, fn, params: set[str]) -> bool:
    """`buf[BUFSIZ]`, `buf[MAX_LEN * 2 + 1]`, `buf[sizeof(struct s)]`.

    A bound made of literals, sizeof and ALL_CAPS names (the macro /
    enum-constant convention) that the function never assigns or takes as a
    parameter is a constant expression, not a VLA. Any lowercase name keeps
    the finding.
    """
    s = size
    k = s.find("sizeof")
    while k >= 0:
        e = k + 6
        while e < len(s) and s[e].isspace():
            e += 1
        if e < len(s) and s[e] == "(":
            depth = 0
            while e < len(s):
                if s[e] == "(":
                    depth += 1
                elif s[e] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                e += 1
            if e >= len(s):
                return False
            e += 1
        else:
            while e < len(s) and (s[e].isalnum() or s[e] == "_"):
                e += 1
        s = s[:k] + " " * (e - k) + s[e:]
        k = s.find("sizeof", k)
    i = 0
    while i < len(s):
        c = s[i]
        if c.isdigit():
            while i < len(s) and (s[i].isalnum() or s[i] == "."):
                i += 1
            continue
        if c.isalpha() or c == "_":
            j = i
            while j < len(s) and (s[j].isalnum() or s[j] == "_"):
                j += 1
            name = s[i:j]
            if (any(ch.islower() for ch in name) or not any(ch.isalpha() for ch in name)
                    or name in params):
                return False
            if re.search(r"\b" + re.escape(name) + r"\s*(?:=[^=]|\+\+|--|[-+*/%&|^]=)", fn.body):
                return False
            i = j
            continue
        if c not in "+-*/%()<> \t\n|&^~":
            return False
        i += 1
    return True


def _vla_size(lines, rel, funcs, out) -> None:
    """Local array bound is not an integer literal (VLA)."""
    for fn in funcs:
        params = {name for typ, name in fn.params if name}
        start, end = fn.span
        seen: set[tuple[str, int]] = set()
        for m in _lit_finditer(_VLA_DECL, fn.body):
            if m.group("static"):
                continue
            name = m.group("name")
            if name in params or name in _KW:
                continue
            size = m.group("size").strip()
            if _parse_int_literal(size) is not None or _constant_bound(size, fn, params):
                continue
            rel_off = fn.body[: m.start()].count("\n")
            key = (name, rel_off)
            if key in seen:
                continue
            seen.add(key)
            line = start + rel_off
            out.append(Finding(
                stage="lints", status=laws.FAILED, file=rel,
                function=fn.name, line=line, cls="MEM-VLA-SIZE",
                message=f"{name}[{size}] is a variable-length local array",
                strength=laws.STRENGTH_FINDS,
                evidence=lines[line - 1].strip()
                if 0 < line <= len(lines) else "",
            ))


def _mem_alloca(lines, rel, funcs, out) -> None:
    """alloca/__builtin_alloca whose size is not an integer literal."""
    for fn in funcs:
        start = fn.span[0]
        body_lines = fn.body.splitlines()
        for i, ln in enumerate(body_lines):
            for fname in ("alloca", "__builtin_alloca"):
                args = _find_call_args(ln, fname)
                if not args:
                    continue
                size = args[0].strip()
                if _parse_int_literal(size) is not None:
                    continue
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="MEM-ALLOCA",
                    message=f"{fname}({size}) size is not a constant",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                break


_STRUCT_DEF = re.compile(
    r"\b(?:struct|class)\s+(?P<tag>[A-Za-z_]\w*)\s*\{(?P<body>[^{}]*)\}"
)
_FAM_TAIL = re.compile(
    r"[A-Za-z_]\w*\s*\[\s*(?:0\s*)?\]\s*$"
)
_FAM_PTR = re.compile(
    r"\bstruct\s+(?P<tag>[A-Za-z_]\w*)\s*\*+\s*(?P<name>[A-Za-z_]\w*)"
)
_BARE_SIZEOF_STRUCT = re.compile(
    r"^sizeof\s*\(\s*(?:struct\s+)?([A-Za-z_]\w*)\s*\)$"
)
_BARE_SIZEOF_STAR = re.compile(
    r"^sizeof\s*(?:\(\s*\*\s*([A-Za-z_]\w*)\s*\)|\*\s*([A-Za-z_]\w*))$"
)


def _flex_array_tags(text: str) -> set[str]:
    """Tags whose last member is a C99 flexible array or GNU `[0]`."""
    tags: set[str] = set()
    for m in _lit_finditer(_STRUCT_DEF, text):
        decls = [p.strip() for p in m.group("body").split(";") if p.strip()]
        if decls and _FAM_TAIL.search(decls[-1]):
            tags.add(m.group("tag"))
    return tags


def _unwrap_parens(expr: str) -> str:
    t = expr.strip()
    while t.startswith("(") and t.endswith(")"):
        inner = t[1:-1].strip()
        if inner.count("(") != inner.count(")"):
            break
        t = inner
    return t


def _bare_sizeof(expr: str) -> tuple[str | None, str | None]:
    """sizeof(struct T) / sizeof(*p) with no added bytes → ('struct', T) or ('star', p)."""
    t = _unwrap_parens(expr)
    if "+" in t:
        return None, None
    m = _BARE_SIZEOF_STRUCT.fullmatch(t)
    if m:
        return "struct", m.group(1)
    m = _BARE_SIZEOF_STAR.fullmatch(t)
    if m:
        return "star", m.group(1) or m.group(2)
    return None, None


def _mem_flex_array(stripped: str, lines, rel, funcs, out) -> None:
    """malloc/calloc/realloc of a flexible-array struct at header size only.

    `sizeof(struct T) + n` is the honest allocation. A struct without a
    trailing `[]` / `[0]` member is not this class.
    """
    tags = _flex_array_tags(stripped)
    if not tags:
        return
    for fn in funcs:
        ptrs: dict[str, str] = {}
        for typ, name in fn.params:
            if not name or "*" not in typ:
                continue
            t = _rx(r"\b(?:const|volatile|struct|class)\b").sub(" ", typ)
            t = t.replace("*", " ")
            ptag = " ".join(t.split())
            if ptag in tags:
                ptrs[name] = ptag
        for m in _lit_finditer(_FAM_PTR, fn.body):
            if m.group("tag") in tags:
                ptrs[m.group("name")] = m.group("tag")
        start = fn.span[0]
        seen: set[int] = set()
        for i, ln in enumerate(fn.body.splitlines()):
            for fname in ("malloc", "realloc", "calloc"):
                args = _find_call_args(ln, fname)
                if not args:
                    continue
                if fname == "malloc":
                    size = args[0]
                elif fname == "realloc" and len(args) >= 2:
                    size = args[1]
                elif fname == "calloc" and len(args) >= 2:
                    size = args[1]
                else:
                    continue
                kind, ident = _bare_sizeof(size)
                tag: str | None = None
                if kind == "struct" and ident in tags:
                    tag = ident
                elif kind == "star" and ident in ptrs:
                    tag = ptrs[ident]
                if not tag or i in seen:
                    continue
                seen.add(i)
                line = start + i
                out.append(Finding(
                    stage="lints", status=laws.FAILED, file=rel,
                    function=fn.name, line=line, cls="MEM-FLEX-ARRAY",
                    message=f"{fname}() sizes struct {tag} at the header; "
                    "flexible array needs extra bytes",
                    strength=laws.STRENGTH_FINDS,
                    evidence=lines[line - 1].strip()
                    if 0 < line <= len(lines) else ln.strip(),
                ))
                break


_BARE_SIZEOF_IDENT = re.compile(
    r"^sizeof\s*\(\s*([A-Za-z_]\w*)\s*\)$"
)
_PTR_DECL = re.compile(
    r"\b(?:const\s+|volatile\s+)?(?:struct\s+[A-Za-z_]\w+\s+)?"
    r"(?:\w+)\s*\*\s*([A-Za-z_]\w*)\b"
)


def _pointer_locals(fn) -> set[str]:
    ptrs: set[str] = set()
    for typ, name in fn.params:
        if name and "*" in typ:
            ptrs.add(name)
    for m in _lit_finditer(_PTR_DECL, fn.body):
        ptrs.add(m.group(1))
    return ptrs


def _mem_sizeof_ptr(lines, rel, funcs, out) -> None:
    """malloc/calloc/realloc sized with sizeof(pointer) instead of sizeof(*p)."""
    for fn in funcs:
        ptrs = _pointer_locals(fn)
        if not ptrs:
            continue
        start = fn.span[0]
        seen: set[int] = set()
        for i, ln in enumerate(fn.body.splitlines()):
            for fname in ("malloc", "realloc", "calloc"):
                args = _find_call_args(ln, fname)
                if not args:
                    continue
                if fname == "malloc":
                    size_args = [args[0]]
                elif fname == "realloc" and len(args) >= 2:
                    size_args = [args[1]]
                elif fname == "calloc" and len(args) >= 2:
                    size_args = list(args[:2])
                else:
                    continue
                for size in size_args:
                    t = _unwrap_parens(size)
                    m = _BARE_SIZEOF_IDENT.fullmatch(t)
                    if not m or m.group(1) not in ptrs:
                        continue
                    if i in seen:
                        continue
                    seen.add(i)
                    line = start + i
                    out.append(Finding(
                        stage="lints", status=laws.FAILED, file=rel,
                        function=fn.name, line=line, cls="MEM-SIZEOF-PTR",
                        message=f"{fname}() size is sizeof({m.group(1)}), "
                        "not sizeof(*p) or the pointee type",
                        strength=laws.STRENGTH_FINDS,
                        evidence=lines[line - 1].strip()
                        if 0 < line <= len(lines) else ln.strip(),
                    ))
                    break


def run_lints(paths: list[Path], root: Path, jobs: int = 0) -> list[Finding]:
    def _one(p: Path) -> list[Finding]:
        try:
            rel = str(p.relative_to(root)) if root.is_dir() else p.name
        except ValueError:
            rel = str(p)
        return _findings_for_file(p, rel)

    n = jobs if jobs and jobs > 1 else 1
    if n == 1 or len(paths) <= 1:
        out: list[Finding] = []
        for p in paths:
            out.extend(_one(p))
        return out
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=n) as pool:
        chunks = list(pool.map(_one, paths))
    out = []
    for chunk in chunks:
        out.extend(chunk)
    return out
