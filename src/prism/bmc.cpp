#include "prism/stages.hpp"
#include "prism/ai.hpp"
#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/regex.hpp"
#include <format>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <filesystem>
#include <cstring>

#ifdef PRISM_HAS_Z3
#include <z3++.h>
#endif

namespace prism {
namespace {

constexpr int WIDTH = 32;

std::string lstrip(std::string s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}
std::string rstrip(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}
std::string strip(std::string s) { return rstrip(lstrip(std::move(s))); }
std::string to_lower_copy(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct RxCached {
    std::unique_ptr<Regex> re;
};
std::unordered_map<std::string, RxCached>& rx_cache() {
    static std::unordered_map<std::string, RxCached> c;
    return c;
}
Regex& cached_rx(std::string_view pattern, bool ml, bool ds) {
    std::string key = (ml ? "m" : "-") + std::string(ds ? "s:" : "-:") + std::string(pattern);
    auto& slot = rx_cache()[key];
    if (!slot.re) slot.re = std::make_unique<Regex>(std::string(pattern), ml, ds);
    return *slot.re;
}
bool rx_search(std::string_view pattern, std::string_view s, bool ml = false, bool ds = false) {
    return cached_rx(pattern, ml, ds).search(s);
}
std::optional<Match> rx_match(std::string_view pattern, std::string_view s, bool ml = false, bool ds = false) {
    auto m = cached_rx(pattern, ml, ds).search_match(s);
    if (!m || m->spans.empty() || m->spans[0].first != 0) return std::nullopt;
    return m;
}
bool rx_fullmatch(std::string_view pattern, std::string_view s, bool ml = false, bool ds = false) {
    auto m = cached_rx(pattern, ml, ds).search_match(s);
    return m && !m->spans.empty() && m->spans[0].first == 0 &&
           static_cast<size_t>(m->spans[0].second) == s.size();
}
std::vector<Match> rx_finditer(std::string_view pattern, std::string_view s, bool ml = false, bool ds = false) {
    return cached_rx(pattern, ml, ds).finditer(s);
}
std::string rx_sub(std::string_view pattern, std::string_view repl, std::string_view s,
                   bool ml = false, bool ds = false) {
    auto ms = cached_rx(pattern, ml, ds).finditer(s);
    if (ms.empty()) return std::string(s);
    std::string out;
    size_t last = 0;
    for (auto& m : ms) {
        auto a = static_cast<size_t>(std::max(0, m.spans[0].first));
        auto b = static_cast<size_t>(std::max(0, m.spans[0].second));
        if (a > last) out.append(s.substr(last, a - last));
        out.append(repl);
        last = b;
    }
    if (last < s.size()) out.append(s.substr(last));
    return out;
}



bool is_ident(std::string_view t);
bool starts_kw(std::string_view text, std::string_view kw);
bool is_nested_function(std::string_view text);

const std::unordered_set<std::string> CALL_KW = {
    "_Alignof",
    "_Generic",
    "_Static_assert",
    "__attribute__",
    "__typeof__",
    "alignof",
    "assert",
    "break",
    "case",
    "continue",
    "default",
    "do",
    "else",
    "enum",
    "for",
    "goto",
    "if",
    "offsetof",
    "return",
    "sizeof",
    "static_assert",
    "struct",
    "switch",
    "typeof",
    "union",
    "while",
};
const std::unordered_set<std::string> UNENCODED_CSTR = {
    "fscanf",
    "gets",
    "memcpy",
    "memmove",
    "mkdtemp",
    "mkstemp",
    "mkstemps",
    "pclose",
    "popen",
    "scanf",
    "snprintf",
    "sprintf",
    "sscanf",
    "strcat",
    "strcpy",
    "strncat",
    "strncpy",
    "strtok",
    "tempnam",
    "tmpnam",
    "tmpnam_r",
    "vsnprintf",
    "vsprintf",
};
const std::unordered_set<std::string> UNENCODED_LIBC_EFFECT = {
    "__atomic_load",
    "__atomic_store",
    "__builtin_choose_expr",
    "__builtin_clz",
    "__builtin_clzll",
    "__builtin_ctz",
    "__builtin_ctzll",
    "__builtin_trap",
    "__builtin_unreachable",
    "__sync_bool_compare_and_swap",
    "__sync_fetch_and_add",
    "_exit",
    "_umtx_op",
    "abort",
    "accept",
    "accept4",
    "access",
    "acct",
    "add_key",
    "adjtime",
    "adjtimex",
    "aio_error",
    "aio_read",
    "aio_return",
    "aio_suspend",
    "aio_write",
    "alarm",
    "aligned_alloc",
    "arc4random",
    "arc4random_buf",
    "arc4random_uniform",
    "arch_prctl",
    "asprintf",
    "auditctl",
    "auditon",
    "bind",
    "bpf",
    "brk",
    "cachestat",
    "cap_enter",
    "cap_fcntls_limit",
    "cap_getmode",
    "cap_getrights",
    "cap_ioctls_limit",
    "cap_rights_get",
    "cap_rights_limit",
    "cap_sandboxed",
    "capget",
    "capset",
    "cfmakeraw",
    "chflags",
    "chmod",
    "chown",
    "clock_adjtime",
    "clock_getcpuclockid",
    "clock_gettime",
    "clock_nanosleep",
    "clock_settime",
    "clone",
    "clone3",
    "close_range",
    "closedir",
    "closefrom",
    "connect",
    "copy_file_range",
    "cpuset_getaffinity",
    "cpuset_setaffinity",
    "crypt",
    "crypt_checkpass",
    "crypt_newhash",
    "daemon",
    "delete_module",
    "devname",
    "devname_r",
    "dlclose",
    "dlfunc",
    "dlopen",
    "dlsym",
    "dlvsym",
    "dup",
    "dup2",
    "dup3",
    "eaccess",
    "endfsent",
    "epoll_create",
    "epoll_create1",
    "epoll_ctl",
    "epoll_pwait",
    "epoll_pwait2",
    "epoll_wait",
    "eventfd",
    "eventfd_read",
    "eventfd_write",
    "execl",
    "execle",
    "execlp",
    "execv",
    "execve",
    "execveat",
    "execvp",
    "execvpe",
    "exit",
    "explicit_bzero",
    "explicit_memset",
    "extattr_delete_fd",
    "extattr_delete_file",
    "extattr_delete_link",
    "extattr_get_fd",
    "extattr_get_file",
    "extattr_get_link",
    "extattr_list_fd",
    "extattr_list_file",
    "extattr_list_link",
    "extattr_set_fd",
    "extattr_set_file",
    "extattr_set_link",
    "faccessat",
    "faccessat2",
    "fallocate",
    "fanotify_init",
    "fanotify_mark",
    "fchflags",
    "fchmod",
    "fchmodat",
    "fchmodat2",
    "fchown",
    "fcntl",
    "fdatasync",
    "fdopendir",
    "fflagstostr",
    "fgetpos",
    "fhlink",
    "fhlinkat",
    "fhopen",
    "fhreadlink",
    "fhstat",
    "fhstatfs",
    "file_getattr",
    "file_setattr",
    "finit_module",
    "flock",
    "fmemopen",
    "fork",
    "fpathconf",
    "fprintf",
    "freeaddrinfo",
    "fsconfig",
    "fseek",
    "fsetpos",
    "fsetxattr",
    "fsmount",
    "fsopen",
    "fspick",
    "fstat",
    "fstatat",
    "fsync",
    "ftell",
    "fts_children",
    "fts_close",
    "fts_open",
    "fts_read",
    "fts_set",
    "ftw",
    "futex",
    "futex_requeue",
    "futex_wait",
    "futex_waitv",
    "futex_wake",
    "futimens",
    "get_mempolicy",
    "getaddrinfo",
    "getaudit",
    "getbootfile",
    "getcontext",
    "getcpu",
    "getdelim",
    "getdents",
    "getdents64",
    "getdirentries",
    "getdomainname",
    "getentropy",
    "getfh",
    "getfhat",
    "getfsent",
    "getfsstat",
    "getgrgid",
    "getgrnam",
    "getgrouplist",
    "getgroups",
    "gethostname",
    "getitimer",
    "getline",
    "getlogin",
    "getlogin_r",
    "getloginclass",
    "getmntinfo",
    "getopt",
    "getopt_long",
    "getopt_long_only",
    "getosreldate",
    "getpagesize",
    "getpagesizes",
    "getpass",
    "getpeereid",
    "getpeername",
    "getpriority",
    "getprogname",
    "getpwnam",
    "getpwuid",
    "getrandom",
    "getresgid",
    "getresuid",
    "getrlimit",
    "getrusage",
    "getsid",
    "getsockname",
    "getsockopt",
    "getspnam",
    "gettid",
    "gettimeofday",
    "getvfsbyname",
    "getxattr",
    "getxattrat",
    "glob",
    "globfree",
    "grantpt",
    "inet_aton",
    "inet_ntop",
    "inet_pton",
    "init_module",
    "initgroups",
    "inotify_add_watch",
    "inotify_init",
    "inotify_init1",
    "inotify_rm_watch",
    "io_cancel",
    "io_destroy",
    "io_getevents",
    "io_pgetevents",
    "io_setup",
    "io_submit",
    "io_uring_enter",
    "io_uring_register",
    "io_uring_setup",
    "ioctl",
    "ioperm",
    "iopl",
    "ioprio_get",
    "ioprio_set",
    "isatty",
    "issetugid",
    "jail",
    "jail_attach",
    "jail_get",
    "jail_remove",
    "jail_set",
    "kcmp",
    "kenv",
    "kevent",
    "kexec_file_load",
    "kexec_load",
    "keyctl",
    "kill",
    "kinfo_getfile",
    "kinfo_getproc",
    "kinfo_getvmmap",
    "kld_isloaded",
    "kld_load",
    "kldfind",
    "kldfirstmod",
    "kldload",
    "kldnextmod",
    "kldstat",
    "kldsym",
    "kldunload",
    "klogctl",
    "kqueue",
    "ksem_close",
    "ksem_open",
    "ksem_post",
    "ksem_unlink",
    "ksem_wait",
    "ktrace",
    "kvm_close",
    "kvm_getprocs",
    "kvm_nlist",
    "kvm_open",
    "kvm_openfiles",
    "landlock_add_rule",
    "landlock_create_ruleset",
    "landlock_restrict_self",
    "lchflags",
    "lchown",
    "linkat",
    "lio_listio",
    "listen",
    "listmount",
    "listxattr",
    "listxattrat",
    "login_getclass",
    "lpathconf",
    "lsetxattr",
    "lsm_get_self_attr",
    "lsm_list_modules",
    "lsm_set_self_attr",
    "lstat",
    "mac_get_fd",
    "mac_get_file",
    "mac_get_proc",
    "mac_set_fd",
    "mac_set_file",
    "mac_set_proc",
    "madvise",
    "makecontext",
    "map_shadow_stack",
    "mbind",
    "membarrier",
    "memfd_create",
    "memfd_secret",
    "memset_s",
    "migrate_pages",
    "mincore",
    "minherit",
    "mkdir",
    "mkdirat",
    "mkfifo",
    "mknod",
    "mknodat",
    "mktemp",
    "mlock",
    "mlock2",
    "mlockall",
    "mmap",
    "modfind",
    "modfnext",
    "modnext",
    "modstat",
    "mount",
    "mount_setattr",
    "move_mount",
    "move_pages",
    "mprotect",
    "mq_getsetattr",
    "mq_notify",
    "mq_open",
    "mq_timedreceive",
    "mq_timedsend",
    "mq_unlink",
    "mremap",
    "mseal",
    "msgctl",
    "msgget",
    "msgrcv",
    "msgsnd",
    "msync",
    "munlock",
    "munlockall",
    "munmap",
    "name_to_handle_at",
    "nanosleep",
    "nfssvc",
    "nftw",
    "nice",
    "nmount",
    "ntp_adjtime",
    "ntp_gettime",
    "open_by_handle_at",
    "open_memstream",
    "open_tree",
    "open_tree_attr",
    "open_wmemstream",
    "openat",
    "openat2",
    "opendir",
    "pathconf",
    "pause",
    "pdfork",
    "pdgetpid",
    "pdwait4",
    "perf_event_open",
    "personality",
    "pidfd_getfd",
    "pidfd_open",
    "pidfd_send_signal",
    "pipe",
    "pipe2",
    "pivot_root",
    "pkey_alloc",
    "pkey_free",
    "pkey_mprotect",
    "pledge",
    "poll",
    "posix_fadvise",
    "posix_fadvise64",
    "posix_fallocate",
    "posix_madvise",
    "posix_memalign",
    "posix_openpt",
    "posix_spawn",
    "posix_spawn_file_actions_init",
    "posix_spawnattr_init",
    "posix_spawnp",
    "posix_typed_mem_get_info",
    "posix_typed_mem_open",
    "ppoll",
    "prctl",
    "preadv",
    "preadv2",
    "printf",
    "prlimit",
    "prlimit64",
    "procctl",
    "process_madvise",
    "process_mrelease",
    "process_vm_readv",
    "pselect",
    "pthread_atfork",
    "pthread_attr_destroy",
    "pthread_attr_getdetachstate",
    "pthread_attr_getstack",
    "pthread_attr_getstacksize",
    "pthread_attr_init",
    "pthread_attr_setdetachstate",
    "pthread_attr_setstack",
    "pthread_attr_setstacksize",
    "pthread_barrier_destroy",
    "pthread_barrier_init",
    "pthread_barrier_wait",
    "pthread_cancel",
    "pthread_cond_broadcast",
    "pthread_cond_destroy",
    "pthread_cond_init",
    "pthread_cond_signal",
    "pthread_cond_timedwait",
    "pthread_cond_wait",
    "pthread_create",
    "pthread_detach",
    "pthread_getcpuclockid",
    "pthread_getspecific",
    "pthread_join",
    "pthread_key_create",
    "pthread_key_delete",
    "pthread_kill",
    "pthread_once",
    "pthread_rwlock_destroy",
    "pthread_rwlock_init",
    "pthread_rwlock_rdlock",
    "pthread_rwlock_timedrdlock",
    "pthread_rwlock_timedwrlock",
    "pthread_rwlock_tryrdlock",
    "pthread_rwlock_trywrlock",
    "pthread_rwlock_unlock",
    "pthread_rwlock_wrlock",
    "pthread_setspecific",
    "pthread_sigmask",
    "pthread_spin_destroy",
    "pthread_spin_init",
    "pthread_spin_lock",
    "pthread_spin_trylock",
    "pthread_spin_unlock",
    "pthread_yield",
    "ptrace",
    "ptsname",
    "ptsname_r",
    "pwritev",
    "pwritev2",
    "quotactl",
    "quotactl_fd",
    "raise",
    "readahead",
    "readdir",
    "readlink",
    "readlinkat",
    "reallocarray",
    "reallocf",
    "reboot",
    "recv",
    "recvfrom",
    "recvmmsg",
    "recvmsg",
    "remap_file_pages",
    "removexattr",
    "removexattrat",
    "rename",
    "renameat",
    "renameat2",
    "request_key",
    "rethrow_if_nested",
    "revoke",
    "rewind",
    "rfork",
    "rmdir",
    "rseq",
    "rt_sigqueueinfo",
    "rt_tgsigqueueinfo",
    "rtprio",
    "rtprio_thread",
    "sbrk",
    "scandir",
    "sched_get_priority_max",
    "sched_get_priority_min",
    "sched_getaffinity",
    "sched_getattr",
    "sched_getparam",
    "sched_getscheduler",
    "sched_setaffinity",
    "sched_setattr",
    "sched_setparam",
    "sched_setscheduler",
    "sched_yield",
    "seccomp",
    "select",
    "sem_close",
    "sem_destroy",
    "sem_getvalue",
    "sem_init",
    "sem_open",
    "sem_post",
    "sem_timedwait",
    "sem_trywait",
    "sem_unlink",
    "sem_wait",
    "semctl",
    "semget",
    "semop",
    "semtimedop",
    "send",
    "sendfile",
    "sendmmsg",
    "sendmsg",
    "sendto",
    "set_mempolicy",
    "set_mempolicy_home_node",
    "setaudit",
    "setcontext",
    "setdomainname",
    "seteuid",
    "setfib",
    "setfsent",
    "setfsgid",
    "setfsuid",
    "setgid",
    "setgroups",
    "sethostname",
    "setitimer",
    "setlogin",
    "setloginclass",
    "setns",
    "setpgid",
    "setpriority",
    "setproctitle",
    "setprogname",
    "setregid",
    "setresgid",
    "setresuid",
    "setreuid",
    "setrlimit",
    "setsid",
    "setsockopt",
    "settimeofday",
    "setuid",
    "setusercontext",
    "setxattr",
    "setxattrat",
    "shm_open",
    "shm_unlink",
    "shmat",
    "shmctl",
    "shmdt",
    "shmget",
    "shutdown",
    "sigaction",
    "sigaltstack",
    "signal",
    "signalfd",
    "sigpending",
    "sigprocmask",
    "sigqueue",
    "sigsuspend",
    "sigtimedwait",
    "sigwait",
    "sigwaitinfo",
    "sleep",
    "socket",
    "socketpair",
    "splice",
    "srand",
    "srandom",
    "stat",
    "statfs",
    "statmount",
    "statx",
    "strlcat",
    "strlcpy",
    "strmode",
    "strtofflags",
    "strtonum",
    "swapcontext",
    "swapoff",
    "swapon",
    "symlink",
    "symlinkat",
    "sync_file_range",
    "syncfs",
    "sysarch",
    "sysconf",
    "sysctl",
    "sysctlbyname",
    "sysinfo",
    "syslog",
    "system",
    "tcgetattr",
    "tcsetattr",
    "tgkill",
    "thr_exit",
    "thr_kill",
    "thr_kill2",
    "thr_new",
    "thr_self",
    "thr_suspend",
    "thr_wake",
    "thrd_create",
    "thrd_current",
    "thrd_detach",
    "thrd_equal",
    "thrd_exit",
    "thrd_join",
    "thrd_sleep",
    "thrd_yield",
    "throw_with_nested",
    "timer_create",
    "timer_delete",
    "timer_getoverrun",
    "timer_gettime",
    "timer_settime",
    "timerfd_create",
    "timerfd_gettime",
    "timerfd_settime",
    "timingsafe_bcmp",
    "timingsafe_memcmp",
    "tkill",
    "ttyname",
    "ttyname_r",
    "umask",
    "umount",
    "umount2",
    "uname",
    "unlink",
    "unlinkat",
    "unlockpt",
    "unmount",
    "unshare",
    "unveil",
    "userfaultfd",
    "usleep",
    "ustat",
    "utimensat",
    "utimes",
    "uuidgen",
    "valloc",
    "vasprintf",
    "vfork",
    "vhangup",
    "vmsplice",
    "wait",
    "wait3",
    "wait4",
    "wait6",
    "waitid",
    "waitpid",
    "wordexp",
    "wordfree",
};
const std::unordered_set<std::string> STMT_START_WORDS = {
    "_Atomic",
    "_Bool",
    "__asm",
    "__asm__",
    "asm",
    "assert",
    "auto",
    "bool",
    "break",
    "case",
    "catch",
    "char",
    "const",
    "continue",
    "default",
    "do",
    "double",
    "else",
    "enum",
    "extern",
    "float",
    "for",
    "goto",
    "if",
    "int",
    "int32_t",
    "int64_t",
    "long",
    "register",
    "return",
    "short",
    "signed",
    "size_t",
    "sizeof",
    "static",
    "struct",
    "switch",
    "throw",
    "try",
    "typedef",
    "typeof",
    "uint32_t",
    "uint64_t",
    "union",
    "unsigned",
    "void",
    "volatile",
    "while",
};



std::vector<std::string> call_names(std::string_view body) {
    std::vector<std::string> out;
    static Regex rx("\\b([A-Za-z_]\\w*)\\s*\\(");
    for (auto& m : rx.finditer(body)) {
        auto n = m.group(1);
        if (!CALL_KW.contains(n)) out.push_back(n);
    }
    return out;
}
bool has_self_call(const FunctionInfo& fn) {
    if (fn.name.empty()) return false;
    for (auto& n : call_names(fn.body))
        if (n == fn.name) return true;
    return false;
}
bool has_unencoded_cstr(const FunctionInfo& fn) {
    for (auto& n : call_names(fn.body))
        if (UNENCODED_CSTR.contains(n)) return true;
    return false;
}
bool has_unencoded_libc_effect(const FunctionInfo& fn) {
    for (auto& n : call_names(fn.body))
        if (UNENCODED_LIBC_EFFECT.contains(n)) return true;
    return false;
}
bool has_unencoded_cxx(const FunctionInfo& fn) {
    std::string blob = fn.return_type + " " + fn.body;
    return rx_search("\\bstd::|\\bstring_view\\b|\\bspan\\b", blob);
}
bool has_unencoded_float(const FunctionInfo& fn) {
    if (rx_search("(?i)\\bfloat\\b|\\bdouble\\b", fn.return_type)) return true;
    for (auto& [typ, _] : fn.params)
        if (rx_search("(?i)\\bfloat\\b|\\bdouble\\b", typ)) return true;
    return rx_search("\\d+\\.\\d+[fFlL]?|\\b(?:float|double)\\b", fn.body);
}
bool has_unencoded_throw(const FunctionInfo& fn) {
    return rx_search("\\bthrow\\b", fn.body);
}
bool has_unencoded_setjmp(const FunctionInfo& fn) {
    return rx_search("\\b(?:setjmp|longjmp|va_list|va_start)\\b", fn.body);
}


std::optional<std::string> unencoded_layout_prefix(std::string_view text) {
    std::string s = lstrip(std::string(text));
if (rx_match(R"BMC((?:struct|union)\s*(?:[A-Za-z_]\w*\s*)?\{)BMC", s)) {
    return R"BMC(struct unencoded)BMC";
}
if (rx_match(R"BMC(enum\s*\{)BMC", s)) {
    return R"BMC(anon enum unencoded)BMC";
}
if (rx_match(R"BMC((?:static|extern)\b)BMC", s)) {
    return R"BMC(storage-duration unencoded)BMC";
}
if (rx_match(R"BMC((?:_Alignas|alignas)\s*\()BMC", s)) {
    return R"BMC(alignas unencoded)BMC";
}
if (rx_match(R"BMC(__auto_type\b)BMC", s)) {
    return R"BMC(storage-class unencoded)BMC";
}
if (rx_match(R"BMC((?:_Thread_local|thread_local)\b)BMC", s)) {
    return R"BMC(thread-local unencoded)BMC";
}
if (rx_match(R"BMC((?:_Complex|_Imaginary)\b)BMC", s)) {
    return R"BMC(complex unencoded)BMC";
}
if (rx_match(R"BMC((?:_Decimal32|_Decimal64|_Decimal128)\b)BMC", s)) {
    return R"BMC(decimal-float unencoded)BMC";
}
if (rx_match(R"BMC((?:_Float16|_Float32|_Float64|__fp16)\b)BMC", s)) {
    return R"BMC(extra-IEEE unencoded)BMC";
}
if (rx_match(R"BMC((?:typeof_unqual|__typeof_unqual__)\s*\()BMC", s)) {
    return R"BMC(typeof_unqual unencoded)BMC";
}
if (rx_match(R"BMC((?:typeof|__typeof__)\s*\()BMC", s)) {
    return R"BMC(typeof unencoded)BMC";
}
if (rx_match(R"BMC(constexpr\b)BMC", s)) {
    return R"BMC(constexpr unencoded)BMC";
}
if (rx_match(R"BMC(\[\[\s*assume\s*\()BMC", s)) {
    return R"BMC(assume unencoded)BMC";
}
if (rx_match(R"BMC((?:int|unsigned(?:\s+int)?|long|short|char|uint32_t|int32_t|size_t)\s+\w+\s*=\s*\([^)]*\)\s*\{)BMC", s)) {
    return R"BMC(compound-lit unencoded)BMC";
}
return std::nullopt;
}

std::optional<std::string> unencoded_layout_stmt(std::string_view stmt) {
    std::string s = strip(std::string(stmt));
    if (s.empty()) return std::nullopt;
if (rx_search(R"BMC(\b(?:__int128(?:_t)?|_BitInt)\b)BMC", s)) {
    return R"BMC(128-bit unencoded)BMC";
}
if (rx_search(R"BMC(\b(?:_Decimal32|_Decimal64|_Decimal128)\b)BMC", s)) {
    return R"BMC(decimal-float unencoded)BMC";
}
if (rx_search(R"BMC(\b(?:_Float16|_Float32|_Float64|__fp16)\b)BMC", s)) {
    return R"BMC(extra-IEEE unencoded)BMC";
}
if (starts_kw(s, R"BMC(constexpr)BMC")) {
    return R"BMC(constexpr unencoded)BMC";
}
if (rx_search(R"BMC(\[\[\s*assume\s*\()BMC", s)) {
    return R"BMC(assume unencoded)BMC";
}
if (rx_search(R"BMC(__attribute__\s*\(\s*\(\s*cleanup)BMC", s)) {
    return R"BMC(cleanup unencoded)BMC";
}
if (rx_search(R"BMC(__attribute__\s*\(\s*\(\s*(?:__)?vector_size|\b__vector_size\b)BMC", s)) {
    return R"BMC(vector_size unencoded)BMC";
}
if (starts_kw(s, R"BMC(const)BMC")) {
    return R"BMC(const unencoded)BMC";
}
if ((starts_kw(s, R"BMC(register)BMC") || starts_kw(s, R"BMC(auto)BMC"))) {
    return R"BMC(storage-class unencoded)BMC";
}
if (rx_match(R"BMC(__auto_type\b)BMC", s)) {
    return R"BMC(storage-class unencoded)BMC";
}
if ((starts_kw(s, R"BMC(static)BMC") || starts_kw(s, R"BMC(extern)BMC"))) {
    return R"BMC(storage-duration unencoded)BMC";
}
if (rx_match(R"BMC((?:_Thread_local|thread_local)\b)BMC", s)) {
    return R"BMC(thread-local unencoded)BMC";
}
if (rx_match(R"BMC((?:_Complex|_Imaginary)\b)BMC", s)) {
    return R"BMC(complex unencoded)BMC";
}
if (rx_match(R"BMC((?:typeof_unqual|__typeof_unqual__)\s*\()BMC", s)) {
    return R"BMC(typeof_unqual unencoded)BMC";
}
if (rx_match(R"BMC((?:typeof|__typeof__)\s*\()BMC", s)) {
    return R"BMC(typeof unencoded)BMC";
}
if (is_nested_function(s)) {
    return R"BMC(nested function unencoded)BMC";
}
if (rx_match(R"BMC((?:_Alignas|alignas)\s*\()BMC", s)) {
    return R"BMC(alignas unencoded)BMC";
}
if (rx_search(R"BMC((?<!=)=\s*\([^)]*\)\s*\{)BMC", s)) {
    return R"BMC(compound-lit unencoded)BMC";
}
if ((starts_kw(s, R"BMC(struct)BMC") || starts_kw(s, R"BMC(union)BMC"))) {
    if (rx_match(R"BMC((?:struct|union)\s*\{)BMC", s)) {
        return R"BMC(struct unencoded)BMC";
    }
    if (rx_match(R"BMC((?:struct|union)\s+[A-Za-z_]\w*\s*\{)BMC", s)) {
        return R"BMC(struct unencoded)BMC";
    }
    if (rx_match(R"BMC((?:struct|union)\s+[A-Za-z_]\w*\s+[A-Za-z_]\w*)BMC", s)) {
        return R"BMC(struct unencoded)BMC";
    }
    return std::nullopt;
}
if (starts_kw(s, R"BMC(enum)BMC")) {
    if (rx_match(R"BMC(enum\s*\{)BMC", s)) {
        return R"BMC(anon enum unencoded)BMC";
    }
    if (rx_match(R"BMC(enum\s+[A-Za-z_]\w*\s+[A-Za-z_]\w*)BMC", s)) {
        return R"BMC(struct unencoded)BMC";
    }
    return std::nullopt;
}
auto m = rx_match(R"BMC(([A-Za-z_]\w*)\s+[A-Za-z_]\w*\s*(?:[=;\[]|$))BMC", s);
if (!(m)) {
    return std::nullopt;
}
if (STMT_START_WORDS.contains(m->group(1))) {
    return std::nullopt;
}
return R"BMC(typedef local unencoded)BMC";
}

}  // namespace

std::optional<std::string> harness_for_parsefail(std::string_view err, const std::string& engine) {
    std::string low = std::string(err);
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
if (low.find(R"BMC(vla)BMC") != std::string::npos) {
    if ((engine == R"BMC(bitvector BMC)BMC")) {
        return R"BMC(VLA size is a missing bound, not a closed BMC proof)BMC";
    }
    return R"BMC(VLA size is a missing bound, not a closed )BMC" + std::string(engine) + R"BMC( run)BMC";
}
if (low.find(R"BMC(throw unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ throw unencoded: )BMC" + std::string(engine) + R"BMC( is not an exception model)BMC";
}
if (low.find(R"BMC(asm unencoded)BMC") != std::string::npos) {
    return R"BMC(inline asm unencoded: )BMC" + std::string(engine) + R"BMC( is not an assembly model)BMC";
}
if (low.find(R"BMC(try unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ try/catch unencoded: )BMC" + std::string(engine) + R"BMC( is not an exception model)BMC";
}
if (low.find(R"BMC(statement-expr unencoded)BMC") != std::string::npos) {
    return R"BMC(GNU statement expression unencoded: )BMC" + std::string(engine) + R"BMC( is not a GNU-C model)BMC";
}
if (low.find(R"BMC(_generic unencoded)BMC") != std::string::npos) {
    return R"BMC(_Generic unencoded: )BMC" + std::string(engine) + R"BMC( is not a type-generic model)BMC";
}
if (low.find(R"BMC(offsetof unencoded)BMC") != std::string::npos) {
    return R"BMC(offsetof unencoded: )BMC" + std::string(engine) + R"BMC( is not a struct-layout model)BMC";
}
if (low.find(R"BMC(const unencoded)BMC") != std::string::npos) {
    return R"BMC(const local unencoded: )BMC" + std::string(engine) + R"BMC( is not a const model)BMC";
}
if (low.find(R"BMC(storage-class unencoded)BMC") != std::string::npos) {
    return R"BMC(register/auto unencoded: )BMC" + std::string(engine) + R"BMC( is not a storage-class or auto-type model)BMC";
}
if (low.find(R"BMC(storage-duration unencoded)BMC") != std::string::npos) {
    return R"BMC(static/extern local unencoded: )BMC" + std::string(engine) + R"BMC( is not a storage-duration model)BMC";
}
if (low.find(R"BMC(anon enum unencoded)BMC") != std::string::npos) {
    return R"BMC(anonymous enum local unencoded: )BMC" + std::string(engine) + R"BMC( is not a layout model)BMC";
}
if (low.find(R"BMC(alignas unencoded)BMC") != std::string::npos) {
    return R"BMC(_Alignas unencoded: )BMC" + std::string(engine) + R"BMC( is not an alignment model)BMC";
}
if (low.find(R"BMC(compound-lit unencoded)BMC") != std::string::npos) {
    return R"BMC(compound literal unencoded: )BMC" + std::string(engine) + R"BMC( is not a compound-literal model)BMC";
}
if (low.find(R"BMC(struct unencoded)BMC") != std::string::npos) {
    return R"BMC(struct/union local unencoded: )BMC" + std::string(engine) + R"BMC( is not a layout model)BMC";
}
if (low.find(R"BMC(typedef local unencoded)BMC") != std::string::npos) {
    return R"BMC(unknown typedef local unencoded: )BMC" + std::string(engine) + R"BMC( is not a layout model)BMC";
}
if (low.find(R"BMC(computed goto unencoded)BMC") != std::string::npos) {
    return R"BMC(computed goto unencoded: )BMC" + std::string(engine) + R"BMC( is not a computed-goto model)BMC";
}
if (low.find(R"BMC(label-address unencoded)BMC") != std::string::npos) {
    return R"BMC(label-address unencoded: )BMC" + std::string(engine) + R"BMC( is not a label-address model)BMC";
}
if (low.find(R"BMC(dynamic_cast unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ dynamic_cast unencoded: )BMC" + std::string(engine) + R"BMC( is not an RTTI model)BMC";
}
if (low.find(R"BMC(type_identity)BMC") != std::string::npos) {
    return R"BMC(C++ type_identity unencoded: )BMC" + std::string(engine) + R"BMC( is not a type_identity model)BMC";
}
if (low.find(R"BMC(typeid unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ typeid unencoded: )BMC" + std::string(engine) + R"BMC( is not an RTTI model)BMC";
}
if (low.find(R"BMC(reinterpret_cast unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ reinterpret_cast unencoded: )BMC" + std::string(engine) + R"BMC( is not a type-pun model)BMC";
}
if (low.find(R"BMC(coroutine unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ coroutine unencoded: )BMC" + std::string(engine) + R"BMC( is not a coroutine model)BMC";
}
if ((low.find(R"BMC(packed layout unencoded)BMC") != std::string::npos || low.find(R"BMC(packed unencoded)BMC") != std::string::npos || low.find(R"BMC(pragma pack)BMC") != std::string::npos)) {
    return R"BMC(packed layout unencoded: )BMC" + std::string(engine) + R"BMC( is not a packed-layout model)BMC";
}
if ((low.find(R"BMC(wide character unencoded)BMC") != std::string::npos || low.find(R"BMC(wide-char unencoded)BMC") != std::string::npos)) {
    return R"BMC(wide character unencoded: )BMC" + std::string(engine) + R"BMC( is not a wide-char model)BMC";
}
if (low.find(R"BMC(thread-local unencoded)BMC") != std::string::npos) {
    return R"BMC(thread-local unencoded: )BMC" + std::string(engine) + R"BMC( is not a TLS model)BMC";
}
if (low.find(R"BMC(complex unencoded)BMC") != std::string::npos) {
    return R"BMC(complex unencoded: )BMC" + std::string(engine) + R"BMC( is not a complex arithmetic model)BMC";
}
if (low.find(R"BMC(typeof_unqual unencoded)BMC") != std::string::npos) {
    return R"BMC(typeof_unqual unencoded: )BMC" + std::string(engine) + R"BMC( is not a typeof model)BMC";
}
if (low.find(R"BMC(typeof unencoded)BMC") != std::string::npos) {
    return R"BMC(typeof unencoded: )BMC" + std::string(engine) + R"BMC( is not a typeof model)BMC";
}
if (low.find(R"BMC(nested function unencoded)BMC") != std::string::npos) {
    return R"BMC(nested function unencoded: )BMC" + std::string(engine) + R"BMC( is not a nested-function model)BMC";
}
if (low.find(R"BMC(designated init unencoded)BMC") != std::string::npos) {
    return R"BMC(designated init unencoded: )BMC" + std::string(engine) + R"BMC( is not a designated-init model)BMC";
}
if (low.find(R"BMC(alignof unencoded)BMC") != std::string::npos) {
    return R"BMC(alignof unencoded: )BMC" + std::string(engine) + R"BMC( is not an alignment model)BMC";
}
if (low.find(R"BMC(va_arg unencoded)BMC") != std::string::npos) {
    return R"BMC(va_arg unencoded: )BMC" + std::string(engine) + R"BMC( is not a variadic model)BMC";
}
if (low.find(R"BMC(range-for unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ range-for unencoded: )BMC" + std::string(engine) + R"BMC( is not a range-for model)BMC";
}
if (low.find(R"BMC(lambda unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ lambda unencoded: )BMC" + std::string(engine) + R"BMC( is not a lambda model)BMC";
}
if (low.find(R"BMC(_static_assert unencoded)BMC") != std::string::npos) {
    return R"BMC(_Static_assert unencoded: )BMC" + std::string(engine) + R"BMC( is not a static-assert model)BMC";
}
if (low.find(R"BMC(if constexpr unencoded)BMC") != std::string::npos) {
    return R"BMC(if constexpr unencoded: )BMC" + std::string(engine) + R"BMC( is not a compile-time-if model)BMC";
}
if (low.find(R"BMC(constexpr unencoded)BMC") != std::string::npos) {
    return R"BMC(constexpr unencoded: )BMC" + std::string(engine) + R"BMC( is not a constexpr model)BMC";
}
if (low.find(R"BMC(case-range unencoded)BMC") != std::string::npos) {
    return R"BMC(case-range unencoded: )BMC" + std::string(engine) + R"BMC( is not a case-range model)BMC";
}
if ((low.find(R"BMC(128-bit unencoded)BMC") != std::string::npos || low.find(R"BMC(__int128)BMC") != std::string::npos || low.find(R"BMC(_bitint)BMC") != std::string::npos)) {
    return R"BMC(128-bit unencoded: )BMC" + std::string(engine) + R"BMC( is not a 128-bit model)BMC";
}
if ((low.find(R"BMC(bit_cast unencoded)BMC") != std::string::npos || low.find(R"BMC(bit_cast)BMC") != std::string::npos)) {
    return R"BMC(bit_cast unencoded: )BMC" + std::string(engine) + R"BMC( is not a type-pun model)BMC";
}
if ((low.find(R"BMC(std::thread unencoded)BMC") != std::string::npos || low.find(R"BMC(thread-lifetime unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::thread unencoded: )BMC" + std::string(engine) + R"BMC( is not a thread-lifetime model)BMC";
}
if (low.find(R"BMC(std::optional unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ std::optional unencoded: )BMC" + std::string(engine) + R"BMC( is not an optional model)BMC";
}
if (low.find(R"BMC(std::variant unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ std::variant unencoded: )BMC" + std::string(engine) + R"BMC( is not a variant model)BMC";
}
if (low.find(R"BMC(std::span unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ std::span unencoded: )BMC" + std::string(engine) + R"BMC( is not a span-lifetime model)BMC";
}
if (low.find(R"BMC(inplace_vector)BMC") != std::string::npos) {
    return R"BMC(C++ inplace_vector unencoded: )BMC" + std::string(engine) + R"BMC( is not an inplace_vector model)BMC";
}
if ((low.find(R"BMC(std::vector unencoded)BMC") != std::string::npos || low.find(R"BMC(vector unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::vector unencoded: )BMC" + std::string(engine) + R"BMC( is not a container model)BMC";
}
if (low.find(R"BMC(catch-all unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ catch-all unencoded: )BMC" + std::string(engine) + R"BMC( is not an exception model)BMC";
}
if (low.find(R"BMC(throw-new unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ throw-new unencoded: )BMC" + std::string(engine) + R"BMC( is not an exception model)BMC";
}
if (rx_search(R"BMC(\bexecveat\b)BMC", low)) {
    return R"BMC(execveat unencoded: unconstrained execveat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bpthread_atfork\b)BMC", low)) {
    return R"BMC(pthread_atfork unencoded: unconstrained pthread_atfork is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bpledge\b)BMC", low)) {
    return R"BMC(pledge unencoded: unconstrained pledge is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bmac_(?:set|get)_(?:proc|fd|file)\b)BMC", low)) {
    return R"BMC(mac_set_proc unencoded: unconstrained mac_set_proc is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bcap_getmode\b)BMC", low)) {
    return R"BMC(cap_getmode unencoded: unconstrained cap_getmode is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bcap_getrights\b)BMC", low)) {
    return R"BMC(cap_getrights unencoded: unconstrained cap_getrights is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bcap_enter\b)BMC", low)) {
    return R"BMC(cap_enter unencoded: unconstrained cap_enter is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bcap_sandboxed\b)BMC", low)) {
    return R"BMC(cap_sandboxed unencoded: unconstrained cap_sandboxed is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bcap_rights_(?:limit|get)\b)BMC", low)) {
    return R"BMC(cap_rights unencoded: unconstrained cap_rights_limit is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bcap_(?:fcntls|ioctls)_limit\b)BMC", low)) {
    return R"BMC(cap_fcntls unencoded: unconstrained cap_fcntls_limit is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bunveil\b)BMC", low)) {
    return R"BMC(unveil unencoded: unconstrained unveil is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsysctl(?:byname)?\b)BMC", low)) {
    return R"BMC(sysctl unencoded: unconstrained sysctl is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bkqueue\b)BMC", low)) {
    return R"BMC(kqueue unencoded: unconstrained kqueue is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bkevent\b)BMC", low)) {
    return R"BMC(kevent unencoded: unconstrained kevent is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bpause\b)BMC", low)) {
    return R"BMC(pause unencoded: unconstrained pause is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:get|set|swap|make)context\b)BMC", low)) {
    return R"BMC(getcontext unencoded: unconstrained getcontext is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((rx_search(R"BMC(\bpthread_attr_(?:init|destroy|setstacksize|setstack|setdetachstate|getstacksize|getstack|getdetachstate)\b)BMC", low) || low.find(R"BMC(pthread_attr unencoded)BMC") != std::string::npos)) {
    return R"BMC(pthread_attr unencoded: unconstrained pthread_attr_init is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(process spawn unencoded)BMC") != std::string::npos) {
    return R"BMC(process spawn unencoded: unconstrained fork/exec is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bpdfork\b)BMC", low)) {
    return R"BMC(pdfork unencoded: unconstrained pdfork is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\brfork\b)BMC", low)) {
    return R"BMC(rfork unencoded: unconstrained rfork is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bminherit\b)BMC", low)) {
    return R"BMC(minherit unencoded: unconstrained minherit is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bnfssvc\b)BMC", low)) {
    return R"BMC(nfssvc unencoded: unconstrained nfssvc is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsysarch\b)BMC", low)) {
    return R"BMC(sysarch unencoded: unconstrained sysarch is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bgetbootfile\b)BMC", low)) {
    return R"BMC(getbootfile unencoded: unconstrained getbootfile is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bdevname(?:_r)?\b)BMC", low)) {
    return R"BMC(devname unencoded: unconstrained devname is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsbrk\b)BMC", low)) {
    return R"BMC(sbrk unencoded: unconstrained sbrk is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((rx_search(R"BMC(\bbrk\s*\()BMC", low) || rx_search(R"BMC(\bbrk\b)BMC", low))) {
    return R"BMC(brk unencoded: unconstrained brk is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(mmap unencoded)BMC") != std::string::npos) {
    return R"BMC(mmap unencoded: )BMC" + std::string(engine) + R"BMC( is not a VM model)BMC";
}
if ((low.find(R"BMC(wide-string copy unencoded)BMC") != std::string::npos || low.find(R"BMC(wcscpy unencoded)BMC") != std::string::npos)) {
    return R"BMC(wide-string copy unencoded: unconstrained wcscpy is not a proof of the buffer ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((rx_search(R"BMC(\b(?:dlfunc|dlvsym)\b)BMC", low) || low.find(R"BMC(dlfunc unencoded)BMC") != std::string::npos)) {
    return R"BMC(dlfunc unencoded: unconstrained dlfunc is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(dlopen unencoded)BMC") != std::string::npos || low.find(R"BMC(dlsym unencoded)BMC") != std::string::npos || low.find(R"BMC(dlclose unencoded)BMC") != std::string::npos)) {
    return R"BMC(dlopen unencoded: )BMC" + std::string(engine) + R"BMC( is not a dynamic-loader model)BMC";
}
if (rx_search(R"BMC(\bmod(?:find|stat|next|fnext)\b)BMC", low)) {
    return R"BMC(modfind unencoded: unconstrained modfind is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bkld(?:firstmod|nextmod)\b)BMC", low)) {
    return R"BMC(kldfirstmod unencoded: unconstrained kldfirstmod is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bkld_(?:isloaded|load)\b)BMC", low)) {
    return R"BMC(kld_load unencoded: unconstrained kld_load is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bkld(?:load|unload|find|sym|stat)\b)BMC", low)) {
    return R"BMC(kldload unencoded: unconstrained kldload is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(clz unencoded)BMC") != std::string::npos || low.find(R"BMC(ctz unencoded)BMC") != std::string::npos || low.find(R"BMC(__builtin_clz)BMC") != std::string::npos)) {
    return R"BMC(clz unencoded: unconstrained __builtin_clz is UB on 0 and is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(nullptr unencoded)BMC") != std::string::npos) {
    return R"BMC(C23 nullptr unencoded: )BMC" + std::string(engine) + R"BMC( is not a nullptr model)BMC";
}
if (low.find(R"BMC(launder unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ launder unencoded: )BMC" + std::string(engine) + R"BMC( is not a lifetime model)BMC";
}
if (low.find(R"BMC(fold unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ fold unencoded: )BMC" + std::string(engine) + R"BMC( is not a fold-expression model)BMC";
}
if ((low.find(R"BMC(decimal-float unencoded)BMC") != std::string::npos || low.find(R"BMC(decimal float unencoded)BMC") != std::string::npos)) {
    return R"BMC(decimal float unencoded: )BMC" + std::string(engine) + R"BMC( is not a decimal-float model)BMC";
}
if ((low.find(R"BMC(extra-ieee unencoded)BMC") != std::string::npos || low.find(R"BMC(_float16)BMC") != std::string::npos || low.find(R"BMC(__fp16)BMC") != std::string::npos)) {
    return R"BMC(extra-IEEE float unencoded: )BMC" + std::string(engine) + R"BMC( is not an extra-IEEE model)BMC";
}
if (low.find(R"BMC(start_lifetime_as)BMC") != std::string::npos) {
    return R"BMC(C++ start_lifetime_as unencoded: )BMC" + std::string(engine) + R"BMC( is not a lifetime model)BMC";
}
if ((low.find(R"BMC(shared_from_this unencoded)BMC") != std::string::npos || low.find(R"BMC(shared-lifetime unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ shared_from_this unencoded: )BMC" + std::string(engine) + R"BMC( is not a shared-lifetime model)BMC";
}
if ((low.find(R"BMC(concepts unencoded)BMC") != std::string::npos || low.find(R"BMC(concept unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ concepts unencoded: )BMC" + std::string(engine) + R"BMC( is not a concepts model)BMC";
}
if ((low.find(R"BMC(choose_expr unencoded)BMC") != std::string::npos || low.find(R"BMC(__builtin_choose_expr)BMC") != std::string::npos)) {
    return R"BMC(choose_expr unencoded: )BMC" + std::string(engine) + R"BMC( is not a __builtin_choose_expr model)BMC";
}
if (rx_search(R"BMC(\b__builtin_unreachable\b)BMC", low)) {
    return R"BMC(__builtin_unreachable unencoded: unconstrained __builtin_unreachable is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b__builtin_trap\b)BMC", low)) {
    return R"BMC(__builtin_trap unencoded: unconstrained __builtin_trap is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bstd\s*::\s*unreachable\b)BMC", low)) {
    return R"BMC(C++ std::unreachable unencoded: )BMC" + std::string(engine) + R"BMC( is not an unreachable model)BMC";
}
if (low.find(R"BMC(restrict unencoded)BMC") != std::string::npos) {
    return R"BMC(restrict unencoded: )BMC" + std::string(engine) + R"BMC( is not a restrict-qualifier model)BMC";
}
if (low.find(R"BMC(listen unencoded)BMC") != std::string::npos) {
    return R"BMC(listen unencoded: unconstrained listen is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(connect unencoded)BMC") != std::string::npos) {
    return R"BMC(connect unencoded: unconstrained connect is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(pipe unencoded)BMC") != std::string::npos) {
    return R"BMC(pipe unencoded: )BMC" + std::string(engine) + R"BMC( is not a pipe model)BMC";
}
if (low.find(R"BMC(dup unencoded)BMC") != std::string::npos) {
    return R"BMC(dup unencoded: )BMC" + std::string(engine) + R"BMC( is not an fd model)BMC";
}
if (low.find(R"BMC(fcntl unencoded)BMC") != std::string::npos) {
    return R"BMC(fcntl unencoded: unconstrained fcntl is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bpd(?:getpid|wait4)\b)BMC", low)) {
    return R"BMC(pdgetpid unencoded: unconstrained pdgetpid is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:wait4|wait3)\b)BMC", low)) {
    return R"BMC(wait4 unencoded: unconstrained wait4 is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bwait6\b)BMC", low)) {
    return R"BMC(wait6 unencoded: unconstrained wait6 is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((rx_search(R"BMC(\bsem_trywait\b)BMC", low) || rx_search(R"BMC(\bsem_getvalue\b)BMC", low))) {
    return R"BMC(sem_trywait unencoded: unconstrained sem_trywait is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(wait unencoded)BMC") != std::string::npos) {
    return R"BMC(wait unencoded: unconstrained wait is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bunlinkat\b)BMC", low)) {
    return R"BMC(unlinkat unencoded: unconstrained unlinkat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bunlink\b)BMC", low)) {
    return R"BMC(unlink unencoded: unconstrained unlink is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bmknodat\b)BMC", low)) {
    return R"BMC(mknodat unencoded: unconstrained mknodat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(unexpected<)BMC") != std::string::npos || low.find(R"BMC(unexpected <)BMC") != std::string::npos || low.find(R"BMC(unexpected unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ unexpected unencoded: )BMC" + std::string(engine) + R"BMC( is not an unexpected model)BMC";
}
if ((low.find(R"BMC(expected unencoded)BMC") != std::string::npos || low.find(R"BMC(std::expected)BMC") != std::string::npos)) {
    return R"BMC(C++ std::expected unencoded: )BMC" + std::string(engine) + R"BMC( is not an expected model)BMC";
}
if (low.find(R"BMC(format_to)BMC") != std::string::npos) {
    return R"BMC(C++ format_to unencoded: )BMC" + std::string(engine) + R"BMC( is not a format_to model)BMC";
}
if ((low.find(R"BMC(format unencoded)BMC") != std::string::npos || low.find(R"BMC(std::format)BMC") != std::string::npos)) {
    return R"BMC(C++ std::format unencoded: )BMC" + std::string(engine) + R"BMC( is not a format model)BMC";
}
if (low.find(R"BMC(spaceship unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ spaceship unencoded: )BMC" + std::string(engine) + R"BMC( is not a three-way comparison model)BMC";
}
if (low.find(R"BMC(cleanup unencoded)BMC") != std::string::npos) {
    return R"BMC(cleanup attribute unencoded: )BMC" + std::string(engine) + R"BMC( is not a cleanup model)BMC";
}
if (low.find(R"BMC(jthread unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ std::jthread unencoded: )BMC" + std::string(engine) + R"BMC( is not a jthread model)BMC";
}
if ((low.find(R"BMC(async unencoded)BMC") != std::string::npos || low.find(R"BMC(std::async)BMC") != std::string::npos || low.find(R"BMC(future unencoded)BMC") != std::string::npos || low.find(R"BMC(promise unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::async unencoded: )BMC" + std::string(engine) + R"BMC( is not a future model)BMC";
}
if (low.find(R"BMC(function_ref)BMC") != std::string::npos) {
    return R"BMC(C++ function_ref unencoded: )BMC" + std::string(engine) + R"BMC( is not a function_ref model)BMC";
}
if ((low.find(R"BMC(move_only_function)BMC") != std::string::npos || low.find(R"BMC(copyable_function)BMC") != std::string::npos)) {
    return R"BMC(C++ move_only_function unencoded: )BMC" + std::string(engine) + R"BMC( is not a move_only_function model)BMC";
}
if ((rx_search(R"BMC(\breference_wrapper\b)BMC", low) || rx_search(R"BMC(\bstd\s*::\s*(?:cref|ref)\s*\()BMC", low))) {
    return R"BMC(C++ reference_wrapper unencoded: )BMC" + std::string(engine) + R"BMC( is not a reference_wrapper model)BMC";
}
if ((low.find(R"BMC(function unencoded)BMC") != std::string::npos || low.find(R"BMC(std::function)BMC") != std::string::npos)) {
    return R"BMC(C++ std::function unencoded: )BMC" + std::string(engine) + R"BMC( is not a type-erased callable model)BMC";
}
if ((low.find(R"BMC(mdspan unencoded)BMC") != std::string::npos || low.find(R"BMC(std::mdspan)BMC") != std::string::npos)) {
    return R"BMC(C++ std::mdspan unencoded: )BMC" + std::string(engine) + R"BMC( is not an mdspan model)BMC";
}
if ((low.find(R"BMC(std mutex unencoded)BMC") != std::string::npos || low.find(R"BMC(std::mutex)BMC") != std::string::npos)) {
    return R"BMC(C++ std::mutex unencoded: )BMC" + std::string(engine) + R"BMC( is not a C++ mutex model)BMC";
}
if (low.find(R"BMC(vector_size unencoded)BMC") != std::string::npos) {
    return R"BMC(vector_size unencoded: )BMC" + std::string(engine) + R"BMC( is not a SIMD vector model)BMC";
}
if (rx_search(R"BMC(\bppoll\b)BMC", low)) {
    return R"BMC(ppoll unencoded: unconstrained ppoll is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bepoll_pwait(?:2)?\b)BMC", low)) {
    return R"BMC(epoll_pwait unencoded: unconstrained epoll_pwait is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bepoll_create(?:1)?\b)BMC", low)) {
    return R"BMC(epoll_create unencoded: unconstrained epoll_create is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(select unencoded)BMC") != std::string::npos) {
    return R"BMC(select unencoded: unconstrained I/O multiplex is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(send unencoded)BMC") != std::string::npos) {
    return R"BMC(send unencoded: unconstrained socket I/O is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(kill_dependency)BMC") != std::string::npos) {
    return R"BMC(C++ kill_dependency unencoded: )BMC" + std::string(engine) + R"BMC( is not a kill_dependency model)BMC";
}
if (rx_search(R"BMC(\brt_(?:tgsigqueueinfo|sigqueueinfo)\b)BMC", low)) {
    return R"BMC(rt_sigqueueinfo unencoded: unconstrained rt_sigqueueinfo is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsigqueue\b)BMC", low)) {
    return R"BMC(sigqueue unencoded: unconstrained sigqueue is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bthr_(?:new|kill2|kill|self|exit|suspend|wake)\b)BMC", low)) {
    return R"BMC(thr_kill unencoded: unconstrained thr_kill is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bpthread_kill\b)BMC", low)) {
    return R"BMC(pthread_kill unencoded: unconstrained pthread_kill is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(kill unencoded)BMC") != std::string::npos) {
    return R"BMC(kill unencoded: unconstrained signal delivery is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(addrinfo unencoded)BMC") != std::string::npos) {
    return R"BMC(addrinfo unencoded: unconstrained DNS is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bpthread_cancel\b)BMC", low)) {
    return R"BMC(pthread_cancel unencoded: unconstrained pthread_cancel is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((rx_search(R"BMC(\bpthread_(?:key_create|key_delete|setspecific|getspecific)\b)BMC", low) || low.find(R"BMC(pthread_key_create unencoded)BMC") != std::string::npos)) {
    return R"BMC(pthread_key_create unencoded: unconstrained pthread_key_create is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(pthread join unencoded)BMC") != std::string::npos || rx_search(R"BMC(\b(?:pthread_join|pthread_detach)\b)BMC", low))) {
    return R"BMC(pthread join unencoded: unconstrained thread join is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((rx_search(R"BMC(\bpthread_spin_(?:try)?(?:lock|unlock|init|destroy)\b)BMC", low) || low.find(R"BMC(pthread_spin unencoded)BMC") != std::string::npos)) {
    return R"BMC(pthread_spin unencoded: unconstrained pthread_spin_lock is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((rx_search(R"BMC(\bpthread_rwlock_(?:try|timed)?(?:rdlock|wrlock|unlock|init|destroy)\b)BMC", low) || low.find(R"BMC(pthread_rwlock unencoded)BMC") != std::string::npos)) {
    return R"BMC(pthread_rwlock unencoded: unconstrained pthread_rwlock is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((rx_search(R"BMC(\bpthread_cond_(?:timedwait|wait|signal|broadcast|init|destroy)\b)BMC", low) || low.find(R"BMC(pthread_cond unencoded)BMC") != std::string::npos)) {
    return R"BMC(pthread_cond unencoded: unconstrained pthread_cond_wait is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(sem unencoded)BMC") != std::string::npos) {
    return R"BMC(sem unencoded: )BMC" + std::string(engine) + R"BMC( is not a semaphore model)BMC";
}
if (rx_search(R"BMC(\bksem_)BMC", low)) {
    return R"BMC(ksem_open unencoded: unconstrained ksem_open is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsem_(?:open|close|unlink)\b)BMC", low)) {
    return R"BMC(sem_open unencoded: unconstrained sem_open is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsem_timedwait\b)BMC", low)) {
    return R"BMC(sem_timedwait unencoded: unconstrained sem_timedwait is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(openat unencoded)BMC") != std::string::npos) {
    return R"BMC(openat unencoded: unconstrained openat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(flock unencoded)BMC") != std::string::npos) {
    return R"BMC(flock unencoded: unconstrained flock is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(aligned_alloc unencoded)BMC") != std::string::npos || low.find(R"BMC(posix_memalign)BMC") != std::string::npos)) {
    return R"BMC(aligned_alloc unencoded: )BMC" + std::string(engine) + R"BMC( is not an aligned-alloc model)BMC";
}
if (rx_search(R"BMC(\bvalloc\b)BMC", low)) {
    return R"BMC(valloc unencoded: unconstrained valloc is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bnotify_all_at_thread_exit\b)BMC", low)) {
    return R"BMC(C++ notify_all_at_thread_exit unencoded: )BMC" + std::string(engine) + R"BMC( is not a notify_all_at_thread_exit model)BMC";
}
if (low.find(R"BMC(condition_variable_any)BMC") != std::string::npos) {
    return R"BMC(C++ condition_variable_any unencoded: )BMC" + std::string(engine) + R"BMC( is not a condition_variable_any model)BMC";
}
if (low.find(R"BMC(shared_timed_mutex)BMC") != std::string::npos) {
    return R"BMC(C++ shared_timed_mutex unencoded: )BMC" + std::string(engine) + R"BMC( is not a shared_timed_mutex model)BMC";
}
if ((low.find(R"BMC(condition_variable unencoded)BMC") != std::string::npos || low.find(R"BMC(shared_mutex)BMC") != std::string::npos)) {
    return R"BMC(C++ std::condition_variable unencoded: )BMC" + std::string(engine) + R"BMC( is not a condvar/shared-mutex model)BMC";
}
if ((low.find(R"BMC(atomic_ref unencoded)BMC") != std::string::npos || low.find(R"BMC(std::atomic_ref)BMC") != std::string::npos)) {
    return R"BMC(C++ std::atomic_ref unencoded: )BMC" + std::string(engine) + R"BMC( is not an atomic_ref model)BMC";
}
if ((low.find(R"BMC(generator unencoded)BMC") != std::string::npos || low.find(R"BMC(std::generator)BMC") != std::string::npos)) {
    return R"BMC(C++ std::generator unencoded: )BMC" + std::string(engine) + R"BMC( is not a generator model)BMC";
}
if ((rx_search(R"BMC(\bassume_aligned\b)BMC", low) || low.find(R"BMC(assume_aligned unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ assume_aligned unencoded: )BMC" + std::string(engine) + R"BMC( is not an assume_aligned model)BMC";
}
if (low.find(R"BMC(assume unencoded)BMC") != std::string::npos) {
    return R"BMC(C++ assume unencoded: )BMC" + std::string(engine) + R"BMC( is not an assume-attribute model)BMC";
}
if ((low.find(R"BMC(std bind unencoded)BMC") != std::string::npos || low.find(R"BMC(std::bind)BMC") != std::string::npos)) {
    return R"BMC(C++ std::bind unencoded: )BMC" + std::string(engine) + R"BMC( is not a bind model)BMC";
}
if (low.find(R"BMC(atomic builtin unencoded)BMC") != std::string::npos) {
    return R"BMC(atomic builtin unencoded: )BMC" + std::string(engine) + R"BMC( is not an atomic-builtin model)BMC";
}
if (low.find(R"BMC(chown unencoded)BMC") != std::string::npos) {
    return R"BMC(chown unencoded: unconstrained chown is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsymlinkat\b)BMC", low)) {
    return R"BMC(symlinkat unencoded: unconstrained symlinkat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\breadlinkat\b)BMC", low)) {
    return R"BMC(readlinkat unencoded: unconstrained readlinkat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((rx_search(R"BMC(\b(?:symlink|readlink)\b)BMC", low) || (low.find(R"BMC(symlink unencoded)BMC") != std::string::npos || low.find(R"BMC(readlink unencoded)BMC") != std::string::npos))) {
    return R"BMC(symlink unencoded: unconstrained symlink/readlink is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bfts_(?:open|read|children|close|set)\b)BMC", low)) {
    return R"BMC(fts_open unencoded: unconstrained fts_open is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(opendir unencoded)BMC") != std::string::npos || low.find(R"BMC(dir* model)BMC") != std::string::npos)) {
    return R"BMC(opendir unencoded: )BMC" + std::string(engine) + R"BMC( is not a DIR* model)BMC";
}
if ((low.find(R"BMC(setrlimit unencoded)BMC") != std::string::npos || low.find(R"BMC(getrlimit unencoded)BMC") != std::string::npos)) {
    return R"BMC(setrlimit unencoded: unconstrained setrlimit is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:getsockname|getpeername)\b)BMC", low)) {
    return R"BMC(getsockname unencoded: unconstrained getsockname is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bgetpeereid\b)BMC", low)) {
    return R"BMC(getpeereid unencoded: unconstrained getpeereid is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(getsockopt unencoded)BMC") != std::string::npos || low.find(R"BMC(setsockopt unencoded)BMC") != std::string::npos)) {
    return R"BMC(getsockopt unencoded: unconstrained socket opts is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:listmount|statmount)\b)BMC", low)) {
    return R"BMC(listmount unencoded: unconstrained listmount/statmount is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(ustat)BMC") != std::string::npos) {
    return R"BMC(ustat unencoded: unconstrained ustat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bfile_(?:get|set)attr\b)BMC", low)) {
    return R"BMC(file_getattr unencoded: unconstrained file_getattr is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((rx_search(R"BMC(\bfh(?:linkat|link|readlink)\b)BMC", low) || rx_search(R"BMC(\bfhlink)BMC", low))) {
    return R"BMC(fhlink unencoded: unconstrained fhlink is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:getfh|fhopen|fhstatfs|fhstat|getfhat)\b)BMC", low)) {
    return R"BMC(getfh unencoded: unconstrained getfh is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bfstatat\b)BMC", low)) {
    return R"BMC(fstatat unencoded: unconstrained fstatat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bgetmntinfo\b)BMC", low)) {
    return R"BMC(getmntinfo unencoded: unconstrained getmntinfo is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bgetvfsbyname\b)BMC", low)) {
    return R"BMC(getvfsbyname unencoded: unconstrained getvfsbyname is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:get|set|end)fsent\b)BMC", low)) {
    return R"BMC(getfsent unencoded: unconstrained getfsent is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bgetfsstat\b)BMC", low)) {
    return R"BMC(getfsstat unencoded: unconstrained getfsstat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((rx_search(R"BMC(\b(?:lstat|fstat|stat)\b)BMC", low) || low.find(R"BMC(stat unencoded)BMC") != std::string::npos || low.find(R"BMC(lstat unencoded)BMC") != std::string::npos || low.find(R"BMC(fstat unencoded)BMC") != std::string::npos)) {
    return R"BMC(stat unencoded: unconstrained stat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\brenameat2\b)BMC", low)) {
    return R"BMC(renameat2 unencoded: unconstrained renameat2 is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\brenameat\b)BMC", low)) {
    return R"BMC(renameat unencoded: unconstrained renameat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bmkdirat\b)BMC", low)) {
    return R"BMC(mkdirat unencoded: unconstrained mkdirat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((rx_search(R"BMC(\b(?:mkdir|rmdir|rename)\b)BMC", low) || low.find(R"BMC(mkdir unencoded)BMC") != std::string::npos || low.find(R"BMC(rmdir unencoded)BMC") != std::string::npos || low.find(R"BMC(rename unencoded)BMC") != std::string::npos)) {
    return R"BMC(mkdir unencoded: unconstrained mkdir is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bcrypt_(?:newhash|checkpass)\b)BMC", low)) {
    return R"BMC(crypt_newhash unencoded: unconstrained crypt_newhash is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bkenv\b)BMC", low)) {
    return R"BMC(kenv unencoded: unconstrained kenv is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(getpwuid unencoded)BMC") != std::string::npos || low.find(R"BMC(getpwnam unencoded)BMC") != std::string::npos || low.find(R"BMC(crypt unencoded)BMC") != std::string::npos)) {
    return R"BMC(getpwuid unencoded: unconstrained getpwuid is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(std::any)BMC") != std::string::npos || low.find(R"BMC(any_cast)BMC") != std::string::npos || low.find(R"BMC(any unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::any unencoded: )BMC" + std::string(engine) + R"BMC( is not an any model)BMC";
}
if ((low.find(R"BMC(filesystem unencoded)BMC") != std::string::npos || low.find(R"BMC(std::filesystem)BMC") != std::string::npos || low.find(R"BMC(std::fs::)BMC") != std::string::npos)) {
    return R"BMC(C++ std::filesystem unencoded: )BMC" + std::string(engine) + R"BMC( is not a filesystem model)BMC";
}
if ((low.find(R"BMC(std::regex)BMC") != std::string::npos || low.find(R"BMC(regex unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::regex unencoded: )BMC" + std::string(engine) + R"BMC( is not a regex model)BMC";
}
if ((rx_search(R"BMC(\bpthread_barrier_(?:wait|init|destroy)\b)BMC", low) || low.find(R"BMC(pthread_barrier unencoded)BMC") != std::string::npos)) {
    return R"BMC(pthread_barrier unencoded: unconstrained pthread_barrier_wait is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(std::latch)BMC") != std::string::npos || low.find(R"BMC(std::barrier)BMC") != std::string::npos || low.find(R"BMC(counting_semaphore)BMC") != std::string::npos || low.find(R"BMC(latch unencoded)BMC") != std::string::npos || low.find(R"BMC(barrier unencoded)BMC") != std::string::npos || low.find(R"BMC(latch/barrier)BMC") != std::string::npos)) {
    return R"BMC(C++ std::latch/barrier unencoded: )BMC" + std::string(engine) + R"BMC( is not a sync primitive model)BMC";
}
if ((low.find(R"BMC(from_chars)BMC") != std::string::npos || low.find(R"BMC(charconv unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ from_chars unencoded: )BMC" + std::string(engine) + R"BMC( is not a charconv model)BMC";
}
if ((low.find(R"BMC(std::visit)BMC") != std::string::npos || low.find(R"BMC(visit unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::visit unencoded: )BMC" + std::string(engine) + R"BMC( is not a visitor model)BMC";
}
if (low.find(R"BMC(initializer_list)BMC") != std::string::npos) {
    return R"BMC(C++ initializer_list unencoded: )BMC" + std::string(engine) + R"BMC( is not a temporary-lifetime model)BMC";
}
if ((low.find(R"BMC(clock_settime)BMC") != std::string::npos || low.find(R"BMC(clock_adjtime)BMC") != std::string::npos || low.find(R"BMC(clock_nanosleep)BMC") != std::string::npos)) {
    return R"BMC(clock_settime unencoded: unconstrained clock_settime is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(settimeofday)BMC") != std::string::npos) {
    return R"BMC(settimeofday unencoded: unconstrained settimeofday is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bpthread_getcpuclockid\b)BMC", low)) {
    return R"BMC(pthread_getcpuclockid unencoded: unconstrained pthread_getcpuclockid is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bclock_getcpuclockid\b)BMC", low)) {
    return R"BMC(clock_getcpuclockid unencoded: unconstrained clock_getcpuclockid is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(clock_gettime)BMC") != std::string::npos || low.find(R"BMC(gettimeofday)BMC") != std::string::npos)) {
    return R"BMC(clock_gettime unencoded: unconstrained clock_gettime is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bposix_typed_mem_(?:open|get_info)\b)BMC", low)) {
    return R"BMC(posix_typed_mem_open unencoded: unconstrained posix_typed_mem_open is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(shmget)BMC") != std::string::npos) {
    return R"BMC(shmget unencoded: unconstrained shmget is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(shm_open)BMC") != std::string::npos || low.find(R"BMC(shm_unlink)BMC") != std::string::npos || low.find(R"BMC(shm unencoded)BMC") != std::string::npos)) {
    return R"BMC(shm unencoded: unconstrained shm_open is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bposix_spawn(?:_file_actions|attr)_init\b)BMC", low)) {
    return R"BMC(posix_spawn_file_actions_init unencoded: unconstrained posix_spawn_file_actions_init is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(posix_spawn)BMC") != std::string::npos) {
    return R"BMC(posix_spawn unencoded: unconstrained posix_spawn is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(glob unencoded)BMC") != std::string::npos || low.find(R"BMC(globfree)BMC") != std::string::npos)) {
    return R"BMC(glob unencoded: unconstrained glob is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(fseek unencoded)BMC") != std::string::npos || low.find(R"BMC(ftell unencoded)BMC") != std::string::npos || low.find(R"BMC(fgetpos)BMC") != std::string::npos || low.find(R"BMC(fsetpos)BMC") != std::string::npos)) {
    return R"BMC(fseek unencoded: unconstrained fseek is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(sleep unencoded)BMC") != std::string::npos || low.find(R"BMC(nanosleep)BMC") != std::string::npos || low.find(R"BMC(usleep)BMC") != std::string::npos)) {
    return R"BMC(sleep unencoded: unconstrained sleep is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bfchmodat2\b)BMC", low)) {
    return R"BMC(fchmodat2 unencoded: unconstrained fchmodat2 is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bfchmodat\b)BMC", low)) {
    return R"BMC(fchmodat unencoded: unconstrained fchmodat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:f|l)?chflags\b)BMC", low)) {
    return R"BMC(chflags unencoded: unconstrained chflags is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bfaccessat2\b)BMC", low)) {
    return R"BMC(faccessat2 unencoded: unconstrained faccessat2 is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bfaccessat\b)BMC", low)) {
    return R"BMC(faccessat unencoded: unconstrained faccessat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(vhangup)BMC") != std::string::npos) {
    return R"BMC(vhangup unencoded: unconstrained vhangup is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:at_)?quick_exit\b)BMC", low)) {
    return R"BMC(quick_exit unencoded: unconstrained quick_exit is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\beaccess\b)BMC", low)) {
    return R"BMC(eaccess unencoded: unconstrained eaccess is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(access unencoded)BMC") != std::string::npos) {
    return R"BMC(access unencoded: unconstrained access is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(source_location)BMC") != std::string::npos) {
    return R"BMC(C++ source_location unencoded: )BMC" + std::string(engine) + R"BMC( is not a source_location model)BMC";
}
if ((low.find(R"BMC(stacktrace unencoded)BMC") != std::string::npos || low.find(R"BMC(std::stacktrace)BMC") != std::string::npos)) {
    return R"BMC(C++ stacktrace unencoded: )BMC" + std::string(engine) + R"BMC( is not a stacktrace model)BMC";
}
if ((low.find(R"BMC(stop_token)BMC") != std::string::npos || low.find(R"BMC(stop_source)BMC") != std::string::npos || low.find(R"BMC(stop_callback)BMC") != std::string::npos)) {
    return R"BMC(C++ stop_token unencoded: )BMC" + std::string(engine) + R"BMC( is not a stop_token model)BMC";
}
if (low.find(R"BMC(flat_map)BMC") != std::string::npos) {
    return R"BMC(C++ flat_map unencoded: )BMC" + std::string(engine) + R"BMC( is not a flat_map model)BMC";
}
if (low.find(R"BMC(flat_set)BMC") != std::string::npos) {
    return R"BMC(C++ flat_set unencoded: )BMC" + std::string(engine) + R"BMC( is not a flat_set model)BMC";
}
if (low.find(R"BMC(flat_multiset)BMC") != std::string::npos) {
    return R"BMC(C++ flat_multiset unencoded: )BMC" + std::string(engine) + R"BMC( is not a flat_multiset model)BMC";
}
if (low.find(R"BMC(flat_multimap)BMC") != std::string::npos) {
    return R"BMC(C++ flat_multimap unencoded: )BMC" + std::string(engine) + R"BMC( is not a flat_multimap model)BMC";
}
if (low.find(R"BMC(zoned_time)BMC") != std::string::npos) {
    return R"BMC(C++ zoned_time unencoded: )BMC" + std::string(engine) + R"BMC( is not a zoned_time model)BMC";
}
if ((low.find(R"BMC(current_zone)BMC") != std::string::npos || rx_search(R"BMC(\btzdb\b)BMC", low))) {
    return R"BMC(C++ tzdb unencoded: )BMC" + std::string(engine) + R"BMC( is not a tzdb model)BMC";
}
if (low.find(R"BMC(chrono)BMC") != std::string::npos) {
    return R"BMC(C++ chrono unencoded: )BMC" + std::string(engine) + R"BMC( is not a chrono model)BMC";
}
if ((rx_search(R"BMC(\bzip_transform(?:_view)?\b)BMC", low) || low.find(R"BMC(zip_transform unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ zip_transform unencoded: )BMC" + std::string(engine) + R"BMC( is not a zip_transform model)BMC";
}
if ((low.find(R"BMC(views::zip)BMC") != std::string::npos || low.find(R"BMC(zip_view)BMC") != std::string::npos)) {
    return R"BMC(C++ views::zip unencoded: )BMC" + std::string(engine) + R"BMC( is not a views::zip model)BMC";
}
if ((rx_search(R"BMC(\bis_scoped_enum\b)BMC", low) || low.find(R"BMC(is_scoped_enum unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ is_scoped_enum unencoded: )BMC" + std::string(engine) + R"BMC( is not an is_scoped_enum model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*)?enumerate(?:_view)?\b)BMC", low) || low.find(R"BMC(enumerate unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ views::enumerate unencoded: )BMC" + std::string(engine) + R"BMC( is not a views::enumerate model)BMC";
}
if ((rx_search(R"BMC(\bcartesian_product(?:_view)?\b)BMC", low) || low.find(R"BMC(cartesian_product unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ cartesian_product unencoded: )BMC" + std::string(engine) + R"BMC( is not a cartesian_product model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*chunk(?:_by)?|chunk(?:_by|_view))\b)BMC", low) || low.find(R"BMC(chunk unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ views::chunk unencoded: )BMC" + std::string(engine) + R"BMC( is not a views::chunk model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*slide|slide_view)\b)BMC", low) || low.find(R"BMC(slide unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ views::slide unencoded: )BMC" + std::string(engine) + R"BMC( is not a views::slide model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*adjacent(?:_transform)?|adjacent(?:_transform|_view))\b)BMC", low) || low.find(R"BMC(adjacent unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ views::adjacent unencoded: )BMC" + std::string(engine) + R"BMC( is not a views::adjacent model)BMC";
}
if ((rx_search(R"BMC(\bjoin_with(?:_view)?\b)BMC", low) || low.find(R"BMC(join_with unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ join_with unencoded: )BMC" + std::string(engine) + R"BMC( is not a join_with model)BMC";
}
if ((low.find(R"BMC(views::join)BMC") != std::string::npos || low.find(R"BMC(join_view)BMC") != std::string::npos)) {
    return R"BMC(C++ views::join unencoded: )BMC" + std::string(engine) + R"BMC( is not a views::join model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*)?stride(?:_view)?\b)BMC", low) || low.find(R"BMC(stride unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ views::stride unencoded: )BMC" + std::string(engine) + R"BMC( is not a views::stride model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*repeat|repeat_view)\b)BMC", low) || low.find(R"BMC(repeat unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ views::repeat unencoded: )BMC" + std::string(engine) + R"BMC( is not a views::repeat model)BMC";
}
if ((rx_search(R"BMC(\btake_while(?:_view)?\b)BMC", low) || low.find(R"BMC(take_while unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ views::take_while unencoded: )BMC" + std::string(engine) + R"BMC( is not a take_while model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*take\b|\btake_view\b))BMC", low) || low.find(R"BMC(take unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ views::take unencoded: )BMC" + std::string(engine) + R"BMC( is not a views::take model)BMC";
}
if ((rx_search(R"BMC(\bdrop_while(?:_view)?\b)BMC", low) || low.find(R"BMC(drop_while unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ views::drop_while unencoded: )BMC" + std::string(engine) + R"BMC( is not a drop_while model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*drop\b|\bdrop_view\b))BMC", low) || low.find(R"BMC(drop unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ views::drop unencoded: )BMC" + std::string(engine) + R"BMC( is not a views::drop model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*keys\b|\bkeys_view\b))BMC", low) || low.find(R"BMC(keys unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ views::keys unencoded: )BMC" + std::string(engine) + R"BMC( is not a keys model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*values\b|\bvalues_view\b))BMC", low) || low.find(R"BMC(values unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ views::values unencoded: )BMC" + std::string(engine) + R"BMC( is not a values model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*reverse\b|\breverse_view\b))BMC", low) || (low.find(R"BMC(reverse_view unencoded)BMC") != std::string::npos || low.find(R"BMC(views::reverse unencoded)BMC") != std::string::npos))) {
    return R"BMC(C++ views::reverse unencoded: )BMC" + std::string(engine) + R"BMC( is not a reverse_view model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*counted\b|\bcounted_view\b))BMC", low) || (low.find(R"BMC(counted_view unencoded)BMC") != std::string::npos || low.find(R"BMC(views::counted unencoded)BMC") != std::string::npos))) {
    return R"BMC(C++ views::counted unencoded: )BMC" + std::string(engine) + R"BMC( is not a counted_view model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*filter\b|\bfilter_view\b))BMC", low) || low.find(R"BMC(filter unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ views::filter unencoded: )BMC" + std::string(engine) + R"BMC( is not a views::filter model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*transform\b|\btransform_view\b))BMC", low) || (low.find(R"BMC(transform_view unencoded)BMC") != std::string::npos || low.find(R"BMC(views::transform unencoded)BMC") != std::string::npos))) {
    return R"BMC(C++ views::transform unencoded: )BMC" + std::string(engine) + R"BMC( is not a transform_view model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*elements\b|\belements_view\b))BMC", low) || low.find(R"BMC(elements unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ views::elements unencoded: )BMC" + std::string(engine) + R"BMC( is not an elements model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*iota\b|\biota_view\b))BMC", low) || low.find(R"BMC(iota unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ views::iota unencoded: )BMC" + std::string(engine) + R"BMC( is not an iota model)BMC";
}
if ((low.find(R"BMC(ranges views)BMC") != std::string::npos || low.find(R"BMC(ranges-views)BMC") != std::string::npos || low.find(R"BMC(std::views)BMC") != std::string::npos || low.find(R"BMC(std::ranges::views)BMC") != std::string::npos)) {
    return R"BMC(C++ ranges views unencoded: )BMC" + std::string(engine) + R"BMC( is not a ranges-views model)BMC";
}
if (low.find(R"BMC(getopt)BMC") != std::string::npos) {
    return R"BMC(getopt unencoded: unconstrained getopt is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bgetosreldate\b)BMC", low)) {
    return R"BMC(getosreldate unencoded: unconstrained getosreldate is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bgetdomainname\b)BMC", low)) {
    return R"BMC(getdomainname unencoded: unconstrained getdomainname is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(uname)BMC") != std::string::npos || low.find(R"BMC(gethostname)BMC") != std::string::npos)) {
    return R"BMC(uname unencoded: unconstrained uname is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(sethostname)BMC") != std::string::npos) {
    return R"BMC(sethostname unencoded: unconstrained sethostname is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:preadv2|pwritev2|preadv|pwritev)\b)BMC", low)) {
    return R"BMC(preadv unencoded: unconstrained preadv is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(sendfile)BMC") != std::string::npos || low.find(R"BMC(copy_file_range)BMC") != std::string::npos)) {
    return R"BMC(sendfile unencoded: unconstrained sendfile is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(memfd_secret)BMC") != std::string::npos) {
    return R"BMC(memfd_secret unencoded: unconstrained memfd_secret is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:timerfd_settime|timerfd_gettime)\b)BMC", low)) {
    return R"BMC(timerfd_settime unencoded: unconstrained timerfd_settime is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\beventfd_(?:read|write)\b)BMC", low)) {
    return R"BMC(eventfd_read unencoded: unconstrained eventfd_read is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(memfd)BMC") != std::string::npos || low.find(R"BMC(eventfd)BMC") != std::string::npos || low.find(R"BMC(timerfd_create)BMC") != std::string::npos)) {
    return R"BMC(memfd unencoded: unconstrained memfd_create is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(arch_prctl)BMC") != std::string::npos) {
    return R"BMC(arch_prctl unencoded: unconstrained arch_prctl is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bprocctl\b)BMC", low)) {
    return R"BMC(procctl unencoded: unconstrained procctl is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bktrace\b)BMC", low)) {
    return R"BMC(ktrace unencoded: unconstrained ktrace is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(prctl)BMC") != std::string::npos || low.find(R"BMC(ptrace)BMC") != std::string::npos)) {
    return R"BMC(prctl unencoded: unconstrained prctl is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(tcgetattr)BMC") != std::string::npos || low.find(R"BMC(tcsetattr)BMC") != std::string::npos || low.find(R"BMC(cfmakeraw)BMC") != std::string::npos)) {
    return R"BMC(tcgetattr unencoded: unconstrained tcgetattr is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(module import unencoded)BMC") != std::string::npos || low.find(R"BMC(export module)BMC") != std::string::npos)) {
    return R"BMC(C++ module import unencoded: )BMC" + std::string(engine) + R"BMC( is not a modules model)BMC";
}
if (rx_search(R"BMC(\bgetpagesizes\b)BMC", low)) {
    return R"BMC(getpagesizes unencoded: unconstrained getpagesizes is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bgetpagesize(?!s)\b)BMC", low)) {
    return R"BMC(getpagesize unencoded: unconstrained getpagesize is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\blpathconf\b)BMC", low)) {
    return R"BMC(lpathconf unencoded: unconstrained lpathconf is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:sysconf|fpathconf|pathconf)\b)BMC", low)) {
    return R"BMC(sysconf unencoded: unconstrained sysconf is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(getrusage)BMC") != std::string::npos) {
    return R"BMC(getrusage unencoded: unconstrained getrusage is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(nftw)BMC") != std::string::npos || low.find(R"BMC(ftw unencoded)BMC") != std::string::npos)) {
    return R"BMC(nftw unencoded: unconstrained nftw is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(wordexp)BMC") != std::string::npos || low.find(R"BMC(wordfree)BMC") != std::string::npos)) {
    return R"BMC(wordexp unencoded: unconstrained wordexp is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:get|set)loginclass\b)BMC", low)) {
    return R"BMC(loginclass unencoded: unconstrained getloginclass is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:login_getclass|setusercontext)\b)BMC", low)) {
    return R"BMC(login_getclass unencoded: unconstrained login_getclass is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsetlogin\b)BMC", low)) {
    return R"BMC(setlogin unencoded: unconstrained setlogin is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((rx_search(R"BMC(\bgetlogin(?:_r)?\b)BMC", low) || rx_search(R"BMC(\bttyname(?:_r)?\b)BMC", low))) {
    return R"BMC(getlogin unencoded: unconstrained getlogin is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(inet_pton)BMC") != std::string::npos || low.find(R"BMC(inet_ntop)BMC") != std::string::npos || low.find(R"BMC(inet_aton)BMC") != std::string::npos)) {
    return R"BMC(inet_pton unencoded: unconstrained inet_pton is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bmseal\b)BMC", low)) {
    return R"BMC(mseal unencoded: unconstrained mseal is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bmlock2\b)BMC", low)) {
    return R"BMC(mlock2 unencoded: unconstrained mlock2 is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(munlockall)BMC") != std::string::npos || low.find(R"BMC(mlockall)BMC") != std::string::npos || low.find(R"BMC(munlock)BMC") != std::string::npos || low.find(R"BMC(mlock)BMC") != std::string::npos)) {
    return R"BMC(mlock unencoded: unconstrained mlock is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bposix_fadvise(?:64)?\b)BMC", low)) {
    return R"BMC(posix_fadvise unencoded: unconstrained posix_fadvise is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\breadahead\b)BMC", low)) {
    return R"BMC(readahead unencoded: unconstrained readahead is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(posix_madvise)BMC") != std::string::npos || (low.find(R"BMC(madvise)BMC") != std::string::npos && low.find(R"BMC(process_madvise)BMC") == std::string::npos))) {
    return R"BMC(madvise unencoded: unconstrained madvise is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(vmsplice)BMC") != std::string::npos || low.find(R"BMC(splice)BMC") != std::string::npos)) {
    return R"BMC(splice unencoded: unconstrained splice is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\binotify_rm_watch\b)BMC", low)) {
    return R"BMC(inotify_rm_watch unencoded: unconstrained inotify_rm_watch is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(inotify)BMC") != std::string::npos) {
    return R"BMC(inotify unencoded: unconstrained inotify is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(fdatasync)BMC") != std::string::npos || (low.find(R"BMC(fsync)BMC") != std::string::npos && low.find(R"BMC(syncfs)BMC") == std::string::npos))) {
    return R"BMC(fsync unencoded: unconstrained fsync is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(getrandom)BMC") != std::string::npos || low.find(R"BMC(getentropy)BMC") != std::string::npos)) {
    return R"BMC(getrandom unencoded: unconstrained getrandom is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\barc4random(?:_buf|_uniform)?\b)BMC", low)) {
    return R"BMC(arc4random unencoded: unconstrained arc4random is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bissetugid\b)BMC", low)) {
    return R"BMC(issetugid unencoded: unconstrained issetugid is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(getdelim)BMC") != std::string::npos || low.find(R"BMC(getline)BMC") != std::string::npos)) {
    return R"BMC(getline unencoded: unconstrained getline is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(vasprintf)BMC") != std::string::npos || low.find(R"BMC(asprintf)BMC") != std::string::npos)) {
    return R"BMC(asprintf unencoded: unconstrained asprintf is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(strlcpy)BMC") != std::string::npos || low.find(R"BMC(strlcat)BMC") != std::string::npos)) {
    return R"BMC(strlcpy unencoded: unconstrained strlcpy is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(explicit_bzero)BMC") != std::string::npos || low.find(R"BMC(memset_s)BMC") != std::string::npos || low.find(R"BMC(explicit_memset)BMC") != std::string::npos)) {
    return R"BMC(explicit_bzero unencoded: unconstrained explicit_bzero is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\btimingsafe_(?:bcmp|memcmp)\b)BMC", low)) {
    return R"BMC(timingsafe_bcmp unencoded: unconstrained timingsafe_bcmp is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(isatty)BMC") != std::string::npos) {
    return R"BMC(isatty unencoded: unconstrained isatty is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(ptsname)BMC") != std::string::npos || low.find(R"BMC(posix_openpt)BMC") != std::string::npos || low.find(R"BMC(grantpt)BMC") != std::string::npos || low.find(R"BMC(unlockpt)BMC") != std::string::npos)) {
    return R"BMC(ptsname unencoded: unconstrained ptsname is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bnmount\b)BMC", low)) {
    return R"BMC(nmount unencoded: unconstrained nmount is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bunmount\b)BMC", low)) {
    return R"BMC(unmount unencoded: unconstrained unmount is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:umount2|umount|mount)\b)BMC", low)) {
    return R"BMC(mount unencoded: unconstrained mount is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(open_wmemstream)BMC") != std::string::npos || low.find(R"BMC(open_memstream)BMC") != std::string::npos || low.find(R"BMC(fmemopen)BMC") != std::string::npos)) {
    return R"BMC(fmemopen unencoded: unconstrained fmemopen is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(scandir)BMC") != std::string::npos) {
    return R"BMC(scandir unencoded: unconstrained scandir is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bextattr_(?:set|get|delete|list)_(?:file|fd|link)\b)BMC", low)) {
    return R"BMC(extattr unencoded: unconstrained extattr_set_file is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:set|get|list|remove)xattrat\b)BMC", low)) {
    return R"BMC(setxattrat unencoded: unconstrained setxattrat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((rx_search(R"BMC(\b(?:lsetxattr|fsetxattr|setxattr)\b)BMC", low) || rx_search(R"BMC(\b(?:listxattr|removexattr|getxattr)\b)BMC", low))) {
    return R"BMC(setxattr unencoded: unconstrained setxattr is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:sched_setattr|sched_getattr)\b)BMC", low)) {
    return R"BMC(sched_setattr unencoded: unconstrained sched_setattr is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsched_yield\b)BMC", low)) {
    return R"BMC(sched_yield unencoded: unconstrained sched_yield is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bpthread_yield\b)BMC", low)) {
    return R"BMC(pthread_yield unencoded: unconstrained pthread_yield is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bcpuset_(?:set|get)affinity\b)BMC", low)) {
    return R"BMC(cpuset unencoded: unconstrained cpuset_setaffinity is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsched_get_priority_(?:max|min)\b)BMC", low)) {
    return R"BMC(sched_get_priority_max unencoded: unconstrained sched_get_priority_max is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(sched_setaffinity)BMC") != std::string::npos || low.find(R"BMC(sched_getaffinity)BMC") != std::string::npos)) {
    return R"BMC(sched unencoded: unconstrained sched_setaffinity is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(sched_setscheduler)BMC") != std::string::npos || low.find(R"BMC(sched_getscheduler)BMC") != std::string::npos || low.find(R"BMC(sched_setparam)BMC") != std::string::npos || low.find(R"BMC(sched_getparam)BMC") != std::string::npos)) {
    return R"BMC(sched_setscheduler unencoded: unconstrained sched_setscheduler is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\blio_listio\b)BMC", low)) {
    return R"BMC(lio_listio unencoded: unconstrained lio_listio is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(aio_read)BMC") != std::string::npos || low.find(R"BMC(aio_write)BMC") != std::string::npos || low.find(R"BMC(aio_error)BMC") != std::string::npos || low.find(R"BMC(aio_return)BMC") != std::string::npos || low.find(R"BMC(aio_suspend)BMC") != std::string::npos || low.find(R"BMC(aio unencoded)BMC") != std::string::npos)) {
    return R"BMC(aio unencoded: unconstrained aio_read is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(io_uring)BMC") != std::string::npos) {
    return R"BMC(io_uring unencoded: unconstrained io_uring_setup is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(capset)BMC") != std::string::npos || low.find(R"BMC(capget)BMC") != std::string::npos)) {
    return R"BMC(capset unencoded: unconstrained capset is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(statx)BMC") != std::string::npos) {
    return R"BMC(statx unencoded: unconstrained statx is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(pidfd)BMC") != std::string::npos) {
    return R"BMC(pidfd unencoded: unconstrained pidfd_open is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(fanotify)BMC") != std::string::npos) {
    return R"BMC(fanotify unencoded: unconstrained fanotify_init is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(seccomp)BMC") != std::string::npos) {
    return R"BMC(seccomp unencoded: unconstrained seccomp is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(getgrnam)BMC") != std::string::npos || low.find(R"BMC(getgrgid)BMC") != std::string::npos || low.find(R"BMC(getspnam)BMC") != std::string::npos)) {
    return R"BMC(getgrnam unencoded: unconstrained getgrnam is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(fallocate)BMC") != std::string::npos) {
    return R"BMC(fallocate unencoded: unconstrained posix_fallocate is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(close_range)BMC") != std::string::npos) {
    return R"BMC(close_range unencoded: unconstrained close_range is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bclosefrom\b)BMC", low)) {
    return R"BMC(closefrom unencoded: unconstrained closefrom is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\blsm_(?:get_self_attr|set_self_attr|list_modules)\b)BMC", low)) {
    return R"BMC(lsm_get_self_attr unencoded: unconstrained lsm_get_self_attr is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(landlock)BMC") != std::string::npos) {
    return R"BMC(landlock unencoded: unconstrained landlock_create_ruleset is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\brtprio(?:_thread)?\b)BMC", low)) {
    return R"BMC(rtprio unencoded: unconstrained rtprio is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(getpriority)BMC") != std::string::npos || low.find(R"BMC(setpriority)BMC") != std::string::npos)) {
    return R"BMC(getpriority unencoded: unconstrained getpriority is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(signalfd)BMC") != std::string::npos) {
    return R"BMC(signalfd unencoded: unconstrained signalfd is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsigaction\b)BMC", low)) {
    return R"BMC(sigaction unencoded: unconstrained sigaction is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bpthread_sigmask\b)BMC", low)) {
    return R"BMC(pthread_sigmask unencoded: unconstrained pthread_sigmask is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsig(?:procmask|suspend)\b)BMC", low)) {
    return R"BMC(sigprocmask unencoded: unconstrained sigprocmask is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsig(?:waitinfo|timedwait|pending|wait)\b)BMC", low)) {
    return R"BMC(sigwait unencoded: unconstrained sigwait is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsigaltstack\b)BMC", low)) {
    return R"BMC(sigaltstack unencoded: unconstrained sigaltstack is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(bpf)BMC") != std::string::npos) {
    return R"BMC(bpf unencoded: unconstrained bpf is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(userfaultfd)BMC") != std::string::npos) {
    return R"BMC(userfaultfd unencoded: unconstrained userfaultfd is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(getpass)BMC") != std::string::npos) {
    return R"BMC(getpass unencoded: unconstrained getpass is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bgetgrouplist\b)BMC", low)) {
    return R"BMC(getgrouplist unencoded: unconstrained getgrouplist is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bgetgroups\b)BMC", low)) {
    return R"BMC(getgroups unencoded: unconstrained getgroups is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(initgroups)BMC") != std::string::npos || low.find(R"BMC(setgroups)BMC") != std::string::npos)) {
    return R"BMC(initgroups unencoded: unconstrained initgroups is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(unshare)BMC") != std::string::npos || low.find(R"BMC(setns)BMC") != std::string::npos || low.find(R"BMC(clone()BMC") != std::string::npos || low.find(R"BMC(clone ()BMC") != std::string::npos)) {
    return R"BMC(unshare unencoded: unconstrained unshare/clone is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(openat2)BMC") != std::string::npos) {
    return R"BMC(openat2 unencoded: unconstrained openat2 is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:sendmsg|recvmsg)\b)BMC", low)) {
    return R"BMC(sendmsg unencoded: unconstrained sendmsg is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(sendmmsg)BMC") != std::string::npos || low.find(R"BMC(recvmmsg)BMC") != std::string::npos)) {
    return R"BMC(sendmmsg unencoded: unconstrained sendmmsg is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(name_to_handle)BMC") != std::string::npos || low.find(R"BMC(open_by_handle)BMC") != std::string::npos)) {
    return R"BMC(name_to_handle unencoded: unconstrained name_to_handle_at is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(process_madvise)BMC") != std::string::npos) {
    return R"BMC(process_madvise unencoded: unconstrained process_madvise is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(personality)BMC") != std::string::npos) {
    return R"BMC(personality unencoded: unconstrained personality is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(quotactl)BMC") != std::string::npos) {
    return R"BMC(quotactl unencoded: unconstrained quotactl is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(pivot_root)BMC") != std::string::npos) {
    return R"BMC(pivot_root unencoded: unconstrained pivot_root is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(membarrier)BMC") != std::string::npos) {
    return R"BMC(membarrier unencoded: unconstrained membarrier is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(pkey_alloc)BMC") != std::string::npos) {
    return R"BMC(pkey_alloc unencoded: unconstrained pkey_alloc is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(statfs)BMC") != std::string::npos) {
    return R"BMC(statfs unencoded: unconstrained statfs is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(syncfs)BMC") != std::string::npos) {
    return R"BMC(syncfs unencoded: unconstrained syncfs is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(prlimit)BMC") != std::string::npos) {
    return R"BMC(prlimit unencoded: unconstrained prlimit is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:migrate_pages|move_pages)\b)BMC", low)) {
    return R"BMC(move_pages unencoded: unconstrained move_pages is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(process_vm_readv)BMC") != std::string::npos) {
    return R"BMC(process_vm_readv unencoded: unconstrained process_vm_readv is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(perf_event_open)BMC") != std::string::npos) {
    return R"BMC(perf_event_open unencoded: unconstrained perf_event_open is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(clone3)BMC") != std::string::npos) {
    return R"BMC(clone3 unencoded: unconstrained clone3 is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(kcmp)BMC") != std::string::npos) {
    return R"BMC(kcmp unencoded: unconstrained kcmp is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(keyctl)BMC") != std::string::npos) {
    return R"BMC(keyctl unencoded: unconstrained keyctl is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bopen_tree_attr\b)BMC", low)) {
    return R"BMC(open_tree_attr unencoded: unconstrained open_tree_attr is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(fsopen)BMC") != std::string::npos || low.find(R"BMC(fsmount)BMC") != std::string::npos || rx_search(R"BMC(\bopen_tree\b)BMC", low) || low.find(R"BMC(move_mount)BMC") != std::string::npos || low.find(R"BMC(fspick)BMC") != std::string::npos || low.find(R"BMC(fsconfig)BMC") != std::string::npos)) {
    return R"BMC(fsopen unencoded: unconstrained fsopen is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(process_mrelease)BMC") != std::string::npos) {
    return R"BMC(process_mrelease unencoded: unconstrained process_mrelease is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(ioprio)BMC") != std::string::npos) {
    return R"BMC(ioprio unencoded: unconstrained ioprio is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(mq_open)BMC") != std::string::npos) {
    return R"BMC(mq_open unencoded: unconstrained mq_open is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b_umtx_op\b)BMC", low)) {
    return R"BMC(_umtx_op unencoded: unconstrained _umtx_op is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bfutex_waitv\b)BMC", low)) {
    return R"BMC(futex_waitv unencoded: unconstrained futex_waitv is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bfutex_(?:wake|wait|requeue)\b)BMC", low)) {
    return R"BMC(futex_wait unencoded: unconstrained futex_wait is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bfutex\b)BMC", low)) {
    return R"BMC(futex unencoded: unconstrained futex is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(adjtimex)BMC") != std::string::npos) {
    return R"BMC(adjtimex unencoded: unconstrained adjtimex is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:ntp_)?adjtime\b)BMC", low)) {
    return R"BMC(adjtime unencoded: unconstrained adjtime is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bntp_gettime\b)BMC", low)) {
    return R"BMC(ntp_gettime unencoded: unconstrained ntp_gettime is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\brevoke\b)BMC", low)) {
    return R"BMC(revoke unencoded: unconstrained revoke is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bjail(?:_attach|_get|_set|_remove)?\b)BMC", low)) {
    return R"BMC(jail unencoded: unconstrained jail is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:fflagstostr|strtofflags)\b)BMC", low)) {
    return R"BMC(fflagstostr unencoded: unconstrained fflagstostr is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bstrmode\b)BMC", low)) {
    return R"BMC(strmode unencoded: unconstrained strmode is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bstrtonum\b)BMC", low)) {
    return R"BMC(strtonum unencoded: unconstrained strtonum is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\breallocarray\b)BMC", low)) {
    return R"BMC(reallocarray unencoded: unconstrained reallocarray is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\breallocf\b)BMC", low)) {
    return R"BMC(reallocf unencoded: unconstrained reallocf is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:get|set)progname\b)BMC", low)) {
    return R"BMC(getprogname unencoded: unconstrained getprogname is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:setproctitle|daemon)\b)BMC", low)) {
    return R"BMC(daemon unencoded: unconstrained daemon is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:auditon|getaudit|setaudit|auditctl)\b)BMC", low)) {
    return R"BMC(auditon unencoded: unconstrained auditon is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bkinfo_get(?:proc|file|vmmap)\b)BMC", low)) {
    return R"BMC(kinfo_getproc unencoded: unconstrained kinfo_getproc is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bkvm_(?:open|openfiles|getprocs|close|nlist)\b)BMC", low)) {
    return R"BMC(kvm_open unencoded: unconstrained kvm_open is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\buuidgen\b)BMC", low)) {
    return R"BMC(uuidgen unencoded: unconstrained uuidgen is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsetfib\b)BMC", low)) {
    return R"BMC(setfib unencoded: unconstrained setfib is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(reboot)BMC") != std::string::npos) {
    return R"BMC(reboot unencoded: unconstrained reboot is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(swapon)BMC") != std::string::npos || low.find(R"BMC(swapoff)BMC") != std::string::npos)) {
    return R"BMC(swapon unencoded: unconstrained swapon is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(acct)BMC") != std::string::npos) {
    return R"BMC(acct unencoded: unconstrained acct is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(ioperm)BMC") != std::string::npos || rx_search(R"BMC(\biopl\b)BMC", low))) {
    return R"BMC(ioperm unencoded: unconstrained ioperm is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(mincore)BMC") != std::string::npos) {
    return R"BMC(mincore unencoded: unconstrained mincore is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\brseq\b)BMC", low)) {
    return R"BMC(rseq unencoded: unconstrained rseq is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(timer_create)BMC") != std::string::npos && low.find(R"BMC(timerfd)BMC") == std::string::npos)) {
    return R"BMC(timer_create unencoded: unconstrained timer_create is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(semget)BMC") != std::string::npos) {
    return R"BMC(semget unencoded: unconstrained semget is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(msgget)BMC") != std::string::npos) {
    return R"BMC(msgget unencoded: unconstrained msgget is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsyslog\b)BMC", low)) {
    return R"BMC(syslog unencoded: unconstrained syslog is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(klogctl)BMC") != std::string::npos) {
    return R"BMC(klogctl unencoded: unconstrained klogctl is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(mount_setattr)BMC") != std::string::npos) {
    return R"BMC(mount_setattr unencoded: unconstrained mount_setattr is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bgetcpu\b)BMC", low)) {
    return R"BMC(getcpu unencoded: unconstrained getcpu is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(init_module)BMC") != std::string::npos || low.find(R"BMC(finit_module)BMC") != std::string::npos || low.find(R"BMC(delete_module)BMC") != std::string::npos)) {
    return R"BMC(init_module unencoded: unconstrained init_module is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(kexec)BMC") != std::string::npos) {
    return R"BMC(kexec unencoded: unconstrained kexec_load is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(quotactl_fd)BMC") != std::string::npos) {
    return R"BMC(quotactl_fd unencoded: unconstrained quotactl_fd is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(pkey_free)BMC") != std::string::npos || low.find(R"BMC(pkey_mprotect)BMC") != std::string::npos)) {
    return R"BMC(pkey_free unencoded: unconstrained pkey_free is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(tgkill)BMC") != std::string::npos) {
    return R"BMC(tgkill unencoded: unconstrained tgkill is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(add_key)BMC") != std::string::npos) {
    return R"BMC(add_key unencoded: unconstrained add_key is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(semctl)BMC") != std::string::npos) {
    return R"BMC(semctl unencoded: unconstrained semctl is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(msgctl)BMC") != std::string::npos) {
    return R"BMC(msgctl unencoded: unconstrained msgctl is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(shmctl)BMC") != std::string::npos) {
    return R"BMC(shmctl unencoded: unconstrained shmctl is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(timer_settime)BMC") != std::string::npos) {
    return R"BMC(timer_settime unencoded: unconstrained timer_settime is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(setdomainname)BMC") != std::string::npos) {
    return R"BMC(setdomainname unencoded: unconstrained setdomainname is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(io_submit)BMC") != std::string::npos || low.find(R"BMC(io_getevents)BMC") != std::string::npos)) {
    return R"BMC(io_submit unencoded: unconstrained io_submit is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:io_setup|io_destroy|io_cancel|io_pgetevents)\b)BMC", low)) {
    return R"BMC(io_setup unencoded: unconstrained io_setup is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(request_key)BMC") != std::string::npos) {
    return R"BMC(request_key unencoded: unconstrained request_key is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\btkill\b)BMC", low)) {
    return R"BMC(tkill unencoded: unconstrained tkill is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(timer_delete)BMC") != std::string::npos || low.find(R"BMC(timer_gettime)BMC") != std::string::npos || low.find(R"BMC(timer_getoverrun)BMC") != std::string::npos)) {
    return R"BMC(timer_delete unencoded: unconstrained timer_delete is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(mq_unlink)BMC") != std::string::npos || low.find(R"BMC(mq_timedsend)BMC") != std::string::npos || low.find(R"BMC(mq_timedreceive)BMC") != std::string::npos || low.find(R"BMC(mq_notify)BMC") != std::string::npos || low.find(R"BMC(mq_getsetattr)BMC") != std::string::npos)) {
    return R"BMC(mq_unlink unencoded: unconstrained mq_unlink is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:shmat|shmdt)\b)BMC", low)) {
    return R"BMC(shmat unencoded: unconstrained shmat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:semop|semtimedop)\b)BMC", low)) {
    return R"BMC(semop unencoded: unconstrained semop is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:msgsnd|msgrcv)\b)BMC", low)) {
    return R"BMC(msgsnd unencoded: unconstrained msgsnd is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(sync_file_range)BMC") != std::string::npos) {
    return R"BMC(sync_file_range unencoded: unconstrained sync_file_range is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bremap_file_pages\b)BMC", low)) {
    return R"BMC(remap_file_pages unencoded: unconstrained remap_file_pages is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:msync|mremap)\b)BMC", low)) {
    return R"BMC(msync unencoded: unconstrained msync is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsocketpair\b)BMC", low)) {
    return R"BMC(socketpair unencoded: unconstrained socketpair is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bsysinfo\b)BMC", low)) {
    return R"BMC(sysinfo unencoded: unconstrained sysinfo is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bgettid\b)BMC", low)) {
    return R"BMC(gettid unencoded: unconstrained gettid is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(setitimer)BMC") != std::string::npos || low.find(R"BMC(getitimer)BMC") != std::string::npos)) {
    return R"BMC(setitimer unencoded: unconstrained setitimer is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bnice\b)BMC", low)) {
    return R"BMC(nice unencoded: unconstrained nice is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bgetdirentries\b)BMC", low)) {
    return R"BMC(getdirentries unencoded: unconstrained getdirentries is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bgetdents(?:64)?\b)BMC", low)) {
    return R"BMC(getdents unencoded: unconstrained getdents is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(utimensat)BMC") != std::string::npos || low.find(R"BMC(futimens)BMC") != std::string::npos || low.find(R"BMC(utimes)BMC") != std::string::npos)) {
    return R"BMC(utimensat unencoded: unconstrained utimensat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\blinkat\b)BMC", low)) {
    return R"BMC(linkat unencoded: unconstrained linkat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bset_mempolicy_home_node\b)BMC", low)) {
    return R"BMC(set_mempolicy_home_node unencoded: unconstrained set_mempolicy_home_node is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(mbind)BMC") != std::string::npos || rx_search(R"BMC(\bset_mempolicy\b)BMC", low) || rx_search(R"BMC(\bget_mempolicy\b)BMC", low))) {
    return R"BMC(mbind unencoded: unconstrained mbind is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:setreuid|setregid|setresuid|setresgid)\b)BMC", low)) {
    return R"BMC(setreuid unencoded: unconstrained setreuid is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bgetres(?:uid|gid)\b)BMC", low)) {
    return R"BMC(getresuid unencoded: unconstrained getresuid is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:setfsuid|setfsgid)\b)BMC", low)) {
    return R"BMC(setfsuid unencoded: unconstrained setfsuid is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\b(?:setpgid|setsid|getsid)\b)BMC", low)) {
    return R"BMC(setpgid unencoded: unconstrained setpgid is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bcachestat\b)BMC", low)) {
    return R"BMC(cachestat unencoded: unconstrained cachestat is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (rx_search(R"BMC(\bmap_shadow_stack\b)BMC", low)) {
    return R"BMC(map_shadow_stack unencoded: unconstrained map_shadow_stack is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if ((low.find(R"BMC(std::pmr)BMC") != std::string::npos || low.find(R"BMC(pmr::)BMC") != std::string::npos)) {
    return R"BMC(C++ pmr unencoded: )BMC" + std::string(engine) + R"BMC( is not a pmr model)BMC";
}
if (low.find(R"BMC(u8string)BMC") != std::string::npos) {
    return R"BMC(C++ u8string unencoded: )BMC" + std::string(engine) + R"BMC( is not a u8string model)BMC";
}
if (low.find(R"BMC(unordered_multimap)BMC") != std::string::npos) {
    return R"BMC(C++ unordered_multimap unencoded: )BMC" + std::string(engine) + R"BMC( is not an unordered_multimap model)BMC";
}
if (low.find(R"BMC(unordered_multiset)BMC") != std::string::npos) {
    return R"BMC(C++ unordered_multiset unencoded: )BMC" + std::string(engine) + R"BMC( is not an unordered_multiset model)BMC";
}
if (low.find(R"BMC(shared_lock)BMC") != std::string::npos) {
    return R"BMC(C++ shared_lock unencoded: )BMC" + std::string(engine) + R"BMC( is not a shared_lock model)BMC";
}
if (rx_search(R"BMC(\batomic_(?:thread|signal)_fence\b)BMC", low)) {
    return R"BMC(C++ atomic_thread_fence unencoded: )BMC" + std::string(engine) + R"BMC( is not an atomic_thread_fence model)BMC";
}
if (low.find(R"BMC(atomic_flag)BMC") != std::string::npos) {
    return R"BMC(C++ atomic_flag unencoded: )BMC" + std::string(engine) + R"BMC( is not an atomic_flag model)BMC";
}
if (low.find(R"BMC(recursive_timed_mutex)BMC") != std::string::npos) {
    return R"BMC(C++ recursive_timed_mutex unencoded: )BMC" + std::string(engine) + R"BMC( is not a recursive_timed_mutex model)BMC";
}
if (low.find(R"BMC(recursive_mutex)BMC") != std::string::npos) {
    return R"BMC(C++ recursive_mutex unencoded: )BMC" + std::string(engine) + R"BMC( is not a recursive_mutex model)BMC";
}
if (rx_search(R"BMC(\btimed_mutex\b)BMC", low)) {
    return R"BMC(C++ timed_mutex unencoded: )BMC" + std::string(engine) + R"BMC( is not a timed_mutex model)BMC";
}
if (rx_search(R"BMC(\b(?:ifstream|ofstream|fstream)\b)BMC", low)) {
    return R"BMC(C++ fstream unencoded: )BMC" + std::string(engine) + R"BMC( is not an fstream model)BMC";
}
if (low.find(R"BMC(this_thread)BMC") != std::string::npos) {
    return R"BMC(C++ this_thread unencoded: )BMC" + std::string(engine) + R"BMC( is not a this_thread model)BMC";
}
if (rx_search(R"BMC(\bcall_once\b)BMC", low)) {
    return R"BMC(C++ call_once unencoded: )BMC" + std::string(engine) + R"BMC( is not a call_once model)BMC";
}
if (low.find(R"BMC(tuple)BMC") != std::string::npos) {
    return R"BMC(C++ tuple unencoded: )BMC" + std::string(engine) + R"BMC( is not a tuple model)BMC";
}
if (low.find(R"BMC(deque)BMC") != std::string::npos) {
    return R"BMC(C++ deque unencoded: )BMC" + std::string(engine) + R"BMC( is not a deque model)BMC";
}
if (low.find(R"BMC(forward_list)BMC") != std::string::npos) {
    return R"BMC(C++ forward_list unencoded: )BMC" + std::string(engine) + R"BMC( is not a forward_list model)BMC";
}
if ((low.find(R"BMC(std::list)BMC") != std::string::npos || (low.find(R"BMC(list<)BMC") != std::string::npos && low.find(R"BMC(initializer_list)BMC") == std::string::npos && low.find(R"BMC(forward_list)BMC") == std::string::npos))) {
    return R"BMC(C++ std::list unencoded: )BMC" + std::string(engine) + R"BMC( is not a list model)BMC";
}
if (low.find(R"BMC(unordered_map)BMC") != std::string::npos) {
    return R"BMC(C++ unordered_map unencoded: )BMC" + std::string(engine) + R"BMC( is not an unordered_map model)BMC";
}
if ((low.find(R"BMC(std::map)BMC") != std::string::npos || (low.find(R"BMC(map<)BMC") != std::string::npos && low.find(R"BMC(flat_map)BMC") == std::string::npos && low.find(R"BMC(unordered_map)BMC") == std::string::npos && low.find(R"BMC(multimap)BMC") == std::string::npos))) {
    return R"BMC(C++ std::map unencoded: )BMC" + std::string(engine) + R"BMC( is not a map model)BMC";
}
if (low.find(R"BMC(unordered_set)BMC") != std::string::npos) {
    return R"BMC(C++ unordered_set unencoded: )BMC" + std::string(engine) + R"BMC( is not an unordered_set model)BMC";
}
if ((low.find(R"BMC(std::set)BMC") != std::string::npos || (low.find(R"BMC(set<)BMC") != std::string::npos && low.find(R"BMC(flat_set)BMC") == std::string::npos && low.find(R"BMC(unordered_set)BMC") == std::string::npos && low.find(R"BMC(multiset)BMC") == std::string::npos))) {
    return R"BMC(C++ std::set unencoded: )BMC" + std::string(engine) + R"BMC( is not a set model)BMC";
}
if (low.find(R"BMC(priority_queue)BMC") != std::string::npos) {
    return R"BMC(C++ priority_queue unencoded: )BMC" + std::string(engine) + R"BMC( is not a priority_queue model)BMC";
}
if ((low.find(R"BMC(std::queue)BMC") != std::string::npos || (low.find(R"BMC(queue<)BMC") != std::string::npos && low.find(R"BMC(priority_queue)BMC") == std::string::npos && low.find(R"BMC(deque)BMC") == std::string::npos))) {
    return R"BMC(C++ std::queue unencoded: )BMC" + std::string(engine) + R"BMC( is not a queue model)BMC";
}
if (((low.find(R"BMC(std::stack)BMC") != std::string::npos || low.find(R"BMC(stack<)BMC") != std::string::npos) && low.find(R"BMC(stacktrace)BMC") == std::string::npos)) {
    return R"BMC(C++ std::stack unencoded: )BMC" + std::string(engine) + R"BMC( is not a stack model)BMC";
}
if (low.find(R"BMC(to_array)BMC") != std::string::npos) {
    return R"BMC(C++ to_array unencoded: )BMC" + std::string(engine) + R"BMC( is not a to_array model)BMC";
}
if ((rx_search(R"BMC(\bfrom_range\b)BMC", low) || low.find(R"BMC(from_range unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ from_range unencoded: )BMC" + std::string(engine) + R"BMC( is not a from_range model)BMC";
}
if ((rx_search(R"BMC(\branges\s*::\s*to\b)BMC", low) || low.find(R"BMC(ranges::to unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ ranges::to unencoded: )BMC" + std::string(engine) + R"BMC( is not a ranges::to model)BMC";
}
if ((low.find(R"BMC(std::array)BMC") != std::string::npos || (low.find(R"BMC(array<)BMC") != std::string::npos && low.find(R"BMC(valarray)BMC") == std::string::npos))) {
    return R"BMC(C++ std::array unencoded: )BMC" + std::string(engine) + R"BMC( is not an array model)BMC";
}
if (low.find(R"BMC(wstring_convert)BMC") != std::string::npos) {
    return R"BMC(C++ wstring_convert unencoded: )BMC" + std::string(engine) + R"BMC( is not a wstring_convert model)BMC";
}
if (low.find(R"BMC(wstring)BMC") != std::string::npos) {
    return R"BMC(C++ wstring unencoded: )BMC" + std::string(engine) + R"BMC( is not a wstring model)BMC";
}
if ((low.find(R"BMC(multimap)BMC") != std::string::npos && low.find(R"BMC(flat_multimap)BMC") == std::string::npos)) {
    return R"BMC(C++ multimap unencoded: )BMC" + std::string(engine) + R"BMC( is not a multimap model)BMC";
}
if ((low.find(R"BMC(multiset)BMC") != std::string::npos && low.find(R"BMC(flat_multiset)BMC") == std::string::npos)) {
    return R"BMC(C++ multiset unencoded: )BMC" + std::string(engine) + R"BMC( is not a multiset model)BMC";
}
if (low.find(R"BMC(binary_semaphore)BMC") != std::string::npos) {
    return R"BMC(C++ binary_semaphore unencoded: )BMC" + std::string(engine) + R"BMC( is not a binary_semaphore model)BMC";
}
if (low.find(R"BMC(error_category)BMC") != std::string::npos) {
    return R"BMC(C++ error_category unencoded: )BMC" + std::string(engine) + R"BMC( is not an error_category model)BMC";
}
if (low.find(R"BMC(system_error)BMC") != std::string::npos) {
    return R"BMC(C++ system_error unencoded: )BMC" + std::string(engine) + R"BMC( is not a system_error model)BMC";
}
if (low.find(R"BMC(error_code)BMC") != std::string::npos) {
    return R"BMC(C++ error_code unencoded: )BMC" + std::string(engine) + R"BMC( is not an error_code model)BMC";
}
if (low.find(R"BMC(std::apply)BMC") != std::string::npos) {
    return R"BMC(C++ std::apply unencoded: )BMC" + std::string(engine) + R"BMC( is not an apply model)BMC";
}
if (low.find(R"BMC(std::invoke)BMC") != std::string::npos) {
    return R"BMC(C++ std::invoke unencoded: )BMC" + std::string(engine) + R"BMC( is not an invoke model)BMC";
}
if ((rx_search(R"BMC(\bstd\s*::\s*endian\b)BMC", low) || low.find(R"BMC(endian unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::endian unencoded: )BMC" + std::string(engine) + R"BMC( is not an endian model)BMC";
}
if ((rx_search(R"BMC(\bstd\s*::\s*rot[lr]\b)BMC", low) || low.find(R"BMC(rotl unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::rotl unencoded: )BMC" + std::string(engine) + R"BMC( is not a rotl model)BMC";
}
if ((low.find(R"BMC(bit_ceil)BMC") != std::string::npos || low.find(R"BMC(bit_floor)BMC") != std::string::npos || low.find(R"BMC(has_single_bit)BMC") != std::string::npos || low.find(R"BMC(std::popcount)BMC") != std::string::npos)) {
    return R"BMC(C++ bit_ceil unencoded: )BMC" + std::string(engine) + R"BMC( is not a bit_ceil model)BMC";
}
if ((rx_search(R"BMC(\bstd\s*::\s*bit_width\b)BMC", low) || low.find(R"BMC(bit_width unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::bit_width unencoded: )BMC" + std::string(engine) + R"BMC( is not a bit_width model)BMC";
}
if ((rx_search(R"BMC(\bstd\s*::\s*gcd\b)BMC", low) || low.find(R"BMC(gcd unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::gcd unencoded: )BMC" + std::string(engine) + R"BMC( is not a gcd model)BMC";
}
if ((rx_search(R"BMC(\bstd\s*::\s*lcm\b)BMC", low) || low.find(R"BMC(lcm unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::lcm unencoded: )BMC" + std::string(engine) + R"BMC( is not a lcm model)BMC";
}
if ((rx_search(R"BMC(\bstd\s*::\s*clamp\b)BMC", low) || low.find(R"BMC(clamp unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::clamp unencoded: )BMC" + std::string(engine) + R"BMC( is not a clamp model)BMC";
}
if ((rx_search(R"BMC(\bstd\s*::\s*exchange\b)BMC", low) || low.find(R"BMC(exchange unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::exchange unencoded: )BMC" + std::string(engine) + R"BMC( is not an exchange model)BMC";
}
if ((rx_search(R"BMC(\bstd\s*::\s*to_address\b)BMC", low) || low.find(R"BMC(to_address unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::to_address unencoded: )BMC" + std::string(engine) + R"BMC( is not a to_address model)BMC";
}
if ((rx_search(R"BMC(\b(?:construct_at|destroy_at)\b)BMC", low) || low.find(R"BMC(construct_at unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ construct_at unencoded: )BMC" + std::string(engine) + R"BMC( is not a construct_at model)BMC";
}
if ((rx_search(R"BMC(\bdestroy_n\b)BMC", low) || low.find(R"BMC(destroy_n unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ destroy_n unencoded: )BMC" + std::string(engine) + R"BMC( is not a destroy_n model)BMC";
}
if ((rx_search(R"BMC(\b(?:std\s*::\s*)?addressof\b)BMC", low) || low.find(R"BMC(addressof unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::addressof unencoded: )BMC" + std::string(engine) + R"BMC( is not an addressof model)BMC";
}
if ((rx_search(R"BMC(\bas_rvalue(?:_view)?\b)BMC", low) || low.find(R"BMC(as_rvalue unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ as_rvalue unencoded: )BMC" + std::string(engine) + R"BMC( is not an as_rvalue model)BMC";
}
if ((rx_search(R"BMC(\bas_const\b)BMC", low) || low.find(R"BMC(as_const unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ as_const unencoded: )BMC" + std::string(engine) + R"BMC( is not an as_const model)BMC";
}
if ((rx_search(R"BMC(\btransform_(?:inclusive|exclusive)_scan\b)BMC", low) || low.find(R"BMC(transform_inclusive_scan unencoded)BMC") != std::string::npos || low.find(R"BMC(transform_exclusive_scan unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ transform_inclusive_scan unencoded: )BMC" + std::string(engine) + R"BMC( is not a transform_inclusive_scan model)BMC";
}
if ((rx_search(R"BMC(\bexclusive_scan\b)BMC", low) || low.find(R"BMC(exclusive_scan unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ exclusive_scan unencoded: )BMC" + std::string(engine) + R"BMC( is not an exclusive_scan model)BMC";
}
if ((rx_search(R"BMC(\binclusive_scan\b)BMC", low) || low.find(R"BMC(inclusive_scan unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ inclusive_scan unencoded: )BMC" + std::string(engine) + R"BMC( is not an inclusive_scan model)BMC";
}
if ((rx_search(R"BMC(\btransform_reduce\b)BMC", low) || low.find(R"BMC(transform_reduce unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ transform_reduce unencoded: )BMC" + std::string(engine) + R"BMC( is not a transform_reduce model)BMC";
}
if ((rx_search(R"BMC(\bstd\s*::\s*reduce\b)BMC", low) || low.find(R"BMC(std::reduce unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::reduce unencoded: )BMC" + std::string(engine) + R"BMC( is not a reduce model)BMC";
}
if ((rx_search(R"BMC(\buninitialized_(?:fill(?:_n)?|default_construct(?:_n)?)\b)BMC", low) || low.find(R"BMC(uninitialized_fill unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ uninitialized_fill unencoded: )BMC" + std::string(engine) + R"BMC( is not an uninitialized_fill model)BMC";
}
if ((rx_search(R"BMC(\buninitialized_value_construct(?:_n)?\b)BMC", low) || low.find(R"BMC(uninitialized_value_construct unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ uninitialized_value_construct unencoded: )BMC" + std::string(engine) + R"BMC( is not an uninitialized_value_construct model)BMC";
}
if ((rx_search(R"BMC(\buninitialized_(?:copy|move)(?:_n)?\b)BMC", low) || low.find(R"BMC(uninitialized_copy unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ uninitialized_copy unencoded: )BMC" + std::string(engine) + R"BMC( is not an uninitialized_copy model)BMC";
}
if ((rx_search(R"BMC(\b(?:add_sat|sub_sat|mul_sat|div_sat|saturate_cast)\b)BMC", low) || low.find(R"BMC(add_sat unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ add_sat unencoded: )BMC" + std::string(engine) + R"BMC( is not an add_sat model)BMC";
}
if ((rx_search(R"BMC(\bnontype\b)BMC", low) || low.find(R"BMC(nontype unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ nontype unencoded: )BMC" + std::string(engine) + R"BMC( is not a nontype model)BMC";
}
if ((rx_search(R"BMC(\bis_layout_compatible\b)BMC", low) || low.find(R"BMC(is_layout_compatible unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ is_layout_compatible unencoded: )BMC" + std::string(engine) + R"BMC( is not an is_layout_compatible model)BMC";
}
if ((rx_search(R"BMC(\bis_pointer_interconvertible_(?:with_class|base_of)\b)BMC", low) || low.find(R"BMC(is_pointer_interconvertible unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ is_pointer_interconvertible unencoded: )BMC" + std::string(engine) + R"BMC( is not an is_pointer_interconvertible model)BMC";
}
if ((rx_search(R"BMC(\bbasic_const_iterator\b)BMC", low) || low.find(R"BMC(basic_const_iterator unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ basic_const_iterator unencoded: )BMC" + std::string(engine) + R"BMC( is not a basic_const_iterator model)BMC";
}
if ((rx_search(R"BMC(\bis_corresponding_member\b)BMC", low) || low.find(R"BMC(is_corresponding_member unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ is_corresponding_member unencoded: )BMC" + std::string(engine) + R"BMC( is not an is_corresponding_member model)BMC";
}
if ((rx_search(R"BMC(\bforward_like\b)BMC", low) || low.find(R"BMC(forward_like unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ forward_like unencoded: )BMC" + std::string(engine) + R"BMC( is not a forward_like model)BMC";
}
if ((rx_search(R"BMC(\b(?:set|get)_terminate\b)BMC", low) || low.find(R"BMC(set_terminate unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ set_terminate unencoded: )BMC" + std::string(engine) + R"BMC( is not a set_terminate model)BMC";
}
if ((rx_search(R"BMC(\bis_constant_evaluated\b)BMC", low) || low.find(R"BMC(is_constant_evaluated unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ is_constant_evaluated unencoded: )BMC" + std::string(engine) + R"BMC( is not an is_constant_evaluated model)BMC";
}
if ((rx_search(R"BMC(\bstd\s*::\s*lerp\b)BMC", low) || low.find(R"BMC(lerp unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::lerp unencoded: )BMC" + std::string(engine) + R"BMC( is not a lerp model)BMC";
}
if ((rx_search(R"BMC(\bstd\s*::\s*midpoint\b)BMC", low) || low.find(R"BMC(midpoint unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::midpoint unencoded: )BMC" + std::string(engine) + R"BMC( is not a midpoint model)BMC";
}
if ((rx_search(R"BMC(\bstd\s*::\s*(?:cmp_(?:less|greater|less_equal|greater_equal|equal_to|not_equal_to)|in_range)\b)BMC", low) || low.find(R"BMC(cmp_less unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::cmp_less unencoded: )BMC" + std::string(engine) + R"BMC( is not a cmp_less model)BMC";
}
if ((rx_search(R"BMC(\bstd\s*::\s*count[lr]_(?:zero|one)\b)BMC", low) || low.find(R"BMC(countl_zero unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::countl_zero unencoded: )BMC" + std::string(engine) + R"BMC( is not a countl_zero model)BMC";
}
if (low.find(R"BMC(byteswap)BMC") != std::string::npos) {
    return R"BMC(C++ byteswap unencoded: )BMC" + std::string(engine) + R"BMC( is not a byteswap model)BMC";
}
if ((low.find(R"BMC(to_chars)BMC") != std::string::npos && low.find(R"BMC(from_chars)BMC") == std::string::npos)) {
    return R"BMC(C++ to_chars unencoded: )BMC" + std::string(engine) + R"BMC( is not a charconv model)BMC";
}
if (low.find(R"BMC(hazard_pointer)BMC") != std::string::npos) {
    return R"BMC(C++ hazard_pointer unencoded: )BMC" + std::string(engine) + R"BMC( is not a hazard_pointer model)BMC";
}
if (low.find(R"BMC(text_encoding)BMC") != std::string::npos) {
    return R"BMC(C++ text_encoding unencoded: )BMC" + std::string(engine) + R"BMC( is not a text_encoding model)BMC";
}
if (low.find(R"BMC(simd)BMC") != std::string::npos) {
    return R"BMC(C++ simd unencoded: )BMC" + std::string(engine) + R"BMC( is not a simd model)BMC";
}
if (low.find(R"BMC(hive)BMC") != std::string::npos) {
    return R"BMC(C++ hive unencoded: )BMC" + std::string(engine) + R"BMC( is not a hive model)BMC";
}
if (low.find(R"BMC(packaged_task)BMC") != std::string::npos) {
    return R"BMC(C++ packaged_task unencoded: )BMC" + std::string(engine) + R"BMC( is not a packaged_task model)BMC";
}
if (low.find(R"BMC(osyncstream)BMC") != std::string::npos) {
    return R"BMC(C++ osyncstream unencoded: )BMC" + std::string(engine) + R"BMC( is not an osyncstream model)BMC";
}
if (low.find(R"BMC(syncbuf)BMC") != std::string::npos) {
    return R"BMC(C++ syncbuf unencoded: )BMC" + std::string(engine) + R"BMC( is not a syncbuf model)BMC";
}
if ((rx_search(R"BMC(\b(?:views\s*::\s*counted\b|\bcounted_view\b))BMC", low) || (low.find(R"BMC(counted_view unencoded)BMC") != std::string::npos || low.find(R"BMC(views::counted unencoded)BMC") != std::string::npos))) {
    return R"BMC(C++ views::counted unencoded: )BMC" + std::string(engine) + R"BMC( is not a counted_view model)BMC";
}
if (low.find(R"BMC(counted_iterator)BMC") != std::string::npos) {
    return R"BMC(C++ counted_iterator unencoded: )BMC" + std::string(engine) + R"BMC( is not a counted_iterator model)BMC";
}
if (low.find(R"BMC(weak_ptr)BMC") != std::string::npos) {
    return R"BMC(C++ weak_ptr unencoded: )BMC" + std::string(engine) + R"BMC( is not a weak_ptr model)BMC";
}
if (low.find(R"BMC(nested_exception)BMC") != std::string::npos) {
    return R"BMC(C++ nested_exception unencoded: )BMC" + std::string(engine) + R"BMC( is not a nested_exception model)BMC";
}
if ((low.find(R"BMC(throw_with_nested)BMC") != std::string::npos || low.find(R"BMC(rethrow_if_nested)BMC") != std::string::npos)) {
    return R"BMC(throw_with_nested unencoded: unconstrained throw_with_nested is not a proof ()BMC" + std::string(engine) + R"BMC())BMC";
}
if (low.find(R"BMC(uncaught_exceptions)BMC") != std::string::npos) {
    return R"BMC(C++ uncaught_exceptions unencoded: )BMC" + std::string(engine) + R"BMC( is not an uncaught_exceptions model)BMC";
}
if (low.find(R"BMC(current_exception)BMC") != std::string::npos) {
    return R"BMC(C++ current_exception unencoded: )BMC" + std::string(engine) + R"BMC( is not a current_exception model)BMC";
}
if ((rx_search(R"BMC(\bmake_exception_ptr\b)BMC", low) || low.find(R"BMC(make_exception_ptr unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ make_exception_ptr unencoded: )BMC" + std::string(engine) + R"BMC( is not a make_exception_ptr model)BMC";
}
if (low.find(R"BMC(exception_ptr)BMC") != std::string::npos) {
    return R"BMC(C++ exception_ptr unencoded: )BMC" + std::string(engine) + R"BMC( is not an exception_ptr model)BMC";
}
if (low.find(R"BMC(coroutine_handle)BMC") != std::string::npos) {
    return R"BMC(C++ coroutine_handle unencoded: )BMC" + std::string(engine) + R"BMC( is not a coroutine_handle model)BMC";
}
if (low.find(R"BMC(valarray)BMC") != std::string::npos) {
    return R"BMC(C++ valarray unencoded: )BMC" + std::string(engine) + R"BMC( is not a valarray model)BMC";
}
if (low.find(R"BMC(to_underlying)BMC") != std::string::npos) {
    return R"BMC(C++ to_underlying unencoded: )BMC" + std::string(engine) + R"BMC( is not a to_underlying model)BMC";
}
if ((low.find(R"BMC(unexpected<)BMC") != std::string::npos || low.find(R"BMC(unexpected <)BMC") != std::string::npos)) {
    return R"BMC(C++ unexpected unencoded: )BMC" + std::string(engine) + R"BMC( is not an unexpected model)BMC";
}
if ((low.find(R"BMC(std::task)BMC") != std::string::npos || low.find(R"BMC(execution::task)BMC") != std::string::npos || low.find(R"BMC(task unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ std::task unencoded: )BMC" + std::string(engine) + R"BMC( is not a task model)BMC";
}
if (low.find(R"BMC(execution)BMC") != std::string::npos) {
    return R"BMC(C++ execution unencoded: )BMC" + std::string(engine) + R"BMC( is not an execution model)BMC";
}
if ((low.find(R"BMC(indirect)BMC") != std::string::npos || low.find(R"BMC(polymorphic)BMC") != std::string::npos)) {
    return R"BMC(C++ indirect unencoded: )BMC" + std::string(engine) + R"BMC( is not an indirect/polymorphic model)BMC";
}
if (low.find(R"BMC(bitset)BMC") != std::string::npos) {
    return R"BMC(C++ bitset unencoded: )BMC" + std::string(engine) + R"BMC( is not a bitset model)BMC";
}
if (low.find(R"BMC(stringstream)BMC") != std::string::npos) {
    return R"BMC(C++ stringstream unencoded: )BMC" + std::string(engine) + R"BMC( is not a stringstream model)BMC";
}
if (low.find(R"BMC(spanstream)BMC") != std::string::npos) {
    return R"BMC(C++ spanstream unencoded: )BMC" + std::string(engine) + R"BMC( is not a spanstream model)BMC";
}
if ((low.find(R"BMC(out_ptr)BMC") != std::string::npos || low.find(R"BMC(inout_ptr)BMC") != std::string::npos)) {
    return R"BMC(C++ out_ptr unencoded: )BMC" + std::string(engine) + R"BMC( is not an out_ptr model)BMC";
}
if ((low.find(R"BMC(rcu unencoded)BMC") != std::string::npos || low.find(R"BMC(rcu_obj)BMC") != std::string::npos || low.find(R"BMC(rcu_synchronize)BMC") != std::string::npos)) {
    return R"BMC(C++ rcu unencoded: )BMC" + std::string(engine) + R"BMC( is not an rcu model)BMC";
}
if (low.find(R"BMC(linalg)BMC") != std::string::npos) {
    return R"BMC(C++ linalg unencoded: )BMC" + std::string(engine) + R"BMC( is not a linalg model)BMC";
}
if (low.find(R"BMC(sync_wait)BMC") != std::string::npos) {
    return R"BMC(C++ sync_wait unencoded: )BMC" + std::string(engine) + R"BMC( is not an execution model)BMC";
}
if ((low.find(R"BMC(#embed)BMC") != std::string::npos || low.find(R"BMC(embed unencoded)BMC") != std::string::npos)) {
    return R"BMC(C++ #embed unencoded: )BMC" + std::string(engine) + R"BMC( is not an embed model)BMC";
}
if ((low.find(R"BMC(contracts unencoded)BMC") != std::string::npos || low.find(R"BMC(contract_assert)BMC") != std::string::npos)) {
    return R"BMC(C++ contracts unencoded: )BMC" + std::string(engine) + R"BMC( is not a contracts model)BMC";
}
if ((low.find(R"BMC(reflection unencoded)BMC") != std::string::npos || low.find(R"BMC(define_aggregate)BMC") != std::string::npos || low.find(R"BMC(define_class)BMC") != std::string::npos || low.find(R"BMC(std::meta)BMC") != std::string::npos)) {
    return R"BMC(C++ reflection unencoded: )BMC" + std::string(engine) + R"BMC( is not a reflection model)BMC";
}
return std::nullopt;
}

namespace {

#include "bmc_unenc.inc"

// Encoder, parser, k-induction, and run_bmc internals. Concatenated into bmc.cpp.

struct ParseFail : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct ContinueLoop : std::exception {
    const char* what() const noexcept override { return "continue"; }
};

bool is_ident(std::string_view t) {
    if (t.empty()) return false;
    unsigned char c = static_cast<unsigned char>(t[0]);
    if (!(std::isalpha(c) || c == '_')) return false;
    for (size_t i = 1; i < t.size(); ++i) {
        c = static_cast<unsigned char>(t[i]);
        if (!(std::isalnum(c) || c == '_')) return false;
    }
    return true;
}

bool starts_kw(std::string_view text, std::string_view kw) {
    if (!text.starts_with(kw)) return false;
    if (text.size() == kw.size()) return true;
    unsigned char c = static_cast<unsigned char>(text[kw.size()]);
    return !(std::isalnum(c) || c == '_');
}

bool is_computed_goto(std::string_view text) {
    if (!starts_kw(text, "goto")) return false;
    auto rest = text.substr(4);
    size_t i = 0;
    while (i < rest.size() && std::isspace(static_cast<unsigned char>(rest[i]))) ++i;
    return i < rest.size() && rest[i] == '*';
}

const char* NESTED_FN_HEAD =
    "(?:void|int|unsigned(?:\\s+int)?|long(?:\\s+int)?|short|char|"
    "float|double|_Bool|bool)\\s+[A-Za-z_]\\w*\\s*\\([^)]*\\)\\s*\\{";

bool is_nested_function(std::string_view text) {
    auto s = lstrip(std::string(text));
    return rx_match(NESTED_FN_HEAD, s).has_value();
}

const std::vector<std::string> DECL_KWS = {
    "int", "unsigned", "long", "short", "char",
    "uint32_t", "int32_t", "uint64_t", "int64_t", "size_t",
};

const char* DECL_TYPE =
    "(?:unsigned\\s+long\\s+long(?:\\s+int)?|long\\s+long(?:\\s+int)?|"
    "unsigned\\s+long(?:\\s+int)?|uint64_t|int64_t|"
    "unsigned(?:\\s+int)?|int|long|short|char|uint32_t|int32_t|size_t)";

bool looks_like_decl(std::string_view stmt) {
    auto s = lstrip(std::string(stmt));
    for (auto& kw : DECL_KWS)
        if (starts_kw(s, kw)) return true;
    return false;
}

bool type_is_unsigned(std::string_view typ) {
    return rx_search("(?i)\\bunsigned\\b|\\bsize_t\\b|\\buint\\d*_t\\b|\\bu_int\\b|\\bu_long\\b", typ);
}

int type_width(std::string_view typ) {
    auto t = rx_sub("\\s+", " ", to_lower_copy(std::string(typ)));
    if (t.find("long long") != std::string::npos || rx_search("\\b[iu]nt64_t\\b", t))
        return 64;
    return WIDTH;
}

std::map<std::string, int> extract_enums(std::string text) {
    try {
        text = strip_comments_keep_lines(text);
    } catch (...) {
        text = rx_sub("/\\*.*?\\*/", " ", text, false, true);
        text = rx_sub("//.*?$", " ", text, true, false);
    }
    std::map<std::string, int> out;
    static Regex en("\\benum\\b(?:\\s+[A-Za-z_]\\w*)?\\s*\\{([^{}]*)\\}");
    for (auto& m : en.finditer(text)) {
        int nxt = 0;
        auto inner = m.group(1);
        std::string part;
        std::stringstream ss(inner);
        while (std::getline(ss, part, ',')) {
            part = rx_sub("\\s+", " ", strip(part));
            if (part.empty()) continue;
            auto eq = part.find('=');
            if (eq != std::string::npos) {
                auto name = strip(part.substr(0, eq));
                auto val = strip(part.substr(eq + 1));
                while (!val.empty() && (val.back() == 'u' || val.back() == 'U' ||
                                        val.back() == 'l' || val.back() == 'L'))
                    val.pop_back();
                if (!is_ident(name)) continue;
                try {
                    nxt = static_cast<int>(std::stoll(val, nullptr, 0));
                } catch (...) {
                    auto it = out.find(val);
                    if (it == out.end()) continue;
                    nxt = it->second;
                }
                out[name] = nxt;
                nxt += 1;
            } else if (is_ident(part)) {
                out[part] = nxt;
                nxt += 1;
            }
        }
    }
    return out;
}

std::map<std::string, int> enums_from_fn(const FunctionInfo& fn) {
    std::filesystem::path path(fn.file);
    if (!std::filesystem::is_regular_file(path)) return {};
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return extract_enums(ss.str());
    } catch (...) {
        return {};
    }
}

std::vector<std::string> split_semi(std::string_view s) {
    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    for (char ch : s) {
        if (ch == '(') ++depth;
        else if (ch == ')') --depth;
        if (ch == ';' && depth == 0) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    parts.push_back(cur);
    return parts;
}

std::vector<std::string> split_comma(std::string_view s) {
    std::vector<std::string> parts;
    int pdepth = 0, bdepth = 0;
    std::string cur;
    for (char ch : s) {
        if (ch == '(') ++pdepth;
        else if (ch == ')') --pdepth;
        else if (ch == '[') ++bdepth;
        else if (ch == ']') --bdepth;
        if (ch == ',' && pdepth == 0 && bdepth == 0) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    parts.push_back(cur);
    return parts;
}

#ifdef PRISM_HAS_Z3

struct Prop {
    std::string name;
    std::string cls;
    z3::expr cond;
    int loc;
};

struct Arr {
    z3::expr a;
    int n;
};

struct Enc {
    int unwind = 8;
    z3::context ctx;
    z3::solver s;
    std::map<std::string, z3::expr> vars;
    std::map<std::string, Arr> arrays;
    std::map<std::string, z3::expr> uninit;
    std::vector<Prop> props;
    int pc = 0;
    int fresh = 0;
    bool unwind_ok = true;
    z3::expr path_true;
    std::unordered_set<std::string> unsigned_names;
    std::unordered_map<unsigned, bool> utag;
    std::map<std::string, int> bits;
    std::unordered_map<unsigned, int> wtag;

    explicit Enc(int u)
        : unwind(u), s(ctx), path_true(ctx.bool_val(true)) {
        s.set("timeout", 8000u);
    }

    unsigned eid(const z3::expr& v) const { return v.id(); }

    void retag_unsigned() {
        for (auto& n : unsigned_names) {
            auto it = vars.find(n);
            if (it != vars.end()) utag[eid(it->second)] = true;
        }
        for (auto& [n, w] : bits) {
            auto it = vars.find(n);
            if (it != vars.end()) wtag[eid(it->second)] = w;
        }
    }

    z3::expr bv(std::string name = {}, int width = 0) {
        fresh += 1;
        int w = width ? width : WIDTH;
        if (name.empty()) name = "t" + std::to_string(fresh);
        auto v = ctx.bv_const(name.c_str(), static_cast<unsigned>(w));
        wtag[eid(v)] = w;
        return v;
    }

    z3::expr get(const std::string& name) {
        int w = bits.count(name) ? bits[name] : WIDTH;
        if (!vars.count(name)) vars.emplace(name, bv(name, w));
        auto v = vars.at(name);
        wtag[eid(v)] = w;
        if (unsigned_names.count(name)) utag[eid(v)] = true;
        return v;
    }

    z3::expr resize(const z3::expr& v, int src, int dst, bool unsign);
    int width_of(const z3::expr& v) const {
        auto it = wtag.find(eid(v));
        return it == wtag.end() ? WIDTH : it->second;
    }
    bool is_u(const z3::expr& v) const {
        auto it = utag.find(eid(v));
        return it != utag.end() && it->second;
    }
    z3::expr tag(const z3::expr& v, bool unsign, int width = -1) {
        utag[eid(v)] = unsign;
        if (width >= 0) wtag[eid(v)] = width;
        return v;
    }

    void set(const std::string& name, z3::expr val) {
        int w = bits.count(name) ? bits[name] : WIDTH;
        int vw = width_of(val);
        if (vw != w) val = resize(val, vw, w, unsigned_names.count(name) != 0);
        vars.insert_or_assign(name, val);
        wtag[eid(val)] = w;
        if (unsigned_names.count(name)) utag[eid(val)] = true;
    }

    void mark_init(const std::string& name) { uninit.insert_or_assign(name, ctx.bool_val(false)); }
    void mark_uninit(const std::string& name) { uninit.insert_or_assign(name, ctx.bool_val(true)); }

    void add_prop(std::string name, std::string cls, const z3::expr& viol, int loc) {
        props.push_back(Prop{std::move(name), std::move(cls), path_true && viol, loc});
    }

    void check_read(const std::string& name) {
        auto it = uninit.find(name);
        if (it == uninit.end()) return;
        try {
            if (it->second.simplify().is_false()) return;
        } catch (...) {}
        add_prop("uninit", "UNINIT-READ", it->second, pc);
    }

    void assume(const z3::expr& cond) { path_true = path_true && cond; }

    z3::expr bv_min(int w) {
        int64_t v = -(int64_t{1} << (w - 1));
        return ctx.bv_val(v, static_cast<unsigned>(w));
    }
    z3::expr bv_zero(int w) { return ctx.bv_val(0, static_cast<unsigned>(w)); }
    z3::expr bv_val_w(int64_t v, int w = WIDTH) {
        return ctx.bv_val(v, static_cast<unsigned>(w));
    }
};

z3::expr Enc::resize(const z3::expr& v, int src, int dst, bool unsign) {
    if (src == dst) return v;
    if (dst > src)
        return unsign ? z3::zext(v, static_cast<unsigned>(dst - src))
                      : z3::sext(v, static_cast<unsigned>(dst - src));
    return v.extract(static_cast<unsigned>(dst - 1), 0);
}

z3::expr oob(Enc& e, const z3::expr& i, int n) {
    auto bound = e.ctx.bv_val(n, WIDTH);
    if (e.is_u(i)) return z3::uge(i, bound);
    return (i < 0) || (i >= n);
}

z3::expr as_bool(const z3::expr& v) {
    if (v.is_bool()) return v;
    return v != 0;
}

std::map<std::string, z3::expr> merge_uninit(const z3::expr& cond,
    const std::map<std::string, z3::expr>& then_u,
    const std::map<std::string, z3::expr>& else_u,
    const std::map<std::string, z3::expr>& saved_u,
    Enc& e) {
    std::set<std::string> names;
    for (auto& m : {then_u, else_u, saved_u})
        for (auto& [k, _] : m) names.insert(k);
    std::map<std::string, z3::expr> merged;
    auto falseb = e.ctx.bool_val(false);
    auto getu = [&](const std::map<std::string, z3::expr>& m, const std::string& n) -> z3::expr {
        auto it = m.find(n);
        if (it != m.end()) return it->second;
        auto jt = saved_u.find(n);
        if (jt != saved_u.end()) return jt->second;
        return falseb;
    };
    for (auto& n : names) {
        auto a = getu(then_u, n);
        auto b = getu(else_u, n);
        if (z3::eq(a, b)) merged.insert_or_assign(n, a);
        else merged.insert_or_assign(n, z3::ite(cond, a, b));
    }
    return merged;
}

std::pair<std::string, std::string> paren(std::string text);
std::pair<std::string, std::string> brace(std::string text);
std::pair<std::string, std::string> stmt_split(std::string text);
std::pair<std::string, std::string> block_or_stmt(std::string text);
std::pair<std::string, std::string> paren_stmt(std::string text);
std::pair<std::string, std::string> upto_colon(std::string text);
std::pair<std::string, std::string> consume_stmt_src(std::string text);

std::pair<std::string, std::string> paren(std::string text) {
    text = lstrip(text);
    if (!text.starts_with("(")) throw ParseFail("expected (");
    int depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '(') ++depth;
        else if (text[i] == ')') {
            --depth;
            if (depth == 0) return {text.substr(1, i - 1), text.substr(i + 1)};
        }
    }
    throw ParseFail("unbalanced (");
}

std::pair<std::string, std::string> paren_stmt(std::string text) {
    auto [inner, rest] = paren(text);
    rest = lstrip(rest);
    if (rest.starts_with(";")) rest = rest.substr(1);
    return {inner, rest};
}

std::pair<std::string, std::string> brace(std::string text) {
    text = lstrip(text);
    if (!text.starts_with("{")) throw ParseFail("expected {");
    int depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        else if (text[i] == '}') {
            --depth;
            if (depth == 0) return {text.substr(1, i - 1), text.substr(i + 1)};
        }
    }
    throw ParseFail("unbalanced {");
}

std::pair<std::string, std::string> stmt_split(std::string text) {
    int depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '(') ++depth;
        else if (text[i] == ')') --depth;
        else if (text[i] == '{' && depth == 0) break;
        else if (text[i] == ';' && depth == 0)
            return {text.substr(0, i + 1), text.substr(i + 1)};
    }
    throw ParseFail("no semicolon in " + text.substr(0, std::min<size_t>(80, text.size())));
}

std::pair<std::string, std::string> block_or_stmt(std::string text) {
    text = lstrip(text);
    if (text.starts_with("{")) return brace(text);
    return stmt_split(text);
}

std::pair<std::string, std::string> upto_colon(std::string text) {
    int depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '(') ++depth;
        else if (text[i] == ')') --depth;
        else if (text[i] == ':' && depth == 0)
            return {text.substr(0, i), text.substr(i + 1)};
    }
    throw ParseFail("expected :");
}

std::pair<std::string, std::string> consume_stmt_src(std::string text_in) {
    std::string raw = text_in;
    std::string text = lstrip(text_in);
    size_t skip = raw.size() - text.size();
    auto taken = [&](const std::string& rest) -> std::pair<std::string, std::string> {
        size_t idx = rest.empty() && rest != raw ? raw.size() : (raw.size() - rest.size());
        // Python: idx = len(raw) - len(rest)
        idx = raw.size() - rest.size();
        return {raw.substr(skip, idx - skip), rest};
    };
    (void)skip;
    if (text.empty()) return {"", ""};
    if (text.starts_with("{")) {
        auto [_, rest] = brace(text);
        return taken(rest);
    }
    if (starts_kw(text, "do")) {
        auto rest = lstrip(text.substr(2));
        auto [__, r2] = block_or_stmt(rest);
        rest = lstrip(r2);
        if (!starts_kw(rest, "while")) throw ParseFail("do without while");
        rest = lstrip(rest.substr(5));
        auto [___, r3] = paren(rest);
        rest = lstrip(r3);
        if (rest.starts_with(";")) rest = rest.substr(1);
        return taken(rest);
    }
    for (auto kw : {"if", "switch", "while", "for"}) {
        if (starts_kw(text, kw)) {
            auto rest = lstrip(text.substr(std::strlen(kw)));
            if (std::string(kw) == "if" && rest.starts_with("constexpr"))
                throw ParseFail("if constexpr unencoded");
            auto [_, r1] = paren(rest);
            auto [__, r2] = block_or_stmt(r1);
            rest = r2;
            if (std::string(kw) == "if") {
                auto r2s = lstrip(rest);
                if (starts_kw(r2s, "else")) {
                    auto [___, r3] = block_or_stmt(r2s.substr(4));
                    rest = r3;
                }
            }
            return taken(rest);
        }
    }
    return stmt_split(text);
}

struct SwitchArm {
    std::vector<std::optional<z3::expr>> labels;
    std::string code;
    bool stops = false;
};

std::string arm_code(const std::vector<SwitchArm>& arms, size_t i) {
    std::string parts;
    for (size_t j = i; j < arms.size(); ++j) {
        auto t = strip(arms[j].code);
        if (!t.empty()) {
            if (!parts.empty()) parts += "\n";
            parts += arms[j].code;
        }
        if (arms[j].stops) break;
    }
    return parts;
}

z3::expr apply_binop(Enc& e, z3::expr a, const std::string& op, z3::expr b);

struct Parser;

z3::expr parse_expr(Enc& e, std::string src, Parser& parser);

struct Parser {
    std::string body;
    std::vector<std::pair<std::string, std::string>> params;
    int unwind = 8;
    std::map<std::string, int> enums;
    std::string err;
    // AI hook statements (__prism_assume/assert/havoc/step) are only parsed
    // for programs built by prism::ai::check_program, never for user code.
    bool ai_hooks = false;

    Parser(std::string b, std::vector<std::pair<std::string, std::string>> p, int u,
           std::map<std::string, int> en)
        : body(std::move(b)), params(std::move(p)), unwind(u), enums(std::move(en)) {}

    std::unique_ptr<Enc> run() {
        auto e = std::make_unique<Enc>(unwind);
        for (auto& [typ, name] : params) {
            if (name.empty()) continue;
            if (type_is_unsigned(typ)) e->unsigned_names.insert(name);
            e->bits[name] = type_width(typ);
            e->get(name);
        }
        try {
            stmts(*e, prep(body));
        } catch (const ParseFail& ex) {
            err = ex.what();
            return nullptr;
        }
        return e;
    }

    static std::string prep(std::string body) {
        return rx_sub("#.*", " ", body);
    }

    void stmts(Enc& e, std::string text);
    void decl(Enc& e, std::string stmt);
    void assign_or_expr(Enc& e, std::string stmt);
    void astore(Enc& e, const std::string& name, const std::string& idx, const std::string& rhs);
    std::string assert_stmt(Enc& e, std::string text);
    void return_stmt(Enc& e, std::string stmt);
    std::string if_stmt(Enc& e, std::string text);
    std::string while_stmt(Enc& e, std::string text);
    std::string do_stmt(Enc& e, std::string text);
    std::string switch_stmt(Enc& e, std::string text);
    std::vector<SwitchArm> parse_switch_arms(Enc& e, std::string body);
    std::string for_stmt(Enc& e, std::string text);
    std::string ai_hook_stmt(Enc& e, std::string text);
    z3::expr expr(Enc& e, std::string src) { return parse_expr(e, strip(src), *this); }
    z3::expr binop(Enc& e, const z3::expr& a, const std::string& op, const z3::expr& b, const std::string&) {
        return apply_binop(e, a, op, b);
    }
};

void Parser::stmts(Enc& e, std::string text) {
    text = strip(text);
    while (!text.empty()) {
        text = lstrip(text);
        if (text.empty()) break;
        if (text.starts_with("{")) {
            auto [inner, rest] = brace(text);
            stmts(e, inner);
            text = rest;
            continue;
        }
        if (rx_match(R"BMC(if\s+constexpr\b)BMC", text)) {
            throw ParseFail(R"BMC(if constexpr unencoded)BMC");
        }
        if (rx_match(R"BMC(constexpr\b)BMC", text)) {
            throw ParseFail(R"BMC(constexpr unencoded)BMC");
        }
        if (text.find(R"BMC(<=>)BMC") != std::string::npos) {
            throw ParseFail(R"BMC(spaceship unencoded)BMC");
        }
        if (rx_search(R"BMC(__attribute__\s*\(\s*\(\s*cleanup)BMC", text)) {
            throw ParseFail(R"BMC(cleanup unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*expected\b|\bexpected\s*<)BMC", text)) {
            throw ParseFail(R"BMC(expected unencoded)BMC");
        }
        if (rx_search(R"BMC(\bformat_to(?:_n)?\s*\()BMC", text)) {
            throw ParseFail(R"BMC(format_to unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*(?:format|print|println)\b)BMC", text)) {
            throw ParseFail(R"BMC(format unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*jthread\b)BMC", text)) {
            throw ParseFail(R"BMC(jthread unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*(?:async|future|promise)\b|(?:future|promise)\s*<)BMC", text)) {
            throw ParseFail(R"BMC(async unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*function\b|function\s*<)BMC", text)) {
            throw ParseFail(R"BMC(function unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*mdspan\b|mdspan\s*<)BMC", text)) {
            throw ParseFail(R"BMC(mdspan unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*(?:mutex|lock_guard|unique_lock|scoped_lock)\b|(?:lock_guard|unique_lock|scoped_lock)\s*<)BMC", text)) {
            throw ParseFail(R"BMC(std mutex unencoded)BMC");
        }
        if (rx_search(R"BMC(\bcondition_variable_any\b)BMC", text)) {
            throw ParseFail(R"BMC(condition_variable_any unencoded)BMC");
        }
        if (rx_search(R"BMC(\bshared_timed_mutex\b)BMC", text)) {
            throw ParseFail(R"BMC(shared_timed_mutex unencoded)BMC");
        }
        if (rx_search(R"BMC(\brecursive_timed_mutex\b)BMC", text)) {
            throw ParseFail(R"BMC(recursive_timed_mutex unencoded)BMC");
        }
        if (rx_search(R"BMC(\berror_category\b)BMC", text)) {
            throw ParseFail(R"BMC(error_category unencoded)BMC");
        }
        if (rx_search(R"BMC(\bnested_exception\b)BMC", text)) {
            throw ParseFail(R"BMC(nested_exception unencoded)BMC");
        }
        if (rx_search(R"BMC(\bwstring_convert\b)BMC", text)) {
            throw ParseFail(R"BMC(wstring_convert unencoded)BMC");
        }
        if (rx_search(R"BMC(\bsystem_error\b)BMC", text)) {
            throw ParseFail(R"BMC(system_error unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:current_zone|tzdb)\b)BMC", text)) {
            throw ParseFail(R"BMC(tzdb unencoded)BMC");
        }
        if (rx_search(R"BMC(\bis_scoped_enum\b)BMC", text)) {
            throw ParseFail(R"BMC(is_scoped_enum unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:views\s*::\s*)?enumerate(?:_view)?\b)BMC", text)) {
            throw ParseFail(R"BMC(enumerate unencoded)BMC");
        }
        if (rx_search(R"BMC(\bcartesian_product(?:_view)?\b)BMC", text)) {
            throw ParseFail(R"BMC(cartesian_product unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:views\s*::\s*chunk(?:_by)?|chunk(?:_by|_view))\b)BMC", text)) {
            throw ParseFail(R"BMC(chunk unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:views\s*::\s*slide|slide_view)\b)BMC", text)) {
            throw ParseFail(R"BMC(slide unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:views\s*::\s*adjacent(?:_transform)?|adjacent(?:_transform|_view))\b)BMC", text)) {
            throw ParseFail(R"BMC(adjacent unencoded)BMC");
        }
        if (rx_search(R"BMC(\bjoin_with(?:_view)?\b)BMC", text)) {
            throw ParseFail(R"BMC(join_with unencoded)BMC");
        }
        if (rx_search(R"BMC(views\s*::\s*join|\bjoin_view\b)BMC", text)) {
            throw ParseFail(R"BMC(views::join unencoded)BMC");
        }
        if (rx_search(R"BMC(\bzip_transform(?:_view)?\b)BMC", text)) {
            throw ParseFail(R"BMC(zip_transform unencoded)BMC");
        }
        if (rx_search(R"BMC(views\s*::\s*zip|\bzip_view\b)BMC", text)) {
            throw ParseFail(R"BMC(views::zip unencoded)BMC");
        }
        if (rx_search(R"BMC(\bas_rvalue(?:_view)?\b)BMC", text)) {
            throw ParseFail(R"BMC(as_rvalue unencoded)BMC");
        }
        if (rx_search(R"BMC(\bfrom_range\b)BMC", text)) {
            throw ParseFail(R"BMC(from_range unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:views\s*::\s*)?stride(?:_view)?\b)BMC", text)) {
            throw ParseFail(R"BMC(stride unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:views\s*::\s*repeat|repeat_view)\b)BMC", text)) {
            throw ParseFail(R"BMC(repeat unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:views\s*::\s*take\b|\btake_view\b))BMC", text)) {
            throw ParseFail(R"BMC(take unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:views\s*::\s*drop\b|\bdrop_view\b))BMC", text)) {
            throw ParseFail(R"BMC(drop unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:views\s*::\s*filter\b|\bfilter_view\b))BMC", text)) {
            throw ParseFail(R"BMC(filter unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:views\s*::\s*transform\b|\btransform_view\b))BMC", text)) {
            throw ParseFail(R"BMC(transform_view unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:views\s*::\s*elements\b|\belements_view\b))BMC", text)) {
            throw ParseFail(R"BMC(elements unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:views\s*::\s*iota\b|\biota_view\b))BMC", text)) {
            throw ParseFail(R"BMC(iota unencoded)BMC");
        }
        if (rx_search(R"BMC(\breference_wrapper\b)BMC", text)) {
            throw ParseFail(R"BMC(reference_wrapper unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*endian\b)BMC", text)) {
            throw ParseFail(R"BMC(std::endian unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*apply\s*\()BMC", text)) {
            throw ParseFail(R"BMC(std::apply unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:bit_ceil|bit_floor|has_single_bit|std\s*::\s*popcount)\s*\()BMC", text)) {
            throw ParseFail(R"BMC(bit_ceil unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*bit_width\s*\()BMC", text)) {
            throw ParseFail(R"BMC(bit_width unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*gcd\s*\()BMC", text)) {
            throw ParseFail(R"BMC(gcd unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*lcm\s*\()BMC", text)) {
            throw ParseFail(R"BMC(lcm unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*clamp\s*\()BMC", text)) {
            throw ParseFail(R"BMC(clamp unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*exchange\s*\()BMC", text)) {
            throw ParseFail(R"BMC(exchange unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*to_address\s*\()BMC", text)) {
            throw ParseFail(R"BMC(to_address unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*addressof\s*\()BMC", text)) {
            throw ParseFail(R"BMC(addressof unencoded)BMC");
        }
        if (rx_search(R"BMC(\bassume_aligned\s*\()BMC", text)) {
            throw ParseFail(R"BMC(assume_aligned unencoded)BMC");
        }
        if (rx_search(R"BMC(\bas_const\s*\()BMC", text)) {
            throw ParseFail(R"BMC(as_const unencoded)BMC");
        }
        if (rx_search(R"BMC(\btransform_(?:inclusive|exclusive)_scan\s*\()BMC", text)) {
            throw ParseFail(R"BMC(transform_inclusive_scan unencoded)BMC");
        }
        if (rx_search(R"BMC(\bexclusive_scan\s*\()BMC", text)) {
            throw ParseFail(R"BMC(exclusive_scan unencoded)BMC");
        }
        if (rx_search(R"BMC(\binclusive_scan\s*\()BMC", text)) {
            throw ParseFail(R"BMC(inclusive_scan unencoded)BMC");
        }
        if (rx_search(R"BMC(\btransform_reduce\s*\()BMC", text)) {
            throw ParseFail(R"BMC(transform_reduce unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*reduce\s*\()BMC", text)) {
            throw ParseFail(R"BMC(std::reduce unencoded)BMC");
        }
        if (rx_search(R"BMC(\buninitialized_(?:fill(?:_n)?|default_construct(?:_n)?)\s*\()BMC", text)) {
            throw ParseFail(R"BMC(uninitialized_fill unencoded)BMC");
        }
        if (rx_search(R"BMC(\buninitialized_value_construct(?:_n)?\s*\()BMC", text)) {
            throw ParseFail(R"BMC(uninitialized_value_construct unencoded)BMC");
        }
        if (rx_search(R"BMC(\buninitialized_(?:copy|move)(?:_n)?\s*\()BMC", text)) {
            throw ParseFail(R"BMC(uninitialized_copy unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:construct_at|destroy_at)\s*\()BMC", text)) {
            throw ParseFail(R"BMC(construct_at unencoded)BMC");
        }
        if (rx_search(R"BMC(\bdestroy_n\s*\()BMC", text)) {
            throw ParseFail(R"BMC(destroy_n unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:add_sat|sub_sat|mul_sat|div_sat|saturate_cast)\s*\()BMC", text)) {
            throw ParseFail(R"BMC(add_sat unencoded)BMC");
        }
        if (rx_search(R"BMC(\btype_identity\b)BMC", text)) {
            throw ParseFail(R"BMC(type_identity unencoded)BMC");
        }
        if (rx_search(R"BMC(\bnontype\b)BMC", text)) {
            throw ParseFail(R"BMC(nontype unencoded)BMC");
        }
        if (rx_search(R"BMC(\bis_layout_compatible\b)BMC", text)) {
            throw ParseFail(R"BMC(is_layout_compatible unencoded)BMC");
        }
        if (rx_search(R"BMC(\bis_pointer_interconvertible_(?:with_class|base_of)\b)BMC", text)) {
            throw ParseFail(R"BMC(is_pointer_interconvertible unencoded)BMC");
        }
        if (rx_search(R"BMC(\bbasic_const_iterator\b)BMC", text)) {
            throw ParseFail(R"BMC(basic_const_iterator unencoded)BMC");
        }
        if (rx_search(R"BMC(\bis_corresponding_member\b)BMC", text)) {
            throw ParseFail(R"BMC(is_corresponding_member unencoded)BMC");
        }
        if (rx_search(R"BMC(\branges\s*::\s*to\s*[<(])BMC", text)) {
            throw ParseFail(R"BMC(ranges::to unencoded)BMC");
        }
        if (rx_search(R"BMC(\bforward_like\b)BMC", text)) {
            throw ParseFail(R"BMC(forward_like unencoded)BMC");
        }
        if (rx_search(R"BMC(\bmake_exception_ptr\s*\()BMC", text)) {
            throw ParseFail(R"BMC(make_exception_ptr unencoded)BMC");
        }
        if (rx_search(R"BMC(\b(?:set|get)_terminate\s*\()BMC", text)) {
            throw ParseFail(R"BMC(set_terminate unencoded)BMC");
        }
        if (rx_search(R"BMC(\bis_constant_evaluated\s*\()BMC", text)) {
            throw ParseFail(R"BMC(is_constant_evaluated unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*lerp\s*\()BMC", text)) {
            throw ParseFail(R"BMC(lerp unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*midpoint\s*\()BMC", text)) {
            throw ParseFail(R"BMC(midpoint unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*(?:cmp_(?:less|greater|less_equal|greater_equal|equal_to|not_equal_to)|in_range)\s*\()BMC", text)) {
            throw ParseFail(R"BMC(cmp_less unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*count[lr]_(?:zero|one)\s*\()BMC", text)) {
            throw ParseFail(R"BMC(countl_zero unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*unreachable\s*\()BMC", text)) {
            throw ParseFail(R"BMC(std::unreachable unencoded)BMC");
        }
        if (rx_search(R"BMC(\buncaught_exceptions\s*\()BMC", text)) {
            throw ParseFail(R"BMC(uncaught_exceptions unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*(?:condition_variable|shared_mutex)\b|\b(?:condition_variable|shared_mutex)\b)BMC", text)) {
            throw ParseFail(R"BMC(condition_variable unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*atomic_ref\b|atomic_ref\s*<)BMC", text)) {
            throw ParseFail(R"BMC(atomic_ref unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*generator\b|generator\s*<)BMC", text)) {
            throw ParseFail(R"BMC(generator unencoded)BMC");
        }
        if (rx_search(R"BMC(\[\[\s*assume\s*\()BMC", text)) {
            throw ParseFail(R"BMC(assume unencoded)BMC");
        }
        if (rx_search(R"BMC(\bstd\s*::\s*bind\s*\()BMC", text)) {
            throw ParseFail(R"BMC(std bind unencoded)BMC");
        }
        if (rx_search(R"BMC(__attribute__\s*\(\s*\(\s*(?:__)?vector_size|\b__vector_size\b)BMC", text)) {
            throw ParseFail(R"BMC(vector_size unencoded)BMC");
        }
        if ((rx_match(R"BMC(requires\s*\()BMC", text) || rx_match(R"BMC(concept\s+)BMC", text))) {
            throw ParseFail(R"BMC(concepts unencoded)BMC");
        }
        if (rx_search(R"BMC(\(\s*\.\.\.\s*[+\-|&^]|[+\-|&^]\s*\.\.\.\s*\))BMC", text)) {
            throw ParseFail(R"BMC(fold unencoded)BMC");
        }
        if (ai_hooks && text.starts_with("__prism_")) { text = ai_hook_stmt(e, text); continue; }
        if (starts_kw(text, "if")) { text = if_stmt(e, text); continue; }
        if (starts_kw(text, "switch")) { text = switch_stmt(e, text); continue; }
        if (starts_kw(text, "do")) { text = do_stmt(e, text); continue; }
        if (starts_kw(text, "while")) { text = while_stmt(e, text); continue; }
        if (starts_kw(text, "for")) { text = for_stmt(e, text); continue; }
        if (starts_kw(text, "assert")) { text = assert_stmt(e, text); continue; }
        if (starts_kw(text, "static_assert") || starts_kw(text, "_Static_assert")) {
            auto [_, rest] = stmt_split(text);
            text = rest;
            continue;
        }
        if (starts_kw(text, "return")) {
            auto [stmt, rest] = stmt_split(text);
            return_stmt(e, stmt);
            text = rest;
            continue;
        }
        if (starts_kw(text, "break")) {
            auto [_, rest] = stmt_split(text);
            e.path_true = e.ctx.bool_val(false);
            text = rest;
            continue;
        }
        if (starts_kw(text, "continue")) {
            auto [_, rest] = stmt_split(text);
            (void)rest;
            throw ContinueLoop();
        }
        if (is_nested_function(text)) throw ParseFail("nested function unencoded");
        if (starts_kw(text, "goto")) {
            if (is_computed_goto(text)) throw ParseFail("computed goto unencoded");
            throw ParseFail("goto unencoded");
        }
        if (starts_kw(text, "throw")) throw ParseFail("throw unencoded");
        if (starts_kw(text, "asm") || starts_kw(text, "__asm__") || starts_kw(text, "__asm"))
            throw ParseFail("asm unencoded");
        if (starts_kw(text, "try") || starts_kw(text, "catch"))
            throw ParseFail("try unencoded");
        if (starts_kw(text, "case") || starts_kw(text, "default"))
            throw ParseFail("case/default outside switch");
        if (auto miss = unencoded_layout_prefix(text)) throw ParseFail(*miss);
        auto [stmt, rest] = stmt_split(text);
        text = rest;
        if (auto miss = unencoded_layout_stmt(stmt)) throw ParseFail(*miss);
        if (looks_like_decl(stmt)) decl(e, stmt);
        else assign_or_expr(e, stmt);
    }
}

void Parser::decl(Enc& e, std::string stmt) {
    while (!stmt.empty() && stmt.back() == ';') stmt.pop_back();
    stmt = strip(stmt);
    {
        static Regex mptr(std::string(DECL_TYPE) + "\\s*\\*+\\s*([A-Za-z_]\\w*)(?:\\s*=\\s*(.*))?$");
        if (auto m = mptr.search_match(stmt); m && m->spans.size() > 1 && m->spans[0].first == 0 &&
            static_cast<size_t>(m->spans[0].second) == stmt.size()) {
            auto name = m->group(1);
            auto init = m->groups.size() > 2 && m->groups[2] ? *m->groups[2] : std::string{};
            if (!m->groups[2] || !m->groups[2]->size())
                throw ParseFail("uninitialised pointer decl: " + stmt.substr(0, 80));
            auto src = strip(init);
            if (is_ident(src) && e.arrays.count(src)) {
                e.arrays.insert_or_assign(name, e.arrays.at(src));
                return;
            }
            throw ParseFail("pointer decl must alias an array: " + stmt.substr(0, 80));
        }
    }
    {
        static Regex marr(std::string(DECL_TYPE) + "\\s+([A-Za-z_]\\w*)\\s*\\[([^\\]]+)\\](?:\\s*=\\s*(.*))?$");
        if (auto m = marr.search_match(stmt); m && m->spans.size() > 1 && m->spans[0].first == 0 &&
            static_cast<size_t>(m->spans[0].second) == stmt.size()) {
            auto name = m->group(1);
            auto dim = strip(m->group(2));
            auto init = m->groups.size() > 3 && m->groups[3] ? *m->groups[3] : std::string{};
            if (!init.empty() && rx_search("\\{\\s*(?:\\[[^\\]]+\\]|\\.[A-Za-z_]\\w*)\\s*=", init))
                throw ParseFail("designated init unencoded");
            if (!rx_fullmatch("\\d+", dim)) throw ParseFail("VLA unencoded");
            int n = std::stoi(dim);
            auto arr = e.ctx.constant(name.c_str(),
                e.ctx.array_sort(e.ctx.bv_sort(WIDTH), e.ctx.bv_sort(WIDTH)));
            e.arrays.insert_or_assign(name, Arr{arr, n});
            return;
        }
    }
    static Regex md(std::string(DECL_TYPE) + "\\s+([A-Za-z_]\\w*)(?:\\s*=\\s*(.*))?$");
    auto m = md.search_match(stmt);
    if (!(m && m->spans[0].first == 0 && static_cast<size_t>(m->spans[0].second) == stmt.size())) {
        if (rx_search(":\\s*[A-Za-z_]", stmt)) throw ParseFail("range-for unencoded");
        throw ParseFail("unparsed decl: " + stmt.substr(0, 80));
    }
    auto name = m->group(1);
    bool has_init = m->groups.size() > 2 && m->groups[2].has_value();
    auto prefix = stmt.substr(0, static_cast<size_t>(std::max(0, m->spans[1].first)));
    if (type_is_unsigned(prefix)) e.unsigned_names.insert(name);
    e.bits[name] = type_width(prefix);
    if (has_init) {
        e.set(name, expr(e, *m->groups[2]));
        e.mark_init(name);
    } else {
        auto v = e.bv(name + "_uninit", e.bits[name]);
        e.set(name, v);
        e.mark_uninit(name);
    }
}

void Parser::assign_or_expr(Enc& e, std::string stmt) {
    while (!stmt.empty() && stmt.back() == ';') stmt.pop_back();
    stmt = strip(stmt);
    if (stmt.empty()) return;
    auto parts = split_comma(stmt);
    if (parts.size() > 1) {
        for (auto& part : parts) {
            auto piece = strip(part);
            if (!piece.empty()) assign_or_expr(e, piece);
        }
        return;
    }
    static Regex star("\\*\\s*([A-Za-z_]\\w*)\\s*=\\s*(.+)$");
    if (auto m = star.search_match(stmt); m && m->spans[0].first == 0 &&
        static_cast<size_t>(m->spans[0].second) == stmt.size()) {
        astore(e, m->group(1), "0", m->group(2));
        return;
    }
    static Regex aidx("([A-Za-z_]\\w*)\\s*\\[(.+)\\]\\s*=\\s*(.+)$");
    if (auto m = aidx.search_match(stmt); m && m->spans[0].first == 0 &&
        static_cast<size_t>(m->spans[0].second) == stmt.size()) {
        astore(e, m->group(1), m->group(2), m->group(3));
        return;
    }
    static Regex asg("([A-Za-z_]\\w*)\\s*([+\\-*/%|&^]?=)\\s*(.+)$");
    if (auto m = asg.search_match(stmt); m && m->spans[0].first == 0 &&
        static_cast<size_t>(m->spans[0].second) == stmt.size()) {
        auto name = m->group(1);
        auto op = m->group(2);
        auto rhs = m->group(3);
        auto val = expr(e, rhs);
        if (op == "=") e.set(name, val);
        else {
            e.check_read(name);
            auto cur = e.get(name);
            e.set(name, binop(e, cur, op.substr(0, 1), val, stmt));
        }
        e.mark_init(name);
        return;
    }
    expr(e, stmt);
}

void Parser::astore(Enc& e, const std::string& name, const std::string& idx, const std::string& rhs) {
    if (!e.arrays.count(name)) throw ParseFail("unknown array " + name);
    auto [arr, n] = e.arrays.at(name);
    auto i = expr(e, idx);
    auto v = expr(e, rhs);
    e.add_prop("oob-write", "MEM-OOB-WRITE", oob(e, i, n), e.pc);
    e.arrays.insert_or_assign(name, Arr{z3::store(arr, i, v), n});
}

std::string Parser::assert_stmt(Enc& e, std::string text) {
    static Regex as("assert\\s*\\((.*)\\)\\s*;", false, true);
    if (auto m = as.search_match(text); m && m->spans[0].first == 0) {
        auto cond = expr(e, m->group(1));
        e.add_prop("assert", "FUNC-CONTRACT", !as_bool(cond), e.pc);
        return text.substr(static_cast<size_t>(m->spans[0].second));
    }
    auto p = text.find('(');
    if (p == std::string::npos) throw ParseFail("assert");
    auto [inner, rest] = paren_stmt(text.substr(p));
    auto cond = expr(e, inner);
    e.add_prop("assert", "FUNC-CONTRACT", !as_bool(cond), e.pc);
    return rest;
}

// prism::ai loop-cut hooks. Expressions inside assume/assert are predicates
// over the state: UB properties raised while evaluating them are dropped (the
// invariant text is not program code). The loop condition of __prism_step is
// program code, so its properties stay.
std::string Parser::ai_hook_stmt(Enc& e, std::string text) {
    auto drop_props_from = [&](size_t n) {
        e.props.erase(e.props.begin() + static_cast<std::ptrdiff_t>(n), e.props.end());
    };
    if (starts_kw(text, "__prism_assume")) {
        auto [inner, rest] = paren_stmt(text.substr(14));
        size_t n = e.props.size();
        auto c = as_bool(expr(e, inner));
        drop_props_from(n);
        e.assume(c);
        return rest;
    }
    if (starts_kw(text, "__prism_assert")) {
        auto [inner, rest] = paren_stmt(text.substr(14));
        auto comma = inner.find(',');
        if (comma == std::string::npos) throw ParseFail("ai assert without tag");
        auto tag = strip(inner.substr(0, comma));
        size_t n = e.props.size();
        auto c = as_bool(expr(e, inner.substr(comma + 1)));
        drop_props_from(n);
        e.add_prop("ai-inv#" + tag, "AI-INVARIANT", !c, e.pc);
        return rest;
    }
    if (starts_kw(text, "__prism_havoc")) {
        auto [inner, rest] = paren_stmt(text.substr(13));
        auto name = strip(inner);
        if (!is_ident(name)) throw ParseFail("ai havoc of non-identifier");
        e.fresh += 1;
        auto fresh_name = "__hv" + std::to_string(e.fresh) + "_" + name;
        if (e.arrays.count(name)) {
            int n = e.arrays.at(name).n;
            auto arr = e.ctx.constant(fresh_name.c_str(),
                e.ctx.array_sort(e.ctx.bv_sort(WIDTH), e.ctx.bv_sort(WIDTH)));
            e.arrays.insert_or_assign(name, Arr{arr, n});
        } else {
            int w = e.bits.count(name) ? e.bits[name] : WIDTH;
            e.set(name, e.bv(fresh_name, w));
            auto it = e.uninit.find(name);
            bool surely_init = false;
            if (it != e.uninit.end()) {
                try {
                    surely_init = it->second.simplify().is_false();
                } catch (...) {}
                // Havoc keeps "maybe uninitialised" as an unknown flag: the
                // loop may run zero times. Conservative (fails closed).
                if (!surely_init)
                    e.uninit.insert_or_assign(name, e.ctx.bool_const((fresh_name + "_u").c_str()));
            }
        }
        return rest;
    }
    if (starts_kw(text, "__prism_step")) {
        auto rest = lstrip(text.substr(12));
        auto [cond_src, r1] = paren(rest);
        auto [body, r2] = block_or_stmt(r1);
        auto cond = as_bool(expr(e, cond_src));
        auto saved_vars = e.vars;
        auto saved_arr = e.arrays;
        auto saved_uninit = e.uninit;
        auto saved_path = e.path_true;
        e.path_true = saved_path && cond;
        try {
            stmts(e, body);
        } catch (const ContinueLoop&) {
            throw ParseFail("continue in ai loop cut");
        }
        e.vars = saved_vars;
        e.arrays = saved_arr;
        e.uninit = saved_uninit;
        e.path_true = saved_path && !cond;
        e.retag_unsigned();
        return r2;
    }
    throw ParseFail("unknown ai hook");
}

void Parser::return_stmt(Enc& e, std::string stmt) {
    auto rest = strip(stmt);
    if (rest.starts_with("return")) {
        auto exprs = strip(rest.substr(6));
        while (!exprs.empty() && exprs.back() == ';') exprs.pop_back();
        exprs = strip(exprs);
        if (!exprs.empty()) {
            auto val = expr(e, exprs);
            e.set("__ret", val);
        }
    }
    e.path_true = e.ctx.bool_val(false);
}

z3::check_result check_sat(Enc& e, const z3::expr& cond) {
    z3::solver s(e.ctx);
    s.set("timeout", 2000u);
    s.add(e.path_true);
    s.add(cond);
    return s.check();
}

std::string Parser::if_stmt(Enc& e, std::string text) {
    auto rest = lstrip(text.substr(2));
    if (rest.starts_with("constexpr")) throw ParseFail("if constexpr unencoded");
    auto [cond_src, r1] = paren(rest);
    auto [then_src, r2] = block_or_stmt(r1);
    rest = r2;
    std::optional<std::string> else_src;
    auto rest2 = lstrip(rest);
    if (rest2.starts_with("else")) {
        auto [es, r3] = block_or_stmt(rest2.substr(4));
        else_src = es;
        rest = r3;
    }
    auto cond = as_bool(expr(e, cond_src));
    auto saved_vars = e.vars;
    auto saved_arr = e.arrays;
    auto saved_uninit = e.uninit;
    auto saved_path = e.path_true;
    e.path_true = saved_path && cond;
    stmts(e, then_src);
    auto then_vars = e.vars;
    auto then_arr = e.arrays;
    auto then_uninit = e.uninit;
    auto then_path = e.path_true;
    e.vars = saved_vars;
    e.arrays = saved_arr;
    e.uninit = saved_uninit;
    e.path_true = saved_path && !cond;
    if (else_src) stmts(e, *else_src);
    auto else_vars = e.vars;
    auto else_arr = e.arrays;
    auto else_uninit = e.uninit;
    auto else_path = e.path_true;
    std::set<std::string> names;
    for (auto& m : {then_vars, else_vars})
        for (auto& [k, _] : m) names.insert(k);
    std::map<std::string, z3::expr> merged;
    for (auto& n : names) {
        std::optional<z3::expr> a, b;
        if (then_vars.count(n)) a = then_vars.at(n);
        else if (saved_vars.count(n)) a = saved_vars.at(n);
        if (else_vars.count(n)) b = else_vars.at(n);
        else if (saved_vars.count(n)) b = saved_vars.at(n);
        if (!a || !b) merged.insert_or_assign(n, a ? *a : *b);
        else if (z3::eq(*a, *b)) merged.insert_or_assign(n, *a);
        else merged.insert_or_assign(n, z3::ite(cond, *a, *b));
    }
    e.vars = merged;
    e.retag_unsigned();
    e.arrays = !then_arr.empty() ? then_arr : else_arr;
    e.uninit = merge_uninit(cond, then_uninit, else_uninit, saved_uninit, e);
    e.path_true = (then_path || else_path).simplify();
    return rest;
}

std::string Parser::while_stmt(Enc& e, std::string text) {
    auto rest = lstrip(text.substr(5));
    auto [cond_src, r1] = paren(rest);
    auto [body, r2] = block_or_stmt(r1);
    rest = r2;
    bool closed = false;
    bool broke = false;
    for (int i = 0; i < e.unwind; ++i) {
        auto cond = as_bool(expr(e, cond_src));
        if (check_sat(e, cond) == z3::unsat) {
            closed = true;
            broke = true;
            break;
        }
        e.assume(cond);
        try { stmts(e, body); } catch (const ContinueLoop&) {}
    }
    if (!broke) {
        auto cond = as_bool(expr(e, cond_src));
        if (check_sat(e, cond) == z3::sat) {
            e.unwind_ok = false;
            e.assume(!cond);
        } else {
            closed = true;
        }
    }
    (void)closed;
    return rest;
}

std::string Parser::do_stmt(Enc& e, std::string text) {
    auto rest = lstrip(text.substr(2));
    auto [body, r1] = block_or_stmt(rest);
    rest = lstrip(r1);
    if (!starts_kw(rest, "while")) throw ParseFail("do without while");
    rest = lstrip(rest.substr(5));
    auto [cond_src, r2] = paren(rest);
    rest = lstrip(r2);
    if (rest.starts_with(";")) rest = rest.substr(1);
    try { stmts(e, body); } catch (const ContinueLoop&) {}
    int remaining = std::max(e.unwind - 1, 0);
    bool closed = false;
    bool broke = false;
    for (int i = 0; i < remaining; ++i) {
        auto cond = as_bool(expr(e, cond_src));
        if (check_sat(e, cond) == z3::unsat) {
            closed = true;
            broke = true;
            break;
        }
        e.assume(cond);
        try { stmts(e, body); } catch (const ContinueLoop&) {}
    }
    if (!broke) {
        auto cond = as_bool(expr(e, cond_src));
        if (check_sat(e, cond) == z3::sat) {
            e.unwind_ok = false;
            e.assume(!cond);
        } else {
            closed = true;
        }
    }
    (void)closed;
    return rest;
}

std::string Parser::switch_stmt(Enc& e, std::string text) {
    auto rest = lstrip(text.substr(6));
    auto [cond_src, r1] = paren(rest);
    auto [body, r2] = block_or_stmt(r1);
    rest = r2;
    auto scrut = expr(e, cond_src);
    auto arms = parse_switch_arms(e, body);
    if (arms.empty()) return rest;

    z3::expr_vector case_eqs(e.ctx);
    bool has_default = false;
    for (auto& arm : arms) {
        for (auto& lab : arm.labels) {
            if (!lab) has_default = true;
            else case_eqs.push_back(scrut == *lab);
        }
    }
    z3::expr any_case = case_eqs.empty() ? e.ctx.bool_val(false) : z3::mk_or(case_eqs);

    auto saved_vars = e.vars;
    auto saved_arr = e.arrays;
    auto saved_uninit = e.uninit;
    auto saved_path = e.path_true;
    struct Taken {
        z3::expr cond;
        std::map<std::string, z3::expr> vars;
        std::map<std::string, Arr> arrs;
        std::map<std::string, z3::expr> uninit;
        z3::expr path;
    };
    std::vector<Taken> taken;

    for (size_t i = 0; i < arms.size(); ++i) {
        auto& arm = arms[i];
        if (arm.labels.empty()) continue;
        z3::expr_vector parts(e.ctx);
        for (auto& lab : arm.labels) {
            if (!lab) parts.push_back(!any_case);
            else parts.push_back(scrut == *lab);
        }
        z3::expr cond = parts.size() == 1 ? parts[0] : z3::mk_or(parts);
        e.vars = saved_vars;
        e.arrays = saved_arr;
        e.uninit = saved_uninit;
        e.path_true = saved_path && cond;
        stmts(e, arm_code(arms, i));
        taken.push_back(Taken{cond, e.vars, e.arrays, e.uninit, e.path_true});
    }
    if (!has_default) {
        auto skip = !any_case;
        taken.push_back(Taken{skip, saved_vars, saved_arr, saved_uninit, saved_path && skip});
    }
    if (taken.empty()) {
        e.vars = saved_vars;
        e.arrays = saved_arr;
        e.uninit = saved_uninit;
        e.path_true = saved_path;
        return rest;
    }
    std::set<std::string> names;
    for (auto& t : taken)
        for (auto& [k, _] : t.vars) names.insert(k);
    for (auto& [k, _] : saved_vars) names.insert(k);
    std::map<std::string, z3::expr> merged;
    for (auto& n : names) {
        std::optional<z3::expr> acc;
        if (saved_vars.count(n)) acc = saved_vars.at(n);
        for (auto it = taken.rbegin(); it != taken.rend(); ++it) {
            std::optional<z3::expr> v;
            if (it->vars.count(n)) v = it->vars.at(n);
            else if (saved_vars.count(n)) v = saved_vars.at(n);
            if (!acc) acc = v;
            else if (!v) {}
            else if (z3::eq(*acc, *v)) {}
            else acc = z3::ite(it->cond, *v, *acc);
        }
        if (acc) merged.insert_or_assign(n, *acc);
    }
    e.vars = merged;
    e.retag_unsigned();
    e.arrays = saved_arr;
    for (auto& t : taken)
        for (auto& [k, av] : t.arrs) e.arrays.insert_or_assign(k, av);
    std::set<std::string> unames;
    for (auto& [k, _] : saved_uninit) unames.insert(k);
    for (auto& t : taken)
        for (auto& [k, _] : t.uninit) unames.insert(k);
    std::map<std::string, z3::expr> u_acc = saved_uninit;
    auto falseb = e.ctx.bool_val(false);
    for (auto& n : unames) {
        z3::expr accu = saved_uninit.count(n) ? saved_uninit.at(n) : falseb;
        for (auto it = taken.rbegin(); it != taken.rend(); ++it) {
            z3::expr v = it->uninit.count(n) ? it->uninit.at(n)
                : (saved_uninit.count(n) ? saved_uninit.at(n) : falseb);
            if (z3::eq(accu, v)) {}
            else accu = z3::ite(it->cond, v, accu);
        }
        u_acc.insert_or_assign(n, accu);
    }
    e.uninit = u_acc;
    z3::expr_vector paths(e.ctx);
    for (auto& t : taken) paths.push_back(t.path);
    e.path_true = z3::mk_or(paths).simplify();
    return rest;
}

std::vector<SwitchArm> Parser::parse_switch_arms(Enc& e, std::string body) {
    std::vector<SwitchArm> arms;
    std::vector<std::optional<z3::expr>> labels;
    std::vector<std::string> chunks;
    bool stops = false;
    std::string text = body;
    auto flush = [&] {
        if (!labels.empty() || !chunks.empty()) {
            std::string code;
            for (size_t i = 0; i < chunks.size(); ++i) {
                if (i) code += "\n";
                code += chunks[i];
            }
            arms.push_back(SwitchArm{labels, code, stops});
        }
        labels.clear();
        chunks.clear();
        stops = false;
    };
    while (!text.empty()) {
        text = lstrip(text);
        if (text.empty()) break;
        if (starts_kw(text, "case")) {
            if (!chunks.empty() || stops) flush();
            auto rest = lstrip(text.substr(4));
            auto [src, t2] = upto_colon(rest);
            text = t2;
            if (rx_search("\\.\\.\\.", src)) throw ParseFail("case-range unencoded");
            labels.push_back(expr(e, src));
            continue;
        }
        if (starts_kw(text, "default")) {
            if (!chunks.empty() || stops) flush();
            auto rest = lstrip(text.substr(7));
            if (!rest.starts_with(":")) throw ParseFail("expected : after default");
            labels.push_back(std::nullopt);
            text = rest.substr(1);
            continue;
        }
        if (starts_kw(text, "break")) {
            auto [_, t2] = stmt_split(text);
            text = t2;
            stops = true;
            continue;
        }
        auto [src, t2] = consume_stmt_src(text);
        text = t2;
        if (stops) continue;
        if (!strip(src).empty()) chunks.push_back(strip(src));
    }
    flush();
    return arms;
}

std::string Parser::for_stmt(Enc& e, std::string text) {
    auto rest = lstrip(text.substr(3));
    auto [head, r1] = paren(rest);
    auto parts = split_semi(head);
    while (parts.size() < 3) parts.push_back("");
    auto init = strip(parts[0]);
    auto cond_src = strip(parts[1]);
    auto incr = strip(parts[2]);
    if (!init.empty()) {
        for (auto& piece0 : split_comma(init)) {
            auto piece = strip(piece0);
            if (piece.empty()) continue;
            auto init_stmt = piece.ends_with(";") ? piece : piece + ";";
            if (auto miss = unencoded_layout_stmt(init_stmt)) throw ParseFail(*miss);
            if (looks_like_decl(init_stmt)) decl(e, init_stmt);
            else assign_or_expr(e, init_stmt);
        }
    }
    auto [body, r2] = block_or_stmt(r1);
    rest = r2;
    bool closed = false;
    bool broke = false;
    for (int i = 0; i < e.unwind; ++i) {
        auto cond = as_bool(expr(e, cond_src.empty() ? "1" : cond_src));
        if (check_sat(e, cond) == z3::unsat) {
            closed = true;
            broke = true;
            break;
        }
        e.assume(cond);
        try { stmts(e, body); } catch (const ContinueLoop&) {}
        if (!incr.empty()) {
            auto incr_stmt = incr.ends_with(";") ? incr : incr + ";";
            assign_or_expr(e, incr_stmt);
        }
    }
    if (!broke) {
        auto cond = as_bool(expr(e, cond_src.empty() ? "1" : cond_src));
        if (check_sat(e, cond) == z3::sat) {
            e.unwind_ok = false;
            e.assume(!cond);
        } else {
            closed = true;
        }
    }
    (void)closed;
    return rest;
}

z3::expr apply_binop(Enc& e, z3::expr a, const std::string& op, z3::expr b) {
    int wa = e.width_of(a), wb = e.width_of(b);
    int w = std::max(wa, wb);
    bool ua = e.is_u(a), ub = e.is_u(b);
    if (wa < w) {
        a = e.resize(a, wa, w, ua);
        e.wtag[e.eid(a)] = w;
        if (ua) e.utag[e.eid(a)] = true;
    }
    if (wb < w) {
        b = e.resize(b, wb, w, ub);
        e.wtag[e.eid(b)] = w;
        if (ub) e.utag[e.eid(b)] = true;
    }
    bool u = ua || ub;
    if (op == "+" || op == "-" || op == "*" || op == "/") {
        if (op == "+") {
            auto r = a + b;
            if (!u) {
                auto ov = !z3::bvadd_no_overflow(a, b, true);
                e.add_prop("ovf+", "INT-SIGNED-OVF", ov, e.pc);
            }
            return e.tag(r, u, w);
        }
        if (op == "-") {
            auto r = a - b;
            if (!u) {
                z3::expr ov = e.ctx.bool_val(false);
                try { ov = !z3::bvsub_no_underflow(a, b, true); } catch (...) {}
                e.add_prop("ovf-", "INT-SIGNED-OVF", ov, e.pc);
            }
            return e.tag(r, u, w);
        }
        if (op == "*") {
            auto r = a * b;
            if (!u) {
                auto ov = !z3::bvmul_no_overflow(a, b, true);
                e.add_prop("ovf*", "INT-SIGNED-OVF", ov, e.pc);
            }
            return e.tag(r, u, w);
        }
        if (op == "/") {
            e.add_prop("div0", "INT-DIV-ZERO", b == 0, e.pc);
            auto z = e.bv_zero(w);
            if (u) {
                auto r = z3::ite(b == 0, z, z3::udiv(a, b));
                return e.tag(r, true, w);
            }
            e.add_prop("divovf", "INT-SIGNED-OVF", (a == e.bv_min(w)) && (b == -1), e.pc);
            return e.tag(z3::ite(b == 0, z, a / b), false, w);
        }
    }
    if (op == "%") {
        e.add_prop("mod0", "INT-DIV-ZERO", b == 0, e.pc);
        auto rem = u ? z3::urem(a, b) : z3::srem(a, b);
        auto r = z3::ite(b == 0, e.bv_zero(w), rem);
        return e.tag(r, u, w);
    }
    if (op == "<<") {
        if (u) e.add_prop("shift", "INT-SHIFT-UB", z3::uge(b, w), e.pc);
        else {
            e.add_prop("shift", "INT-SHIFT-UB", (b < 0) || z3::uge(b, w), e.pc);
            e.add_prop("shift31", "INT-SHIFT-UB", (a == 1) && z3::uge(b, w - 1), e.pc);
        }
        return e.tag(z3::shl(a, b), u, w);
    }
    if (op == ">>") {
        if (u) {
            e.add_prop("shift", "INT-SHIFT-UB", z3::uge(b, w), e.pc);
            return e.tag(z3::lshr(a, b), true, w);
        }
        e.add_prop("shift", "INT-SHIFT-UB", (b < 0) || z3::uge(b, w), e.pc);
        return e.tag(z3::ashr(a, b), false, w);
    }
    if (op == "&") return e.tag(a & b, u, w);
    if (op == "|") return e.tag(a | b, u, w);
    if (op == "^") return e.tag(a ^ b, u, w);
    auto one = e.ctx.bv_val(1, WIDTH);
    auto zero = e.ctx.bv_val(0, WIDTH);
    if (op == "==") return e.tag(z3::ite(a == b, one, zero), false, WIDTH);
    if (op == "!=") return e.tag(z3::ite(a != b, one, zero), false, WIDTH);
    if (op == "<") {
        auto pred = u ? z3::ult(a, b) : (a < b);
        return e.tag(z3::ite(pred, one, zero), false, WIDTH);
    }
    if (op == ">") {
        auto pred = u ? z3::ugt(a, b) : (a > b);
        return e.tag(z3::ite(pred, one, zero), false, WIDTH);
    }
    if (op == "<=") {
        auto pred = u ? z3::ule(a, b) : (a <= b);
        return e.tag(z3::ite(pred, one, zero), false, WIDTH);
    }
    if (op == ">=") {
        auto pred = u ? z3::uge(a, b) : (a >= b);
        return e.tag(z3::ite(pred, one, zero), false, WIDTH);
    }
    throw ParseFail("op " + op);
}

int char_lit_value(const std::string& tok) {
    auto inner = tok.substr(1, tok.size() - 2);
    if (inner.empty()) throw ParseFail("empty character literal");
    if (inner[0] == '\\' && inner.size() >= 2) {
        char esc = inner[1];
        switch (esc) {
        case 'n': return 10;
        case 't': return 9;
        case 'r': return 13;
        case '0': return 0;
        case '\\': return 92;
        case '\'': return 39;
        case '"': return 34;
        default: return static_cast<unsigned char>(esc);
        }
    }
    return static_cast<unsigned char>(inner[0]);
}

const std::unordered_set<std::string> CAST_WORDS = {
    "char", "short", "int", "long", "unsigned", "signed",
    "const", "volatile", "void", "_Bool", "bool",
    "uint32_t", "int32_t", "uint64_t", "int64_t", "size_t",
};

const std::map<std::string, int> TYPE_SIZE = {
    {"char", 1}, {"signed char", 1}, {"unsigned char", 1},
    {"short", 2}, {"short int", 2}, {"signed short", 2}, {"unsigned short", 2},
    {"int", 4}, {"signed", 4}, {"signed int", 4}, {"unsigned", 4}, {"unsigned int", 4},
    {"long", 4}, {"long int", 4}, {"unsigned long", 4},
    {"long long", 8}, {"long long int", 8}, {"unsigned long long", 8},
    {"uint32_t", 4}, {"int32_t", 4}, {"size_t", 4},
    {"_Bool", 1}, {"bool", 1},
};

int sizeof_tokens(const std::vector<std::string>& inner, Enc& e) {
    if (inner.empty()) return WIDTH / 8;
    for (auto& t : inner)
        if (t == "*") return WIDTH / 8;
    std::string joined;
    for (size_t i = 0; i < inner.size(); ++i) {
        if (i) joined += " ";
        joined += inner[i];
    }
    auto it = TYPE_SIZE.find(joined);
    if (it != TYPE_SIZE.end()) return it->second;
    if (inner.size() == 1 && is_ident(inner[0])) {
        auto name = inner[0];
        if (e.arrays.count(name)) return e.arrays.at(name).n * (WIDTH / 8);
        return WIDTH / 8;
    }
    for (auto& t : inner)
        if (t == "[") return WIDTH / 8;
    return WIDTH / 8;
}

std::vector<std::string> tok(const std::string& src) {
    static Regex rx(
        "(0x[0-9a-fA-F]+)|(\\d+)|('(?:\\\\.|[^\\\\'])')|(\"(?:\\\\.|[^\\\\\"])*\")|"
        "([A-Za-z_]\\w*)|(&&|\\|\\||==|!=|<=|>=|<<|>>|\\+\\+|--)|"
        "([+\\-*/%<>=!&|^~()[\\],?:])");
    std::vector<std::string> out;
    for (auto& m : rx.finditer(src)) out.push_back(m.text);
    return out;
}

z3::expr parse_expr(Enc& e, std::string src, Parser& parser) {
    src = strip(src);
    auto tokens = tok(src);
    size_t pos = 0;
    auto peek = [&]() -> std::string {
        return pos < tokens.size() ? tokens[pos] : std::string{};
    };
    auto eat = [&](std::string t = {}) -> std::string {
        if (pos >= tokens.size()) throw ParseFail("unexpected end of expression");
        auto got = tokens[pos];
        if (!t.empty() && got != t) throw ParseFail("expected " + t + " got " + got);
        ++pos;
        return got;
    };
    std::function<z3::expr(int)> parse;
    std::function<z3::expr()> nud;
    static const std::map<std::string, int> PREC = {
        {"||", 10}, {"&&", 20},
        {"|", 30}, {"^", 40}, {"&", 50},
        {"==", 60}, {"!=", 60},
        {"<", 70}, {">", 70}, {"<=", 70}, {">=", 70},
        {"<<", 80}, {">>", 80},
        {"+", 90}, {"-", 90},
        {"*", 100}, {"/", 100}, {"%", 100},
    };
    nud = [&]() -> z3::expr {
        auto t = eat();
        if (t.size() >= 3 && t.front() == '\'' && t.back() == '\'')
            return e.ctx.bv_val(char_lit_value(t), WIDTH);
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
            return e.ctx.bv_val(1, WIDTH);
        if (t == "alignof" || t == "_Alignof") throw ParseFail("alignof unencoded");
        if (t == "sizeof") {
            if (peek() == "(") {
                eat("(");
                std::vector<std::string> inner;
                int depth = 1;
                while (depth) {
                    auto ntok = eat();
                    if (ntok == "(") { ++depth; inner.push_back(ntok); }
                    else if (ntok == ")") {
                        --depth;
                        if (depth) inner.push_back(ntok);
                    } else inner.push_back(ntok);
                }
                return e.ctx.bv_val(sizeof_tokens(inner, e), WIDTH);
            }
            auto name = eat();
            return e.ctx.bv_val(sizeof_tokens({name}, e), WIDTH);
        }
        if (t == "(") {
            if (peek() == "{") throw ParseFail("statement-expr unencoded");
            if (CAST_WORDS.count(peek())) {
                std::vector<std::string> words;
                while (!peek().empty() && peek() != ")") {
                    if (!CAST_WORDS.count(peek()) && peek() != "*") break;
                    words.push_back(eat());
                }
                eat(")");
                auto v = parse(110);
                std::string joined;
                for (size_t i = 0; i < words.size(); ++i) {
                    if (i) joined += " ";
                    joined += words[i];
                }
                int dst = type_width(joined);
                int srcw = e.width_of(v);
                bool u = type_is_unsigned(joined);
                v = e.resize(v, srcw, dst, u);
                return e.tag(v, u, dst);
            }
            auto v = parse(0);
            eat(")");
            return v;
        }
        if (t == "++" || t == "--") {
            auto name = eat();
            if (!is_ident(name)) throw ParseFail("prefix " + t + " needs an identifier");
            e.check_read(name);
            auto cur = e.get(name);
            auto one = e.ctx.bv_val(1, WIDTH);
            auto nw = apply_binop(e, cur, t == "++" ? "+" : "-", one);
            e.set(name, nw);
            e.mark_init(name);
            return nw;
        }
        if (t == "*") {
            auto name = peek();
            if (!is_ident(name) || !e.arrays.count(name))
                throw ParseFail("deref of '" + name + "'");
            eat();
            auto [arr, n] = e.arrays.at(name);
            auto idx = e.ctx.bv_val(0, WIDTH);
            e.add_prop("oob-read", "MEM-OOB-READ", oob(e, idx, n), e.pc);
            return z3::select(arr, idx);
        }
        if (t == "-") {
            auto v = parse(110);
            int w = e.width_of(v);
            auto z = e.bv_zero(w);
            auto r = z - v;
            if (!e.is_u(v)) e.add_prop("neg", "INT-SIGNED-OVF", v == e.bv_min(w), e.pc);
            return e.tag(r, e.is_u(v), w);
        }
        if (t == "!") {
            auto v = parse(110);
            return z3::ite(as_bool(v), e.ctx.bv_val(0, WIDTH), e.ctx.bv_val(1, WIDTH));
        }
        if (t == "~") return ~parse(110);
        if (std::isdigit(static_cast<unsigned char>(t[0])) || t.starts_with("0x")) {
            uint64_t val = 0;
            try { val = std::stoull(t, nullptr, 0); } catch (...) { val = 0; }
            return e.tag(e.ctx.bv_val(static_cast<uint64_t>(val), WIDTH), false, WIDTH);
        }
        if (is_ident(t)) {
            if (t == "_Generic" || t == "offsetof") throw ParseFail(t + " unencoded");
            if (t == "__int128" || t == "__int128_t" || t == "_BitInt")
                throw ParseFail("128-bit unencoded");
            if (t == "_Decimal32" || t == "_Decimal64" || t == "_Decimal128")
                throw ParseFail("decimal-float unencoded");
            if (t == "_Float16" || t == "_Float32" || t == "_Float64" || t == "__fp16")
                throw ParseFail("extra-IEEE unencoded");
            if (t == "typeof_unqual" || t == "__typeof_unqual__")
                throw ParseFail("typeof_unqual unencoded");
            if (t == "start_lifetime_as" || t == "start_lifetime_as_array")
                throw ParseFail("start_lifetime_as unencoded");
            if (t == "nullptr") throw ParseFail("nullptr unencoded");
            if (t == "restrict") throw ParseFail("restrict unencoded");
            if (t == "launder" || (t == "std" && peek() == "launder"))
                throw ParseFail("launder unencoded");
            if (t == "requires" && peek() == "(") throw ParseFail("concepts unencoded");
            if (t == "__builtin_choose_expr") throw ParseFail("choose_expr unencoded");
            if (t == "dlopen" || t == "dlsym" || t == "dlclose")
                throw ParseFail("dlopen unencoded");
            if (t == "__builtin_clz" || t == "__builtin_ctz" || t == "__builtin_clzll" || t == "__builtin_ctzll")
                throw ParseFail("clz unencoded");
            if (t == "bit_cast") throw ParseFail("bit_cast unencoded");
            if (t == "shared_from_this" || t == "enable_shared_from_this"
                || (t == "std" && (peek() == "shared_from_this" || peek() == "enable_shared_from_this")))
                throw ParseFail("shared_from_this unencoded");
            if (t == "__atomic_load" || t == "__atomic_store"
                || t == "__sync_fetch_and_add" || t == "__sync_bool_compare_and_swap")
                throw ParseFail("atomic builtin unencoded");
            if ((t == "optional" || t == "variant" || t == "span" || t == "expected"
                 || t == "function" || t == "mdspan" || t == "future" || t == "promise"
                 || t == "atomic_ref" || t == "generator")
                && peek() == "<")
                throw ParseFail("std::" + t + " unencoded");
            if (t == "condition_variable_any") throw ParseFail("condition_variable_any unencoded");
            if (t == "shared_timed_mutex") throw ParseFail("shared_timed_mutex unencoded");
            if (t == "recursive_timed_mutex") throw ParseFail("recursive_timed_mutex unencoded");
            if (t == "error_category") throw ParseFail("error_category unencoded");
            if (t == "nested_exception") throw ParseFail("nested_exception unencoded");
            if (t == "wstring_convert") throw ParseFail("wstring_convert unencoded");
            if (t == "system_error") throw ParseFail("system_error unencoded");
            if (t == "current_zone" || t == "tzdb") throw ParseFail("tzdb unencoded");
            if (t == "is_scoped_enum") throw ParseFail("is_scoped_enum unencoded");
            if (t == "enumerate" || t == "enumerate_view") throw ParseFail("enumerate unencoded");
            if (t == "cartesian_product" || t == "cartesian_product_view")
                throw ParseFail("cartesian_product unencoded");
            if (t == "chunk_by" || t == "chunk_view") throw ParseFail("chunk unencoded");
            if (t == "slide_view") throw ParseFail("slide unencoded");
            if (t == "adjacent_transform" || t == "adjacent_view")
                throw ParseFail("adjacent unencoded");
            if (t == "join_with" || t == "join_with_view") throw ParseFail("join_with unencoded");
            if (t == "join_view") throw ParseFail("views::join unencoded");
            if (t == "zip_transform" || t == "zip_transform_view")
                throw ParseFail("zip_transform unencoded");
            if (t == "zip_view") throw ParseFail("views::zip unencoded");
            if (t == "as_rvalue" || t == "as_rvalue_view") throw ParseFail("as_rvalue unencoded");
            if (t == "from_range" || t == "from_range_t") throw ParseFail("from_range unencoded");
            if (t == "stride_view") throw ParseFail("stride unencoded");
            if (t == "repeat_view") throw ParseFail("repeat unencoded");
            if (t == "take_view") throw ParseFail("take unencoded");
            if (t == "drop_view") throw ParseFail("drop unencoded");
            if (t == "filter_view") throw ParseFail("filter unencoded");
            if (t == "transform_view") throw ParseFail("transform_view unencoded");
            if (t == "elements_view") throw ParseFail("elements unencoded");
            if (t == "iota_view") throw ParseFail("iota unencoded");
            if (t == "reference_wrapper") throw ParseFail("reference_wrapper unencoded");
            if (t == "uncaught_exceptions" && peek() == "(")
                throw ParseFail("uncaught_exceptions unencoded");
            if ((t == "bit_ceil" || t == "bit_floor" || t == "has_single_bit") && peek() == "(")
                throw ParseFail("bit_ceil unencoded");
            if (t == "bit_width" && peek() == "(") throw ParseFail("bit_width unencoded");
            if (t == "gcd" && peek() == "(") throw ParseFail("gcd unencoded");
            if (t == "lcm" && peek() == "(") throw ParseFail("lcm unencoded");
            if (t == "clamp" && peek() == "(") throw ParseFail("clamp unencoded");
            if (t == "exchange" && peek() == "(") throw ParseFail("exchange unencoded");
            if (t == "to_address" && peek() == "(") throw ParseFail("to_address unencoded");
            if (t == "addressof" && peek() == "(") throw ParseFail("addressof unencoded");
            if (t == "assume_aligned" && peek() == "(") throw ParseFail("assume_aligned unencoded");
            if (t == "as_const" && peek() == "(") throw ParseFail("as_const unencoded");
            if ((t == "transform_inclusive_scan" || t == "transform_exclusive_scan") && peek() == "(")
                throw ParseFail("transform_inclusive_scan unencoded");
            if (t == "exclusive_scan" && peek() == "(") throw ParseFail("exclusive_scan unencoded");
            if (t == "inclusive_scan" && peek() == "(") throw ParseFail("inclusive_scan unencoded");
            if (t == "transform_reduce" && peek() == "(") throw ParseFail("transform_reduce unencoded");
            if ((t == "uninitialized_fill" || t == "uninitialized_fill_n"
                 || t == "uninitialized_default_construct"
                 || t == "uninitialized_default_construct_n")
                && peek() == "(")
                throw ParseFail("uninitialized_fill unencoded");
            if ((t == "uninitialized_value_construct" || t == "uninitialized_value_construct_n")
                && peek() == "(")
                throw ParseFail("uninitialized_value_construct unencoded");
            if ((t == "uninitialized_copy" || t == "uninitialized_move"
                 || t == "uninitialized_copy_n" || t == "uninitialized_move_n")
                && peek() == "(")
                throw ParseFail("uninitialized_copy unencoded");
            if ((t == "construct_at" || t == "destroy_at") && peek() == "(")
                throw ParseFail("construct_at unencoded");
            if (t == "destroy_n" && peek() == "(") throw ParseFail("destroy_n unencoded");
            if ((t == "add_sat" || t == "sub_sat" || t == "mul_sat" || t == "div_sat"
                 || t == "saturate_cast")
                && peek() == "(")
                throw ParseFail("add_sat unencoded");
            if (t == "type_identity") throw ParseFail("type_identity unencoded");
            if (t == "nontype") throw ParseFail("nontype unencoded");
            if (t == "is_layout_compatible") throw ParseFail("is_layout_compatible unencoded");
            if (t == "is_pointer_interconvertible_with_class"
                || t == "is_pointer_interconvertible_base_of")
                throw ParseFail("is_pointer_interconvertible unencoded");
            if (t == "basic_const_iterator") throw ParseFail("basic_const_iterator unencoded");
            if (t == "is_corresponding_member") throw ParseFail("is_corresponding_member unencoded");
            if (t == "forward_like") throw ParseFail("forward_like unencoded");
            if (t == "make_exception_ptr" && peek() == "(")
                throw ParseFail("make_exception_ptr unencoded");
            if ((t == "set_terminate" || t == "get_terminate") && peek() == "(")
                throw ParseFail("set_terminate unencoded");
            if (t == "is_constant_evaluated" && peek() == "(")
                throw ParseFail("is_constant_evaluated unencoded");
            if (t == "lerp" && peek() == "(") throw ParseFail("lerp unencoded");
            if (t == "midpoint" && peek() == "(") throw ParseFail("midpoint unencoded");
            if ((t == "cmp_less" || t == "cmp_greater" || t == "cmp_less_equal"
                 || t == "cmp_greater_equal" || t == "cmp_equal_to" || t == "cmp_not_equal_to"
                 || t == "in_range")
                && peek() == "(")
                throw ParseFail("cmp_less unencoded");
            if ((t == "countl_zero" || t == "countr_zero" || t == "countl_one" || t == "countr_one")
                && peek() == "(")
                throw ParseFail("countl_zero unencoded");
            if (t == "unreachable" && peek() == "(") throw ParseFail("std::unreachable unencoded");
            if (t == "condition_variable" || t == "shared_mutex")
                throw ParseFail("condition_variable unencoded");
            if (peek() == "[") {
                eat("[");
                auto idx = parse(0);
                eat("]");
                if (!e.arrays.count(t)) throw ParseFail("unknown array " + t);
                auto [arr, n] = e.arrays.at(t);
                e.add_prop("oob-read", "MEM-OOB-READ", oob(e, idx, n), e.pc);
                return z3::select(arr, idx);
            }
            if (peek() == "(") {
                eat("(");
                int depth = 1;
                while (depth) {
                    auto ntok = eat();
                    if (ntok == "(") ++depth;
                    else if (ntok == ")") --depth;
                }
                return e.bv("call_" + t);
            }
            if (peek() == "++" || peek() == "--") {
                auto op = eat();
                e.check_read(t);
                auto cur = e.get(t);
                auto one = e.ctx.bv_val(1, WIDTH);
                auto nw = apply_binop(e, cur, op == "++" ? "+" : "-", one);
                e.set(t, nw);
                e.mark_init(t);
                return cur;
            }
            if (e.arrays.count(t)) return e.ctx.bv_val(1, WIDTH);
            if (e.vars.count(t)) {
                e.check_read(t);
                return e.get(t);
            }
            if (parser.enums.count(t)) return e.ctx.bv_val(parser.enums.at(t), WIDTH);
            return e.get(t);
        }
        throw ParseFail("bad token " + t);
    };
    parse = [&](int minp) -> z3::expr {
        auto left = nud();
        while (PREC.count(peek()) && PREC.at(peek()) >= minp) {
            auto op = eat();
            auto right = parse(PREC.at(op) + 1);
            if (op == "&&")
                left = z3::ite(as_bool(left) && as_bool(right), e.ctx.bv_val(1, WIDTH), e.ctx.bv_val(0, WIDTH));
            else if (op == "||")
                left = z3::ite(as_bool(left) || as_bool(right), e.ctx.bv_val(1, WIDTH), e.ctx.bv_val(0, WIDTH));
            else
                left = apply_binop(e, left, op, right);
        }
        if (minp <= 5 && peek() == "?") {
            eat("?");
            auto then_v = parse(0);
            eat(":");
            auto else_v = parse(5);
            left = z3::ite(as_bool(left), then_v, else_v);
        }
        if (minp <= 1 && peek() == ",") {
            eat(",");
            left = parse(0);
        }
        return left;
    };
    auto v = parse(0);
    if (pos != tokens.size()) {
        std::string trail;
        for (size_t i = pos; i < tokens.size(); ++i) {
            if (i > pos) trail += " ";
            trail += tokens[i];
        }
        throw ParseFail("trailing " + trail);
    }
    return v;
}

std::string cex(const z3::model& model, Enc& e, const std::vector<std::pair<std::string, std::string>>& params) {
    std::vector<std::string> bits;
    for (auto& [typ, name] : params) {
        if (name.empty()) continue;
        try {
            auto d = model.eval(e.ctx.bv_const(name.c_str(), static_cast<unsigned>(type_width(typ))), true);
            bits.push_back(name + "=" + d.to_string());
        } catch (...) {}
    }
    std::string out;
    for (size_t i = 0; i < bits.size(); ++i) {
        if (i) out += ", ";
        out += bits[i];
    }
    return out;
}

bool loop_kw(std::string_view src) {
    return rx_search("\\b(do|while|for)\\b", src);
}

std::optional<std::vector<std::tuple<std::string, std::string, std::string>>>
extract_simple_loops(std::string text) {
    std::vector<std::tuple<std::string, std::string, std::string>> loops;
    std::function<void(std::string)> walk;
    walk = [&](std::string src) {
        while (!src.empty()) {
            src = lstrip(src);
            if (src.empty()) break;
            if (src.starts_with("{")) {
                auto [inner, rest] = brace(src);
                walk(inner);
                src = rest;
                continue;
            }
            if (starts_kw(src, "do")) {
                auto rest = lstrip(src.substr(2));
                auto [body, r1] = block_or_stmt(rest);
                rest = lstrip(r1);
                if (!starts_kw(rest, "while")) throw ParseFail("do without while");
                rest = lstrip(rest.substr(5));
                auto [cond, r2] = paren(rest);
                rest = lstrip(r2);
                if (rest.starts_with(";")) rest = rest.substr(1);
                if (loop_kw(body)) throw ParseFail("nested loop");
                loops.emplace_back("do", cond, body);
                src = rest;
                continue;
            }
            if (starts_kw(src, "while")) {
                auto rest = lstrip(src.substr(5));
                auto [cond, r1] = paren(rest);
                auto [body, r2] = block_or_stmt(r1);
                if (loop_kw(body)) throw ParseFail("nested loop");
                loops.emplace_back("while", cond, body);
                src = r2;
                continue;
            }
            if (starts_kw(src, "for")) {
                auto rest = lstrip(src.substr(3));
                auto [head, r1] = paren(rest);
                auto [body, r2] = block_or_stmt(r1);
                if (loop_kw(body)) throw ParseFail("nested loop");
                auto parts = split_semi(head);
                while (parts.size() < 3) parts.push_back("");
                auto cond = strip(parts[1]);
                auto incr = strip(parts[2]);
                auto incr_stmt = incr.empty() || incr.ends_with(";") ? incr : incr + ";";
                loops.emplace_back("for", cond.empty() ? "1" : cond, body + "\n" + incr_stmt);
                src = r2;
                continue;
            }
            if (starts_kw(src, "if")) {
                auto rest = lstrip(src.substr(2));
                auto [_, r1] = paren(rest);
                auto [then_src, r2] = block_or_stmt(r1);
                walk(then_src);
                auto r2s = lstrip(r2);
                if (starts_kw(r2s, "else")) {
                    auto [else_src, r3] = block_or_stmt(r2s.substr(4));
                    walk(else_src);
                    src = r3;
                } else src = r2;
                continue;
            }
            if (starts_kw(src, "switch")) {
                auto rest = lstrip(src.substr(6));
                auto [_, r1] = paren(rest);
                auto [body, r2] = block_or_stmt(r1);
                walk(body);
                src = r2;
                continue;
            }
            if (starts_kw(src, "case")) {
                auto rest = lstrip(src.substr(4));
                auto [__, t2] = upto_colon(rest);
                src = t2;
                continue;
            }
            if (starts_kw(src, "default")) {
                auto rest = lstrip(src.substr(7));
                if (!rest.starts_with(":")) throw ParseFail("expected : after default");
                src = rest.substr(1);
                continue;
            }
            auto [___, t2] = consume_stmt_src(src);
            src = t2;
        }
    };
    try {
        walk(text);
    } catch (const ParseFail&) {
        return std::nullopt;
    }
    return loops;
}

std::string k_step_body(const std::string& kind, const std::string& cond, const std::string& body, int k) {
    std::string piece = kind == "do" ? body : ("if (" + cond + ") {\n" + body + "\n}");
    std::string out;
    int n = std::max(1, k);
    for (int i = 0; i < n; ++i) {
        if (i) out += "\n";
        out += piece;
    }
    return out;
}

std::vector<int> unwind_schedule(int unwind) {
    std::vector<int> ks;
    int k = 1;
    while (k < std::max(1, unwind)) {
        ks.push_back(k);
        k *= 2;
    }
    if (std::find(ks.begin(), ks.end(), unwind) == ks.end())
        ks.push_back(std::max(1, unwind));
    return ks;
}

std::string param_premise(const std::string& cexs, const std::vector<std::pair<std::string, std::string>>& params) {
    if (cexs.empty() || cexs.find('=') == std::string::npos) return "unattributed";
    std::unordered_set<std::string> named;
    std::stringstream ss(cexs);
    std::string p;
    while (std::getline(ss, p, ',')) {
        auto eq = p.find('=');
        if (eq != std::string::npos) named.insert(strip(p.substr(0, eq)));
    }
    for (auto& [_, n] : params)
        if (!n.empty() && named.count(n)) return "named";
    return "local";
}

Finding mk_base(const FunctionInfo& fn) {
    Finding f;
    f.stage = "bmc";
    f.file = fn.file;
    f.function = fn.name;
    f.line = fn.line;
    f.cls = "";
    f.strength = std::string(laws::STRENGTH_PROVES);
    return f;
}

std::string join_csv(const std::vector<int>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += std::to_string(v[i]);
    }
    return s;
}
std::string join_csv(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += v[i];
    }
    return s;
}

Finding bmc_function(const FunctionInfo& fn, int unwind, bool try_unbounded = true,
                     const std::map<std::string, int>* enums = nullptr,
                     bool allow_local_pointers = false, bool incremental = true);

Finding bmc_once(const FunctionInfo& fn, int unwind, bool try_unbounded,
                 const std::map<std::string, int>& enums, Finding base) {
    Parser p(fn.body, fn.params, unwind, enums);
    auto enc = p.run();
    if (!enc) {
        base.strength = std::string(laws::STRENGTH_SOME);
        auto err = p.err.empty() ? std::string("parse failed") : p.err;
        if (auto msg = harness_for_parsefail(err, "bitvector BMC")) {
            base.status = std::string(laws::NEEDS_HARNESS);
            base.message = *msg;
            return base;
        }
        base.status = std::string(laws::ERROR);
        base.message = "BMC frontend: " + err;
        return base;
    }
    for (auto& prop : enc->props) {
        z3::solver s(enc->ctx);
        s.set("timeout", 8000u);
        s.add(prop.cond);
        auto r = s.check();
        if (r == z3::sat) {
            auto c = cex(s.get_model(), *enc, fn.params);
            if (c.empty()) c = prop.name + "=sat";
            Finding f;
            f.stage = "bmc";
            f.status = std::string(laws::FAILED);
            f.file = fn.file;
            f.function = fn.name;
            f.line = fn.line;
            f.cls = prop.cls;
            f.message = prop.name + ": " + prop.cls;
            f.strength = std::string(laws::STRENGTH_PROVES);
            f.counterexample = c;
            f.extra["oracle"] = "false";
            f.extra["unwind"] = std::to_string(unwind);
            f.extra["param_premise"] = param_premise(c, fn.params);
            return f;
        }
        if (r == z3::unknown) {
            base.status = std::string(laws::UNKNOWN);
            base.message = "solver unknown on " + prop.name;
            return base;
        }
    }
    if (enc->props.empty() && has_unencoded_libc_effect(fn)) {
        base.strength = std::string(laws::STRENGTH_SOME);
        base.status = std::string(laws::NEEDS_HARNESS);
        base.message = "unmodeled libc side-effect: unconstrained call is not a proof";
        return base;
    }
    Finding f;
    f.stage = "bmc";
    f.file = fn.file;
    f.function = fn.name;
    f.line = fn.line;
    f.strength = std::string(laws::STRENGTH_PROVES);
    f.extra["unwind"] = std::to_string(unwind);
    f.extra["unwind_closed"] = enc->unwind_ok ? "true" : "false";
    if (enc->unwind_ok && try_unbounded) {
        f.status = std::string(laws::PROVED_UNBOUNDED);
        f.message = "encoded UB properties hold for all unrollings";
        return f;
    }
    if (enc->unwind_ok) {
        f.status = std::string(laws::PROVED);
        f.message = "properties hold, loops closed at unwind " + std::to_string(unwind);
        return f;
    }
    f.status = std::string(laws::BOUNDED);
    f.message = "no violation within unwind " + std::to_string(unwind)
                + "; loops did not close";
    return f;
}

Finding bmc_function(const FunctionInfo& fn, int unwind, bool try_unbounded,
                     const std::map<std::string, int>* enums,
                     bool allow_local_pointers, bool incremental) {
    auto base = mk_base(fn);
    if (fn.kind == "POINTER") {
        base.strength = std::string(laws::STRENGTH_SOME);
        base.status = std::string(laws::NEEDS_HARNESS);
        base.message = "pointer parameter: unguarded BMC reports missing preconditions, not defects";
        return base;
    }
    if (fn.kind == "OTHER") {
        base.strength = std::string(laws::STRENGTH_SOME);
        base.status = std::string(laws::NEEDS_HARNESS);
        base.message = "non-scalar parameter: unguarded BMC reports missing preconditions, not defects";
        return base;
    }
    if (auto syn = unencoded_syntax_reason(fn, "bitvector BMC")) {
        base.strength = std::string(laws::STRENGTH_SOME);
        base.status = std::string(laws::NEEDS_HARNESS);
        base.message = *syn;
        return base;
    }
    if (!allow_local_pointers && body_needs_pointer_harness(fn.body)) {
        base.strength = std::string(laws::STRENGTH_SOME);
        base.status = std::string(laws::NEEDS_HARNESS);
        base.message = "local pointer or heap object: unguarded BMC reports missing preconditions, not defects";
        return base;
    }
    if (has_self_call(fn)) {
        base.strength = std::string(laws::STRENGTH_SOME);
        base.status = std::string(laws::NEEDS_HARNESS);
        base.message = "recursive call unencoded: unconstrained result is not a proof of the callee";
        return base;
    }
    if (has_unencoded_float(fn)) {
        base.strength = std::string(laws::STRENGTH_SOME);
        base.status = std::string(laws::NEEDS_HARNESS);
        base.message = "float/double unencoded: bitvector BMC is not an IEEE model";
        return base;
    }
    if (has_unencoded_cxx(fn)) {
        base.strength = std::string(laws::STRENGTH_SOME);
        base.status = std::string(laws::NEEDS_HARNESS);
        base.message = "C++ view/span unencoded: bitvector BMC is not a lifetime model";
        return base;
    }
    if (has_unencoded_cstr(fn)) {
        base.strength = std::string(laws::STRENGTH_SOME);
        base.status = std::string(laws::NEEDS_HARNESS);
        base.message = "libc string copy unencoded: unconstrained call is not a proof of the buffer";
        return base;
    }
    if (has_unencoded_throw(fn)) {
        base.strength = std::string(laws::STRENGTH_SOME);
        base.status = std::string(laws::NEEDS_HARNESS);
        base.message = "C++ throw unencoded: bitvector BMC is not an exception model";
        return base;
    }
    if (has_unencoded_setjmp(fn)) {
        base.strength = std::string(laws::STRENGTH_SOME);
        base.status = std::string(laws::NEEDS_HARNESS);
        base.message = "setjmp/longjmp/va_list unencoded: bitvector BMC is not a nonlocal-control model";
        return base;
    }
    std::map<std::string, int> local_en;
    if (!enums) local_en = enums_from_fn(fn);
    const auto& en = enums ? *enums : local_en;
    auto schedule = incremental ? unwind_schedule(unwind) : std::vector<int>{unwind};
    Finding last;
    bool have = false;
    std::vector<int> tried;
    for (int k : schedule) {
        auto rec = bmc_once(fn, k, try_unbounded, en, base);
        tried.push_back(k);
        rec.extra["incremental_k"] = std::to_string(k);
        rec.extra["incremental"] = join_csv(tried);
        if (rec.status != laws::BOUNDED) return rec;
        last = rec;
        have = true;
    }
    if (have) return last;
    base.status = std::string(laws::ERROR);
    base.message = "empty unwind schedule";
    return base;
}

Finding k_induction(const FunctionInfo& fn, int unwind, bool allow_local_pointers = false) {
    auto rec = bmc_function(fn, unwind, true, nullptr, allow_local_pointers, true);
    if (rec.status != laws::BOUNDED) {
        rec.extra["k_induction"] = "not-needed";
        return rec;
    }
    auto loops = extract_simple_loops(fn.body);
    if (!loops || loops->empty()) {
        rec.extra["k_induction"] = "unencoded";
        return rec;
    }
    std::string last_open_cls;
    std::vector<int> tried;
    for (int kstep : {1, 2}) {
        bool step_open = false;
        std::string step_cls;
        bool unencoded = false;
        std::vector<std::string> k_steps;
        for (auto& [kind, cond, body] : *loops) {
            auto piece = k_step_body(kind, cond, body, kstep);
            FunctionInfo cloned = fn;
            cloned.body = piece;
            auto step = bmc_function(cloned, 1, false, nullptr, true, true);
            k_steps.push_back(step.status);
            if (step.status == laws::FAILED) {
                step_open = true;
                step_cls = step.cls;
            } else if (step.status != laws::PROVED && step.status != laws::PROVED_UNBOUNDED) {
                // Python engine: only PROVED / PROVED-UNBOUNDED close a step.
                // BOUNDED is not a closed proof and must not fold into
                // PROVED-UNBOUNDED.
                unencoded = true;
            }
        }
        tried.push_back(kstep);
        rec.extra["k_steps"] = join_csv(k_steps);
        rec.extra["k_induction_tried"] = join_csv(tried);
        rec.extra["k_induction_k"] = std::to_string(kstep);
        if (!step_open && !unencoded) {
            Finding f;
            f.stage = "bmc";
            f.status = std::string(laws::PROVED_UNBOUNDED);
            f.file = fn.file;
            f.function = fn.name;
            f.line = fn.line;
            f.message = "k-induction step closed at k=" + std::to_string(kstep) +
                        "; not a bounded-only result";
            f.strength = std::string(laws::STRENGTH_PROVES);
            f.extra = rec.extra;
            f.extra["k_induction"] = "closed";
            f.extra["unwind_closed"] = "true";
            return f;
        }
        if (step_open) last_open_cls = step_cls;
        if (unencoded) break;
    }
    rec.extra["k_induction_tried"] = join_csv(tried);
    if (!last_open_cls.empty()) {
        rec.extra["k_induction"] = "step-open";
        rec.extra["k_induction_cls"] = last_open_cls;
        return rec;
    }
    rec.extra["k_induction"] = "unencoded";
    return rec;
}

// Hook (roadmap 4.2): a function k-induction leaves BOUNDED gets one more try
// with the step strengthened by Houdini-filtered invariants (prism::ai).
// Only a closed step plus a holding base case turns it PROVED-UNBOUNDED.
Finding k_induction_strengthened(const FunctionInfo& fn, int unwind, bool allow_local_pointers) {
    auto rec = k_induction(fn, unwind, allow_local_pointers);
    if (rec.status == laws::PROVED_UNBOUNDED && rec.extra["k_induction"] == "closed") {
        // The k-induction step checks each loop body from an arbitrary state;
        // code after a loop was only checked on paths within the unwind (e.g.
        // `while (i < n) i++; return 100 / (i - 500);`). Keep PROVED-UNBOUNDED
        // only when the loop cut, which covers the post-loop code, closes too.
        auto as_bounded = rec;
        as_bounded.status = std::string(laws::BOUNDED);
        auto v = ai::strengthen_bounded(fn, as_bounded, unwind);
        if (v.status == laws::PROVED_UNBOUNDED) {
            for (auto* k : {"invariants", "invariant_source", "ai_audit_id", "ai_checker", "ai_checker_result"})
                if (v.extra.count(k)) rec.extra[k] = v.extra[k];
            rec.extra["post_loop_check"] = "closed (loop cut)";
            return rec;
        }
        v.status = std::string(laws::BOUNDED);
        v.message = "k-induction step closed for the loop body only; code after the loop is not covered by "
                    "that step and the loop-cut check did not close; no violation within unwind " +
                    std::to_string(unwind);
        v.extra["k_induction"] = "step-closed-post-open";
        v.extra["unwind_closed"] = "false";
        return v;
    }
    if (rec.status != laws::BOUNDED) return rec;
    return ai::strengthen_bounded(fn, rec, unwind);
}

#endif  // PRISM_HAS_Z3


}  // namespace

namespace ai {

ProgramCheck check_program(const FunctionInfo& fn, const std::string& body, int unwind,
                           unsigned timeout_ms, bool only_invariants) {
    ProgramCheck out;
#ifdef PRISM_HAS_Z3
    try {
        Parser p(body, fn.params, unwind, enums_from_fn(fn));
        p.ai_hooks = true;
        auto enc = p.run();
        if (!enc) {
            out.error = p.err.empty() ? std::string("parse failed") : p.err;
            return out;
        }
        out.encoded = true;
        out.unwind_ok = enc->unwind_ok;
        // One call for all properties first: unsat means every one holds.
        // Otherwise the model refutes each property it satisfies (Houdini
        // drops many candidates per solver call); the rest are re-checked.
        std::vector<int> todo;
        for (int i = 0; i < static_cast<int>(enc->props.size()); ++i) {
            bool inv = enc->props[static_cast<size_t>(i)].name.rfind("ai-inv#", 0) == 0;
            if (!only_invariants || inv) todo.push_back(i);
        }
        std::map<int, std::string> verdict, models;
        auto model_text = [](const z3::model& m) {
            std::string txt;
            int shown = 0;
            for (unsigned i = 0; i < m.num_consts() && shown < 24; ++i) {
                auto d = m.get_const_decl(i);
                if (!d.range().is_bv()) continue;
                auto v = m.get_const_interp(d);
                if (!txt.empty()) txt += ", ";
                txt += d.name().str() + "=" + v.to_string();
                ++shown;
            }
            return txt;
        };
        for (int guard = 0; !todo.empty() && guard < 64; ++guard) {
            z3::expr_vector any(enc->ctx);
            for (int i : todo) any.push_back(enc->props[static_cast<size_t>(i)].cond);
            z3::solver s(enc->ctx);
            s.set("timeout", timeout_ms);
            s.add(z3::mk_or(any));
            auto r = s.check();
            if (r == z3::unsat) {
                for (int i : todo) verdict[i] = "unsat";
                todo.clear();
                break;
            }
            if (r != z3::sat) break;
            auto m = s.get_model();
            std::vector<int> left;
            for (int i : todo) {
                auto v = m.eval(enc->props[static_cast<size_t>(i)].cond, true);
                if (v.is_true()) {
                    verdict[i] = "sat";
                    models[i] = model_text(m);
                } else {
                    left.push_back(i);
                }
            }
            if (left.size() == todo.size()) break;  // no progress: fall back to single checks
            todo = std::move(left);
        }
        for (int i : todo) {
            z3::solver s(enc->ctx);
            s.set("timeout", timeout_ms);
            s.add(enc->props[static_cast<size_t>(i)].cond);
            auto r = s.check();
            verdict[i] = r == z3::sat ? "sat" : r == z3::unsat ? "unsat" : "unknown";
            if (r == z3::sat) models[i] = model_text(s.get_model());
        }
        for (int i = 0; i < static_cast<int>(enc->props.size()); ++i) {
            auto& prop = enc->props[static_cast<size_t>(i)];
            ProgramProp pp;
            pp.name = prop.name;
            pp.cls = prop.cls;
            auto it = verdict.find(i);
            bool inv = prop.name.rfind("ai-inv#", 0) == 0;
            if (only_invariants && !inv) pp.result = "skipped";
            else pp.result = it == verdict.end() ? "unknown" : it->second;
            if (auto mt = models.find(i); mt != models.end()) pp.model = mt->second;
            out.props.push_back(std::move(pp));
        }
    } catch (const z3::exception& ex) {
        out.encoded = false;
        out.error = std::string("z3: ") + ex.msg();
    } catch (const std::exception& ex) {
        out.encoded = false;
        out.error = ex.what();
    }
#else
    (void)fn;
    (void)body;
    (void)unwind;
    (void)timeout_ms;
    out.error = "z3 not built";
#endif
    return out;
}

}  // namespace ai

ForkFlipResult solve_fork_flip(const FunctionInfo& fn, const std::map<std::string, int>& seed,
                               const std::string& cond, bool want) {
#ifdef PRISM_HAS_Z3
    // Mirrors prism.concolic._z3_solve_flip: Enc + Parser::expr on the
    // negated branch, then getInitialValues. Solver::False → drop the side.
    try {
        Enc e(0);
        std::vector<std::string> names;
        for (auto& [typ, name] : fn.params) {
            if (name.empty()) continue;
            names.push_back(name);
            if (type_is_unsigned(typ)) e.unsigned_names.insert(name);
            e.bits[name] = type_width(typ);
            e.get(name);
        }
        e.retag_unsigned();
        Parser parser("", fn.params, 0, enums_from_fn(fn));
        auto cond_z3 = as_bool(parser.expr(e, cond));
        z3::solver s(e.ctx);
        s.set("timeout", 2000u);
        s.add(want ? cond_z3 : !cond_z3);
        z3::expr_vector diffs(e.ctx);
        for (auto& name : names) {
            auto it = e.vars.find(name);
            if (it == e.vars.end()) continue;
            int w = e.bits.count(name) ? e.bits[name] : WIDTH;
            int cur = 0;
            if (auto sit = seed.find(name); sit != seed.end()) cur = sit->second;
            uint64_t mask = w >= 64 ? ~uint64_t{0} : ((uint64_t{1} << w) - 1);
            uint64_t bitsv = static_cast<uint64_t>(static_cast<int64_t>(cur)) & mask;
            diffs.push_back(it->second != e.ctx.bv_val(bitsv, static_cast<unsigned>(w)));
        }
        z3::check_result verdict = z3::unknown;
        if (diffs.empty()) {
            verdict = s.check();
        } else {
            s.push();
            s.add(z3::mk_or(diffs));
            verdict = s.check();
            if (verdict != z3::sat) {
                s.pop();
                verdict = s.check();
            }
        }
        if (verdict == z3::unsat) return {ForkFlipKind::Unsat, {}};
        if (verdict != z3::sat) return {};
        auto model = s.get_model();
        std::map<std::string, int> out;
        auto to_i32 = [](int64_t x) -> int {
            auto u = static_cast<uint32_t>(static_cast<uint64_t>(x) & 0xFFFFFFFFu);
            return static_cast<int>(static_cast<int32_t>(u));
        };
        for (auto& name : names) {
            auto it = e.vars.find(name);
            if (it == e.vars.end()) {
                out[name] = seed.count(name) ? to_i32(seed.at(name)) : 0;
                continue;
            }
            auto val = model.eval(it->second, true);
            uint64_t u = 0;
            int64_t si = 0;
            int raw = 0;
            if (val.is_numeral_u64(u)) {
                raw = to_i32(static_cast<int64_t>(u));
            } else if (val.is_numeral_i64(si)) {
                raw = to_i32(si);
            } else {
                out[name] = seed.count(name) ? to_i32(seed.at(name)) : 0;
                continue;
            }
            out[name] = raw;
        }
        return {ForkFlipKind::Model, std::move(out)};
    } catch (const ParseFail&) {
        return {};
    } catch (const z3::exception&) {
        return {};
    } catch (...) {
        return {};
    }
#else
    (void)fn;
    (void)seed;
    (void)cond;
    (void)want;
    return {};
#endif
}

std::vector<Finding> run_bmc(const std::vector<FunctionInfo>& functions, int unwind,
                             bool allow_local_pointers) {
#ifdef PRISM_HAS_Z3
    std::vector<Finding> out;
    for (auto& fn : inline_static(functions))
        out.push_back(k_induction_strengthened(fn, unwind, allow_local_pointers));
    // Python engine k_induction always returns a Finding (empty unwind → ERROR).
    // A non-empty function list must never look like a silent clean BMC stage.
    if (out.empty() && !functions.empty()) {
        Finding f;
        f.stage = "bmc";
        f.status = std::string(laws::ERROR);
        f.message = "empty unwind schedule";
        f.strength = std::string(laws::STRENGTH_PROVES);
        out.push_back(std::move(f));
    }
    return out;
#else
    (void)functions;
    (void)unwind;
    Finding f;
    f.stage = "bmc";
    f.status = std::string(laws::NOTRUN);
    f.message = "z3 not built";
    f.strength = std::string(laws::STRENGTH_PROVES);
    f.extra["install"] = "rebuild with -DPRISM_Z3=ON (vendored third_party/z3)";
    return {f};
#endif
}

}  // namespace prism
