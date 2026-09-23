#include "prism/stages.hpp"
#include "prism/cparse.hpp"
#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/regex.hpp"
#include "prism/sandbox.hpp"
#include "prism/simd.hpp"

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <winhttp.h>
#  include <io.h>
#  pragma comment(lib, "winhttp.lib")
#  ifdef min
#    undef min
#  endif
#  ifdef max
#    undef max
#  endif
#  ifdef ERROR
#    undef ERROR
#  endif
#  ifdef OPTIONAL
#    undef OPTIONAL
#  endif
#  ifdef CONST
#    undef CONST
#  endif
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#endif

#include <nlohmann/json.hpp>

#ifdef PRISM_HAS_LLAMA
#  include "llama.h"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace prism {
namespace fs = std::filesystem;
namespace {

constexpr int WIDTH = 32;
constexpr int64_t INT_MIN_32 = -(int64_t{1} << (WIDTH - 1));
constexpr int64_t INT_MAX_32 = (int64_t{1} << (WIDTH - 1)) - 1;
constexpr int64_t INT64_MIN_V = std::numeric_limits<int64_t>::min();
constexpr int64_t INT64_MAX_V = std::numeric_limits<int64_t>::max();
constexpr int MAX_STEPS = 10000;
constexpr int F_BOUND = 8;

const std::unordered_set<std::string> kCastWords = {
    "char", "short", "int", "long", "unsigned", "signed", "const", "volatile", "void",
    "_Bool", "bool", "uint32_t", "int32_t", "uint64_t", "int64_t", "size_t",
};
const std::unordered_set<std::string> kDeclKws = {
    "int", "unsigned", "long", "short", "char", "uint32_t", "int32_t", "uint64_t", "int64_t",
    "size_t",
};
const std::unordered_set<std::string> kStmtStartWords = {
    "if",     "for",        "while",    "switch",   "return",   "sizeof",   "typeof",
    "else",   "do",         "case",     "default",  "goto",     "break",    "continue",
    "assert", "throw",      "try",      "catch",    "asm",      "__asm__",  "__asm",
    "typedef","static",     "extern",   "auto",     "register", "int",      "unsigned",
    "signed", "long",       "short",    "char",     "uint32_t", "int32_t",  "uint64_t",
    "int64_t","size_t",     "void",     "float",    "double",   "_Bool",    "bool",
    "const",  "volatile",   "_Atomic",  "struct",   "union",    "enum",
};
const std::unordered_set<std::string> kCallKw = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof", "__typeof__", "else", "do",
    "case", "default", "_Generic", "break", "continue", "goto", "struct", "union", "enum",
    "assert", "static_assert", "_Static_assert", "alignof", "_Alignof", "__attribute__",
    "offsetof",
};
const std::map<std::string, int> kTypeSize = {
    {"char", 1}, {"signed char", 1}, {"unsigned char", 1},
    {"short", 2}, {"short int", 2}, {"signed short", 2}, {"unsigned short", 2},
    {"int", 4}, {"signed", 4}, {"signed int", 4}, {"unsigned", 4}, {"unsigned int", 4},
    {"long", 4}, {"long int", 4}, {"unsigned long", 4},
    {"long long", 8}, {"long long int", 8}, {"unsigned long long", 8},
    {"uint32_t", 4}, {"int32_t", 4}, {"size_t", 4}, {"_Bool", 1}, {"bool", 1},
};
const std::map<std::string, int> kCTypeSize = {
    {"char", 1}, {"signed char", 1}, {"unsigned char", 1},
    {"short", 2}, {"unsigned short", 2},
    {"int", 4}, {"unsigned", 4}, {"unsigned int", 4},
    {"long", 8}, {"unsigned long", 8},
    {"long long", 8}, {"unsigned long long", 8},
    {"int8_t", 1}, {"uint8_t", 1}, {"int16_t", 2}, {"uint16_t", 2},
    {"int32_t", 4}, {"uint32_t", 4}, {"int64_t", 8}, {"uint64_t", 8},
    {"size_t", 8}, {"ssize_t", 8}, {"bool", 1}, {"_Bool", 1},
};

struct ParseFail : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct UB : std::runtime_error {
    std::string cls;
    explicit UB(std::string c) : std::runtime_error(c), cls(std::move(c)) {}
};
struct ReturnEx : std::exception {
    std::optional<int64_t> value;
    explicit ReturnEx(std::optional<int64_t> v = std::nullopt) : value(v) {}
};
struct BreakEx : std::exception {};
struct ContinueEx : std::exception {};
struct PredFail : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string strip(std::string s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
std::string lstrip(std::string s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}
std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string join_sv(const std::vector<std::string>& v, std::string_view sep) {
    std::string o;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) o += sep;
        o += v[i];
    }
    return o;
}
bool is_ident(std::string_view t) {
    if (t.empty() || !(std::isalpha(static_cast<unsigned char>(t[0])) || t[0] == '_'))
        return false;
    for (std::size_t i = 1; i < t.size(); ++i)
        if (!(std::isalnum(static_cast<unsigned char>(t[i])) || t[i] == '_')) return false;
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
    return lstrip(std::string(text.substr(4))).starts_with("*");
}
bool is_nested_function(std::string_view text) {
    static Regex re(
        "(?:void|int|unsigned(?:\\s+int)?|long(?:\\s+int)?|short|char|"
        "float|double|_Bool|bool)\\s+[A-Za-z_]\\w*\\s*\\([^)]*\\)\\s*\\{");
    auto s = lstrip(std::string(text));
    auto m = re.search_match(s, 0);
    return m && !m->spans.empty() && m->spans[0].first == 0;
}
bool looks_like_decl(std::string_view stmt) {
    auto s = lstrip(std::string(stmt));
    for (auto& kw : kDeclKws)
        if (starts_kw(s, kw)) return true;
    return false;
}
bool type_is_unsigned(std::string_view typ) {
    static Regex re("(?i)\\bunsigned\\b|\\bsize_t\\b|\\buint\\d*_t\\b|\\bu_int\\b|\\bu_long\\b");
    return re.search(typ);
}
int type_width(std::string_view typ) {
    std::string t = lower_copy(std::string(typ));
    if (t.find("long long") != std::string::npos || re_search("\\b[iu]nt64_t\\b", t)) return 64;
    return WIDTH;
}
std::optional<Match> match_at(const Regex& re, std::string_view s) {
    auto m = re.search_match(s, 0);
    if (!m || m->spans.empty() || m->spans[0].first != 0) return std::nullopt;
    return m;
}
bool fullmatch(const Regex& re, std::string_view s) {
    auto m = match_at(re, s);
    return m && static_cast<std::size_t>(m->spans[0].second) == s.size();
}
int32_t i32(int64_t x) {
    auto u = static_cast<uint32_t>(static_cast<uint64_t>(x) & 0xFFFFFFFFu);
    if (u >= 0x80000000u) return static_cast<int32_t>(static_cast<int64_t>(u) - 0x100000000LL);
    return static_cast<int32_t>(u);
}
uint32_t u32(int64_t x) { return static_cast<uint32_t>(static_cast<uint64_t>(x) & 0xFFFFFFFFu); }
bool truth(int64_t v) { return i32(v) != 0; }

Finding make_find(std::string stage, std::string_view status, const FunctionInfo& fn, std::string cls,
                  std::string msg, std::string_view strength) {
    Finding f;
    f.stage = std::move(stage);
    f.status = std::string(status);
    f.file = fn.file;
    f.function = fn.name;
    f.line = fn.line;
    f.cls = std::move(cls);
    f.message = std::move(msg);
    f.strength = std::string(strength);
    return f;
}
Finding nr(std::string stage, std::string msg, std::string strength = std::string(laws::STRENGTH_READS)) {
    Finding f;
    f.stage = std::move(stage);
    f.status = std::string(laws::NOTRUN);
    f.message = std::move(msg);
    f.strength = std::move(strength);
    return f;
}

std::vector<std::string> split_comma(const std::string& s) {
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
std::vector<std::string> split_semi(const std::string& s) {
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
std::pair<std::string, std::string> paren(std::string text) {
    text = lstrip(std::move(text));
    if (!text.starts_with("(")) throw ParseFail("expected (");
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '(') ++depth;
        else if (text[i] == ')') {
            --depth;
            if (depth == 0) return {text.substr(1, i - 1), text.substr(i + 1)};
        }
    }
    throw ParseFail("unbalanced (");
}
std::pair<std::string, std::string> brace(std::string text) {
    text = lstrip(std::move(text));
    if (!text.starts_with("{")) throw ParseFail("expected {");
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        else if (text[i] == '}') {
            --depth;
            if (depth == 0) return {text.substr(1, i - 1), text.substr(i + 1)};
        }
    }
    throw ParseFail("unbalanced {");
}
std::pair<std::string, std::string> stmt(const std::string& text) {
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (ch == '(') ++depth;
        else if (ch == ')') --depth;
        else if (ch == '{' && depth == 0) break;
        else if (ch == ';' && depth == 0) return {text.substr(0, i + 1), text.substr(i + 1)};
    }
    throw ParseFail("no semicolon in " + text.substr(0, std::min<std::size_t>(80, text.size())));
}
std::pair<std::string, std::string> take_block(std::string text) {
    text = lstrip(std::move(text));
    if (text.starts_with("{")) return brace(text);
    return stmt(text);
}
std::pair<std::string, std::string> take_block_keep_braces(std::string text) {
    text = lstrip(std::move(text));
    if (text.starts_with("{")) {
        auto [inner, rest] = brace(text);
        return {"{" + inner + "}", rest};
    }
    return stmt(text);
}
std::pair<std::string, std::string> upto_colon(const std::string& text) {
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '(') ++depth;
        else if (text[i] == ')') --depth;
        else if (text[i] == ':' && depth == 0) return {text.substr(0, i), text.substr(i + 1)};
    }
    throw ParseFail("expected :");
}
std::vector<std::string> tok(const std::string& src) {
    static Regex rx(
        R"(0x[0-9a-fA-F]+|\d+|'(?:\\.|[^\\'])'|"(?:\\.|[^\\"])*"|[A-Za-z_]\w*|&&|\|\||==|!=|<=|>=|<<|>>|\+\+|--|[+\-*/%<>=!&|^~()[\],?:])");
    std::vector<std::string> out;
    for (auto& m : rx.finditer(src)) out.push_back(m.text);
    return out;
}
int char_lit_value(const std::string& t) {
    if (t.size() < 2) throw ParseFail("empty character literal");
    std::string inner = t.substr(1, t.size() - 2);
    if (inner.empty()) throw ParseFail("empty character literal");
    if (inner[0] == '\\' && inner.size() >= 2) {
        switch (inner[1]) {
        case 'n': return 10;
        case 't': return 9;
        case 'r': return 13;
        case '0': return 0;
        case '\\': return 92;
        case '\'': return 39;
        case '"': return 34;
        default: return static_cast<unsigned char>(inner[1]);
        }
    }
    return static_cast<unsigned char>(inner[0]);
}
std::string string_lit_value(const std::string& t) {
    if (t.size() < 2) return {};
    std::string inner = t.substr(1, t.size() - 2);
    std::string out;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '\\' && i + 1 < inner.size()) {
            char esc = inner[++i];
            switch (esc) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '0': out.push_back('\0'); break;
            default: out.push_back(esc); break;
            }
        } else {
            out.push_back(inner[i]);
        }
    }
    return out;
}

std::string read_text_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
std::optional<fs::path> locate_source(const FunctionInfo& fn) {
    if (fn.file.empty()) return std::nullopt;
    fs::path p = fn.file;
    if (fs::is_regular_file(p)) return p;
    auto cwd = fs::current_path() / p;
    if (fs::is_regular_file(cwd)) return cwd;
    fs::path name = p.filename();
    fs::path walk = fs::current_path();
    for (int i = 0; i < 6; ++i) {
        auto td = walk / "testdata" / name;
        if (fs::is_regular_file(td)) return td;
        if (!walk.has_parent_path() || walk == walk.parent_path()) break;
        walk = walk.parent_path();
    }
    return std::nullopt;
}
std::string read_fn_source(const FunctionInfo& fn) {
    auto p = locate_source(fn);
    if (!p) return {};
    try {
        return read_text_file(*p);
    } catch (...) {
        return {};
    }
}

std::optional<fs::path> which_cc() {
    Config cfg;
    return cfg.which({"gcc", "clang", "cl"});
}

struct ProcRun {
    int rc = -1;
    std::string out;
    std::string err;
    bool timeout = false;
    bool crashed = false;
};

#ifdef _WIN32
std::wstring wide_utf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
ProcRun win_create_process(const std::vector<std::string>& args, const std::string& input, double timeout_s) {
    ProcRun r;
    if (args.empty()) {
        r.err = "no argv";
        return r;
    }
    // CommandLineToArgvW quoting (sandbox::windows_command_line); a .bat/.cmd
    // target runs through cmd.exe, so it gets cmd quoting or is refused.
    std::string cl;
    if (sandbox::is_batch_file(args[0])) {
        auto bl = sandbox::batch_command_line(args);
        if (!bl) {
            r.err = "refusing to pass %, !, \" or a newline to a batch file";
            return r;
        }
        cl = *bl;
    } else {
        cl = sandbox::windows_command_line(args);
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0) ||
        !CreatePipe(&err_r, &err_w, &sa, 0)) {
        r.err = "pipe";
        return r;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    PROCESS_INFORMATION pi{};
    std::wstring wcl = wide_utf8(cl);
    std::vector<wchar_t> buf(wcl.begin(), wcl.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                             &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (!ok) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        CloseHandle(err_r);
        r.err = "CreateProcess failed";
        return r;
    }
    if (!input.empty()) {
        DWORD wr = 0;
        WriteFile(in_w, input.data(), static_cast<DWORD>(input.size()), &wr, nullptr);
    }
    CloseHandle(in_w);
    DWORD ms = static_cast<DWORD>(std::max(100.0, timeout_s * 1000.0));
    DWORD w = WaitForSingleObject(pi.hProcess, ms);
    auto slurp = [](HANDLE h) {
        std::string s;
        char buf[4096];
        DWORD n = 0;
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
            DWORD got = 0;
            if (!ReadFile(h, buf, static_cast<DWORD>(std::min<std::size_t>(sizeof buf, avail)), &got, nullptr) ||
                got == 0)
                break;
            s.append(buf, got);
        }
        return s;
    };
    if (w == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        r.timeout = true;
        WaitForSingleObject(pi.hProcess, 2000);
    }
    r.out = slurp(out_r);
    r.err = slurp(err_r);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.rc = static_cast<int>(code);
    if (code >= 0xC0000000u) r.crashed = true;
    CloseHandle(out_r);
    CloseHandle(err_r);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return r;
}
#endif

// limits: rlimits applied in the forked child (Law 9 sandbox for built
// binaries; the caller wraps argv with sandbox::wrap_argv). Default: none.
ProcRun run_argv(const std::vector<std::string>& args, const std::string& input, double timeout_s,
                 const sandbox::Limits& limits = sandbox::Limits()) {
#ifdef _WIN32
    (void)limits;  // no rlimits on Windows (sandbox kind "none")
    return win_create_process(args, input, timeout_s);
#else
    ProcRun r;
    if (args.empty()) {
        r.err = "no argv";
        return r;
    }
    int in_p[2] = {-1, -1};
    int out_p[2] = {-1, -1};
    int err_p[2] = {-1, -1};
    if (::pipe(in_p) != 0) {
        r.err = "pipe";
        return r;
    }
    if (::pipe(out_p) != 0) {
        ::close(in_p[0]);
        ::close(in_p[1]);
        r.err = "pipe";
        return r;
    }
    if (::pipe(err_p) != 0) {
        ::close(in_p[0]);
        ::close(in_p[1]);
        ::close(out_p[0]);
        ::close(out_p[1]);
        r.err = "pipe";
        return r;
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(in_p[0]);
        ::close(in_p[1]);
        ::close(out_p[0]);
        ::close(out_p[1]);
        ::close(err_p[0]);
        ::close(err_p[1]);
        r.err = "fork failed";
        return r;
    }
    if (pid == 0) {
        sandbox::apply_child_limits(limits);
        ::dup2(in_p[0], STDIN_FILENO);
        ::dup2(out_p[1], STDOUT_FILENO);
        ::dup2(err_p[1], STDERR_FILENO);
        ::close(in_p[0]);
        ::close(in_p[1]);
        ::close(out_p[0]);
        ::close(out_p[1]);
        ::close(err_p[0]);
        ::close(err_p[1]);
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }
    ::close(in_p[0]);
    ::close(out_p[1]);
    ::close(err_p[1]);
    auto set_nb = [](int fd) {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    };
    set_nb(in_p[1]);
    set_nb(out_p[0]);
    set_nb(err_p[0]);
    std::size_t in_off = 0;
    bool in_closed = false;
    auto pump = [&]() {
        char buf[4096];
        if (!in_closed) {
            while (in_off < input.size()) {
                ssize_t n = ::write(in_p[1], input.data() + in_off, input.size() - in_off);
                if (n > 0) {
                    in_off += static_cast<std::size_t>(n);
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            if (in_off >= input.size()) {
                ::close(in_p[1]);
                in_p[1] = -1;
                in_closed = true;
            }
        }
        for (;;) {
            ssize_t n = ::read(out_p[0], buf, sizeof buf);
            if (n > 0) {
                r.out.append(buf, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) break;
            if (errno == EINTR) continue;
            break;
        }
        for (;;) {
            ssize_t n = ::read(err_p[0], buf, sizeof buf);
            if (n > 0) {
                r.err.append(buf, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) break;
            if (errno == EINTR) continue;
            break;
        }
    };
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration<double>(std::max(0.1, timeout_s));
    int st = 0;
    bool reaped = false;
    for (;;) {
        pump();
        pid_t w = ::waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            reaped = true;
            break;
        }
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            ::kill(pid, SIGKILL);
            r.timeout = true;
            while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
            reaped = true;
            break;
        }
        if (w < 0 && errno != EINTR) {
            ::kill(pid, SIGKILL);
            r.timeout = true;
            while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
            reaped = true;
            break;
        }
        const auto remain_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd pfds[3]{};
        nfds_t nfd = 0;
        if (!in_closed) {
            pfds[nfd].fd = in_p[1];
            pfds[nfd].events = POLLOUT;
            ++nfd;
        }
        pfds[nfd].fd = out_p[0];
        pfds[nfd].events = POLLIN;
        ++nfd;
        pfds[nfd].fd = err_p[0];
        pfds[nfd].events = POLLIN;
        ++nfd;
        int pr = ::poll(pfds, nfd, static_cast<int>(std::max<long long>(1, remain_ms)));
        (void)pr;
    }
    if (!reaped) {
        ::kill(pid, SIGKILL);
        r.timeout = true;
        while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        }
    }
    if (!in_closed) {
        ::close(in_p[1]);
        in_p[1] = -1;
        in_closed = true;
    }
    int out_flags = ::fcntl(out_p[0], F_GETFL, 0);
    int err_flags = ::fcntl(err_p[0], F_GETFL, 0);
    if (out_flags >= 0) ::fcntl(out_p[0], F_SETFL, out_flags & ~O_NONBLOCK);
    if (err_flags >= 0) ::fcntl(err_p[0], F_SETFL, err_flags & ~O_NONBLOCK);
    pump();
    ::close(out_p[0]);
    ::close(err_p[0]);
    if (WIFEXITED(st)) {
        r.rc = WEXITSTATUS(st);
    } else if (WIFSIGNALED(st)) {
        r.rc = -WTERMSIG(st);
        if (!r.timeout) r.crashed = true;
    } else {
        r.rc = st;
    }
    return r;
#endif
}

std::optional<std::string> http_request(const std::string& method, const std::string& url, const std::string& body,
                                        int timeout_ms) {
#ifdef _WIN32
    std::string rest = url;
    bool https = false;
    if (rest.starts_with("https://")) {
        https = true;
        rest = rest.substr(8);
    } else if (rest.starts_with("http://")) {
        rest = rest.substr(7);
    }
    auto slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    INTERNET_PORT port = https ? 443 : 80;
    auto colon = hostport.rfind(':');
    std::string host = hostport;
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        try {
            port = static_cast<INTERNET_PORT>(std::stoi(hostport.substr(colon + 1)));
        } catch (...) {
        }
    }
    HINTERNET sess = WinHttpOpen(L"PRISM", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) return std::nullopt;
    WinHttpSetTimeouts(sess, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    auto whost = wide_utf8(host);
    HINTERNET conn = WinHttpConnect(sess, whost.c_str(), port, 0);
    if (!conn) {
        WinHttpCloseHandle(sess);
        return std::nullopt;
    }
    auto wpath = wide_utf8(path);
    auto wmethod = wide_utf8(method);
    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, wmethod.c_str(), wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(sess);
        return std::nullopt;
    }
    LPCWSTR hdrs = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : L"Content-Type: application/json\r\n";
    BOOL ok = WinHttpSendRequest(req, hdrs, body.empty() ? 0 : static_cast<DWORD>(-1L),
                                 body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                 static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!ok || !WinHttpReceiveResponse(req, nullptr)) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(sess);
        return std::nullopt;
    }
    DWORD status = 0, slen = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &slen, WINHTTP_NO_HEADER_INDEX);
    std::string resp;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) break;
        resp.append(chunk.data(), got);
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(sess);
    if (status < 200 || status >= 300) return std::nullopt;
    return resp;
#else
    if (url.starts_with("https://")) return std::nullopt;
    if (!url.starts_with("http://") || method.empty()) return std::nullopt;
    const std::string rest = url.substr(7);
    if (rest.empty()) return std::nullopt;
    const auto slash = rest.find('/');
    const std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    const std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (hostport.empty() || path.empty() || path.front() != '/') return std::nullopt;
    int port = 80;
    std::string host = hostport;
    const auto colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        if (colon == 0) return std::nullopt;
        host = hostport.substr(0, colon);
        const std::string ps = hostport.substr(colon + 1);
        if (ps.empty()) return std::nullopt;
        for (unsigned char c : ps)
            if (!std::isdigit(c)) return std::nullopt;
        try {
            port = std::stoi(ps);
        } catch (...) {
            return std::nullopt;
        }
        if (port < 1 || port > 65535) return std::nullopt;
    }
    if (host.empty()) return std::nullopt;
    auto has_ctl_or_space = [](const std::string& s, bool spaces) {
        for (unsigned char c : s) {
            if (c < 32 || c == 127) return true;
            if (spaces && c == ' ') return true;
        }
        return false;
    };
    if (has_ctl_or_space(host, true) || has_ctl_or_space(path, false) || has_ctl_or_space(method, true))
        return std::nullopt;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
    auto remain_ms = [&]() -> int {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return 0;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        return ms > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(ms);
    };
    auto poll_wait = [&](int sfd, short events) -> bool {
        for (;;) {
            const int ms = remain_ms();
            if (ms <= 0 && std::chrono::steady_clock::now() >= deadline) return false;
            pollfd pfd{};
            pfd.fd = sfd;
            pfd.events = events;
            const int pr = ::poll(&pfd, 1, ms);
            if (pr == 0) return false;
            if (pr < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (pfd.revents & POLLNVAL) return false;
            return true;
        }
    };
    auto try_connect = [&](const sockaddr* sa, socklen_t slen) -> int {
        const int sfd = ::socket(sa->sa_family, SOCK_STREAM, IPPROTO_TCP);
        if (sfd < 0) return -1;
        const int fl = ::fcntl(sfd, F_GETFL, 0);
        if (fl >= 0) ::fcntl(sfd, F_SETFL, fl | O_NONBLOCK);
        const int rc = ::connect(sfd, sa, slen);
        if (rc != 0 && errno != EINPROGRESS && errno != EINTR) {
            ::close(sfd);
            return -1;
        }
        if (rc != 0) {
            if (!poll_wait(sfd, POLLOUT)) {
                ::close(sfd);
                return -1;
            }
            int err = 0;
            socklen_t elen = sizeof err;
            if (::getsockopt(sfd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
                ::close(sfd);
                return -1;
            }
        }
        return sfd;
    };

    int fd = -1;
    sockaddr_in in4{};
    in4.sin_family = AF_INET;
    in4.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &in4.sin_addr) == 1) {
        fd = try_connect(reinterpret_cast<sockaddr*>(&in4), sizeof in4);
    } else {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* res = nullptr;
        const std::string port_s = std::to_string(port);
        if (::getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) == 0 && res) {
            for (addrinfo* ai = res; ai != nullptr && fd < 0; ai = ai->ai_next)
                fd = try_connect(ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
            ::freeaddrinfo(res);
        }
    }
    if (fd < 0) return std::nullopt;
    struct FdGuard {
        int fd;
        explicit FdGuard(int f) : fd(f) {}
        ~FdGuard() {
            if (fd >= 0) ::close(fd);
        }
        FdGuard(const FdGuard&) = delete;
        FdGuard& operator=(const FdGuard&) = delete;
    } guard{fd};

#ifdef MSG_NOSIGNAL
    const int sflags = MSG_NOSIGNAL;
#else
    const int sflags = 0;
#endif
    std::string req = method;
    req += ' ';
    req += path;
    req += " HTTP/1.1\r\nHost: ";
    req += host;
    if (port != 80) {
        req += ':';
        req += std::to_string(port);
    }
    req += "\r\n";
    if (!body.empty()) {
        req += "Content-Type: application/json\r\nContent-Length: ";
        req += std::to_string(body.size());
        req += "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    req += body;

    std::size_t off = 0;
    while (off < req.size()) {
        if (!poll_wait(fd, POLLOUT)) return std::nullopt;
        const ssize_t n = ::send(fd, req.data() + off, req.size() - off, sflags);
        if (n > 0) {
            off += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return std::nullopt;
    }

    std::string raw;
    char rbuf[4096];
    auto read_some = [&]() -> int {
        if (!poll_wait(fd, POLLIN)) return -1;
        const ssize_t n = ::recv(fd, rbuf, sizeof rbuf, 0);
        if (n > 0) {
            raw.append(rbuf, static_cast<std::size_t>(n));
            return 1;
        }
        if (n == 0) return 0;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 1;
        return -1;
    };
    for (;;) {
        if (raw.find("\r\n\r\n") != std::string::npos) break;
        if (raw.size() > 65536) return std::nullopt;
        if (read_some() <= 0) return std::nullopt;
    }
    const auto hdr_end = raw.find("\r\n\r\n");
    const std::string headers = raw.substr(0, hdr_end);
    const auto crlf = headers.find("\r\n");
    const std::string status_line = crlf == std::string::npos ? headers : headers.substr(0, crlf);
    const auto sp = status_line.find(' ');
    if (sp == std::string::npos) return std::nullopt;
    int status = 0;
    try {
        status = std::stoi(status_line.substr(sp + 1));
    } catch (...) {
        return std::nullopt;
    }
    if (status < 200 || status >= 300) return std::nullopt;

    std::string hlow = headers;
    for (char& c : hlow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool has_len = false;
    std::size_t content_length = 0;
    const std::string cl_key = "\r\ncontent-length:";
    auto clp = hlow.find(cl_key);
    if (clp != std::string::npos) {
        clp += cl_key.size();
        while (clp < hlow.size() && (hlow[clp] == ' ' || hlow[clp] == '\t')) ++clp;
        auto cle = hlow.find("\r\n", clp);
        if (cle == std::string::npos) cle = hlow.size();
        const std::string cl = hlow.substr(clp, cle - clp);
        if (cl.empty()) return std::nullopt;
        for (unsigned char c : cl)
            if (!std::isdigit(c)) return std::nullopt;
        try {
            content_length = static_cast<std::size_t>(std::stoull(cl));
            has_len = true;
        } catch (...) {
            return std::nullopt;
        }
    }

    const std::size_t body_off = hdr_end + 4;
    constexpr std::size_t kMaxBody = 32u * 1024u * 1024u;
    if (has_len) {
        if (content_length > kMaxBody) return std::nullopt;
        while (raw.size() < body_off + content_length) {
            if (read_some() <= 0) return std::nullopt;
        }
        return raw.substr(body_off, content_length);
    }
    for (;;) {
        const int g = read_some();
        if (g < 0) return std::nullopt;
        if (g == 0) break;
        if (raw.size() > kMaxBody) return std::nullopt;
    }
    return raw.substr(body_off);
#endif
}
bool http_ok(const std::string& url, int timeout_ms = 1500) {
    return http_request("GET", url, {}, timeout_ms).has_value();
}

std::optional<std::string> unencoded_layout_prefix(std::string_view text);
std::optional<std::string> unencoded_layout_stmt(std::string_view stmt_s);
#include "bmc_unenc.inc"

std::optional<std::string> unencoded_layout_prefix(std::string_view text) {
    auto s = lstrip(std::string(text));
    static Regex su(R"((?:struct|union)\s*(?:[A-Za-z_]\w*\s*)?\{)");
    static Regex en(R"(enum\s*\{)");
    static Regex se(R"((?:static|extern)\b)");
    static Regex al(R"((?:_Alignas|alignas)\s*\()");
    static Regex at(R"(__auto_type\b)");
    static Regex tl(R"((?:_Thread_local|thread_local)\b)");
    static Regex cx(R"((?:_Complex|_Imaginary)\b)");
    static Regex cl(
        R"((?:int|unsigned(?:\\s+int)?|long|short|char|uint32_t|int32_t|size_t)\\s+\\w+\\s*=\\s*\\([^)]*\\)\\s*\\{)");
    if (match_at(su, s)) return "struct unencoded";
    if (match_at(en, s)) return "anon enum unencoded";
    if (match_at(se, s)) return "storage-duration unencoded";
    if (match_at(al, s)) return "alignas unencoded";
    if (match_at(at, s)) return "storage-class unencoded";
    if (match_at(tl, s)) return "thread-local unencoded";
    if (match_at(cx, s)) return "complex unencoded";
    if (match_at(cl, s)) return "compound-lit unencoded";
    return std::nullopt;
}
std::optional<std::string> unencoded_layout_stmt(std::string_view stmt_s) {
    auto s = strip(std::string(stmt_s));
    if (s.empty()) return std::nullopt;
    if (re_search(R"(\b(?:__int128(?:_t)?|_BitInt)\b)", s)) return "128-bit unencoded";
    if (starts_kw(s, "constexpr")) return "constexpr unencoded";
    if (starts_kw(s, "const")) return "const unencoded";
    if (starts_kw(s, "register") || starts_kw(s, "auto")) return "storage-class unencoded";
    if (starts_kw(s, "static") || starts_kw(s, "extern")) return "storage-duration unencoded";
    if (starts_kw(s, "struct") || starts_kw(s, "union")) return "struct unencoded";
    if (is_nested_function(s)) return "nested function unencoded";
    static Regex td(R"(([A-Za-z_]\w*)\s+[A-Za-z_]\w*\s*(?:[=;\[]|$))");
    auto m = match_at(td, s);
    if (!m) return std::nullopt;
    if (kStmtStartWords.contains(m->group(1))) return std::nullopt;
    return "typedef local unencoded";
}

std::map<std::string, int> extract_enums(std::string text) {
    text = strip_comments_keep_lines(text);
    std::map<std::string, int> out;
    static Regex re("\\benum\\b(?:\\s+[A-Za-z_]\\w*)?\\s*\\{([^{}]*)\\}");
    for (auto& m : re.finditer(text)) {
        int nxt = 0;
        for (auto part0 : split_comma(m.group(1))) {
            auto part = strip(part0);
            if (part.empty()) continue;
            auto eq = part.find('=');
            if (eq != std::string::npos) {
                auto name = strip(part.substr(0, eq));
                auto val = strip(part.substr(eq + 1));
                while (!val.empty() && (val.back() == 'u' || val.back() == 'U' || val.back() == 'l' ||
                                        val.back() == 'L'))
                    val.pop_back();
                if (!is_ident(name)) continue;
                try {
                    nxt = std::stoi(val, nullptr, 0);
                } catch (...) {
                    auto it = out.find(val);
                    if (it == out.end()) continue;
                    nxt = it->second;
                }
                out[name] = nxt++;
            } else if (is_ident(part)) {
                out[part] = nxt++;
            }
        }
    }
    return out;
}
std::map<std::string, int> enums_for(const FunctionInfo& fn) {
    auto p = locate_source(fn);
    if (!p) return {};
    try {
        return extract_enums(read_text_file(*p));
    } catch (...) {
        return {};
    }
}
bool has_self_call(const FunctionInfo& fn) {
    if (fn.name.empty()) return false;
    static Regex re("\\b([A-Za-z_]\\w*)\\s*\\(");
    for (auto& m : re.finditer(fn.body)) {
        auto n = m.group(1);
        if (!kCallKw.contains(n) && n == fn.name) return true;
    }
    return false;
}
bool has_unencoded_cxx(const FunctionInfo& fn) {
    return re_search("\\bstd::|\\bstring_view\\b|\\bspan\\b", fn.return_type + " " + fn.body);
}
bool has_unencoded_float(const FunctionInfo& fn) {
    static Regex fl("(?i)\\bfloat\\b|\\bdouble\\b");
    if (fl.search(fn.return_type)) return true;
    for (auto& [t, n] : fn.params)
        if (fl.search(t)) return true;
    return re_search("\\d+\\.\\d+[fFlL]?|\\b(?:float|double)\\b", fn.body);
}
bool has_unencoded_throw(const FunctionInfo& fn) { return re_search("\\bthrow\\b", fn.body); }
bool has_unencoded_setjmp(const FunctionInfo& fn) {
    return re_search("\\b(?:setjmp|longjmp|va_list|va_start)\\b", fn.body);
}

int sizeof_concrete(const std::vector<std::string>& inner, const std::map<std::string, std::vector<int64_t>>& arrays) {
    if (inner.empty()) return WIDTH / 8;
    for (auto& t : inner)
        if (t == "*") return WIDTH / 8;
    std::string joined = join_sv(inner, " ");
    auto it = kTypeSize.find(joined);
    if (it != kTypeSize.end()) return it->second;
    if (inner.size() == 1 && is_ident(inner[0])) {
        auto a = arrays.find(inner[0]);
        if (a != arrays.end()) return static_cast<int>(a->second.size()) * (WIDTH / 8);
        return WIDTH / 8;
    }
    return WIDTH / 8;
}

struct St {
    std::map<std::string, int64_t> vars;
    std::map<std::string, std::vector<int64_t>> arrays;
    std::map<std::string, int> enums;
    std::unordered_set<std::string> uns;
    std::map<std::string, int> bits;
    int steps = 0;
    St(const std::vector<std::pair<std::string, std::string>>& params, const std::map<std::string, int>& args,
       std::map<std::string, int> en)
        : enums(std::move(en)) {
        for (auto& [typ, name] : params) {
            if (name.empty()) continue;
            if (type_is_unsigned(typ)) uns.insert(name);
            int w = type_width(typ);
            bits[name] = w;
            int raw = 0;
            auto it = args.find(name);
            if (it != args.end()) raw = it->second;
            vars[name] = w <= 32 ? i32(raw) : raw;
        }
    }
    void tick() {
        if (++steps > MAX_STEPS) throw ReturnEx(vars.contains("__ret") ? std::optional<int64_t>(vars["__ret"])
                                                                       : std::nullopt);
    }
};

enum class Nk { Num, Id, Idx, Un, Pre, Post, Str, Call, Tern, Comma, Bin };
struct Node {
    Nk k{};
    std::string a;
    int64_t n = 0;
    std::string s;
    std::vector<Node> ch;
};

int64_t binop(int64_t a, const std::string& op, int64_t b, bool uns, int width);
int64_t eval_tree(const Node& t, St& st);
bool tree_unsigned(const Node& t, const St& st);
int tree_width(const Node& t, const St& st);

struct EParser {
    St& st;
    std::vector<std::string> tokens;
    std::size_t pos = 0;
    const std::unordered_map<std::string, int> prec{
        {"||", 10}, {"&&", 20}, {"|", 30},  {"^", 40},  {"&", 50},  {"==", 60}, {"!=", 60},
        {"<", 70},  {">", 70},  {"<=", 70}, {">=", 70}, {"<<", 80}, {">>", 80}, {"+", 90},
        {"-", 90},  {"*", 100}, {"/", 100}, {"%", 100},
    };
    std::string peek() const { return pos < tokens.size() ? tokens[pos] : std::string{}; }
    std::string eat(const std::string* expect = nullptr) {
        if (pos >= tokens.size()) throw ParseFail("unexpected end");
        auto got = tokens[pos];
        if (expect && got != *expect) throw ParseFail("expected " + *expect + " got " + got);
        ++pos;
        return got;
    }
    std::string eat_s(std::string t) { return eat(&t); }
    Node nud() {
        auto t = eat();
        if (t == "sizeof") {
            if (peek() == "(") {
                eat_s("(");
                std::vector<std::string> inner;
                int depth = 1;
                while (depth) {
                    auto ntok = eat();
                    if (ntok == "(") {
                        ++depth;
                        inner.push_back(ntok);
                    } else if (ntok == ")") {
                        --depth;
                        if (depth) inner.push_back(ntok);
                    } else {
                        inner.push_back(ntok);
                    }
                }
                return Node{Nk::Num, {}, sizeof_concrete(inner, st.arrays)};
            }
            auto name = eat();
            return Node{Nk::Num, {}, sizeof_concrete({name}, st.arrays)};
        }
        if (t == "(") {
            if (peek() == "{") throw ParseFail("statement-expr unencoded");
            if (kCastWords.contains(peek())) {
                while (!peek().empty() && peek() != ")") {
                    if (!kCastWords.contains(peek()) && peek() != "*") break;
                    eat();
                }
                eat_s(")");
                return parse(110);
            }
            auto v = parse(0);
            eat_s(")");
            return v;
        }
        if (t == "-" || t == "!" || t == "~") {
            Node n{Nk::Un, t};
            n.ch.push_back(parse(110));
            return n;
        }
        if (t == "++" || t == "--") {
            auto name = eat();
            if (!is_ident(name)) throw ParseFail("bad token " + name);
            return Node{Nk::Pre, t, 0, name};
        }
        if (t.size() >= 3 && t.front() == '\'' && t.back() == '\'')
            return Node{Nk::Num, {}, char_lit_value(t)};
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
            return Node{Nk::Str, {}, 0, string_lit_value(t)};
        if (std::isdigit(static_cast<unsigned char>(t[0])) || t.starts_with("0x") || t.starts_with("0X")) {
            try {
                return Node{Nk::Num, {}, std::stoll(t, nullptr, 0)};
            } catch (...) {
                throw ParseFail("bad num " + t);
            }
        }
        if (is_ident(t)) {
            if (t == "_Generic" || t == "offsetof") throw ParseFail(t + " unencoded");
            if (peek() == "[") {
                eat_s("[");
                auto idx = parse(0);
                eat_s("]");
                Node n{Nk::Idx, t};
                n.ch.push_back(std::move(idx));
                return n;
            }
            if (peek() == "(") {
                eat_s("(");
                Node n{Nk::Call, t};
                if (peek() != ")") {
                    n.ch.push_back(parse(2));
                    while (peek() == ",") {
                        eat_s(",");
                        n.ch.push_back(parse(2));
                    }
                }
                eat_s(")");
                return n;
            }
            if (peek() == "++" || peek() == "--") {
                auto op = eat();
                return Node{Nk::Post, op, 0, t};
            }
            return Node{Nk::Id, t};
        }
        throw ParseFail("bad token " + t);
    }
    Node parse(int minp) {
        auto left = nud();
        while (prec.contains(peek()) && prec.at(peek()) >= minp) {
            auto op = eat();
            auto right = parse(prec.at(op) + 1);
            Node n{Nk::Bin, op};
            n.ch.push_back(std::move(left));
            n.ch.push_back(std::move(right));
            left = std::move(n);
        }
        if (minp <= 5 && peek() == "?") {
            eat_s("?");
            auto th = parse(0);
            eat_s(":");
            auto el = parse(5);
            Node n{Nk::Tern};
            n.ch.push_back(std::move(left));
            n.ch.push_back(std::move(th));
            n.ch.push_back(std::move(el));
            left = std::move(n);
        }
        if (minp <= 1 && peek() == ",") {
            eat_s(",");
            auto right = parse(0);
            Node n{Nk::Comma};
            n.ch.push_back(std::move(left));
            n.ch.push_back(std::move(right));
            left = std::move(n);
        }
        return left;
    }
};

bool tree_unsigned(const Node& t, const St& st) {
    if (t.k == Nk::Id) return st.uns.contains(t.a);
    if (t.k == Nk::Bin) return tree_unsigned(t.ch[0], st) || tree_unsigned(t.ch[1], st);
    if (t.k == Nk::Un) return tree_unsigned(t.ch[0], st);
    if (t.k == Nk::Tern) return tree_unsigned(t.ch[1], st) || tree_unsigned(t.ch[2], st);
    if (t.k == Nk::Comma) return tree_unsigned(t.ch[1], st);
    return false;
}
int tree_width(const Node& t, const St& st) {
    if (t.k == Nk::Id) {
        auto it = st.bits.find(t.a);
        return it == st.bits.end() ? WIDTH : it->second;
    }
    if (t.k == Nk::Post || t.k == Nk::Pre) {
        auto it = st.bits.find(t.s);
        return it == st.bits.end() ? WIDTH : it->second;
    }
    if (t.k == Nk::Bin) return std::max(tree_width(t.ch[0], st), tree_width(t.ch[1], st));
    if (t.k == Nk::Un) return tree_width(t.ch[0], st);
    if (t.k == Nk::Tern) return std::max(tree_width(t.ch[1], st), tree_width(t.ch[2], st));
    if (t.k == Nk::Comma) return tree_width(t.ch[1], st);
    return WIDTH;
}

int64_t binop(int64_t a, const std::string& op, int64_t b, bool uns, int width) {
    if (uns) {
        uint32_t ua = u32(a), ub = u32(b);
        if (op == "+") return i32(static_cast<int64_t>(ua) + ub);
        if (op == "-") return i32(static_cast<int64_t>(ua) - ub);
        if (op == "*") return i32(static_cast<int64_t>(ua) * ub);
        if (op == "/") {
            if (ub == 0) throw UB("INT-DIV-ZERO");
            return i32(ua / ub);
        }
        if (op == "%") {
            if (ub == 0) throw UB("INT-DIV-ZERO");
            return i32(ua % ub);
        }
        if (op == "<<") {
            if (ub >= static_cast<uint32_t>(WIDTH)) throw UB("INT-SHIFT-UB");
            return i32(static_cast<int64_t>(ua) << ub);
        }
        if (op == ">>") {
            if (ub >= static_cast<uint32_t>(WIDTH)) throw UB("INT-SHIFT-UB");
            return i32(ua >> ub);
        }
        if (op == "&") return i32(ua & ub);
        if (op == "|") return i32(ua | ub);
        if (op == "^") return i32(ua ^ ub);
        if (op == "==") return ua == ub;
        if (op == "!=") return ua != ub;
        if (op == "<") return ua < ub;
        if (op == ">") return ua > ub;
        if (op == "<=") return ua <= ub;
        if (op == ">=") return ua >= ub;
        throw ParseFail("op " + op);
    }
    if (width >= 64) {
        if (op == "+") {
            auto r = a + b;
            if (r < INT64_MIN_V || r > INT64_MAX_V) throw UB("INT-SIGNED-OVF");
            return r;
        }
        if (op == "-") {
            auto r = a - b;
            if (r < INT64_MIN_V || r > INT64_MAX_V) throw UB("INT-SIGNED-OVF");
            return r;
        }
        if (op == "*") {
            auto r = a * b;
            if (r < INT64_MIN_V || r > INT64_MAX_V) throw UB("INT-SIGNED-OVF");
            return r;
        }
        if (op == "/") {
            if (b == 0) throw UB("INT-DIV-ZERO");
            if (a == INT64_MIN_V && b == -1) throw UB("INT-SIGNED-OVF");
            return a / b;
        }
        if (op == "%") {
            if (b == 0) throw UB("INT-DIV-ZERO");
            return a - (a / b) * b;
        }
        if (op == "<<") {
            if (b < 0 || b >= 64) throw UB("INT-SHIFT-UB");
            if (a == 1 && b >= 63) throw UB("INT-SHIFT-UB");
            return a << b;
        }
        if (op == ">>") {
            if (b < 0 || b >= 64) throw UB("INT-SHIFT-UB");
            return a >> b;
        }
        if (op == "&") return a & b;
        if (op == "|") return a | b;
        if (op == "^") return a ^ b;
        if (op == "==") return a == b;
        if (op == "!=") return a != b;
        if (op == "<") return a < b;
        if (op == ">") return a > b;
        if (op == "<=") return a <= b;
        if (op == ">=") return a >= b;
        throw ParseFail("op " + op);
    }
    a = i32(a);
    b = i32(b);
    if (op == "+") {
        auto r = a + b;
        if (r < INT_MIN_32 || r > INT_MAX_32) throw UB("INT-SIGNED-OVF");
        return r;
    }
    if (op == "-") {
        auto r = a - b;
        if (r < INT_MIN_32 || r > INT_MAX_32) throw UB("INT-SIGNED-OVF");
        return r;
    }
    if (op == "*") {
        auto r = a * b;
        if (r < INT_MIN_32 || r > INT_MAX_32) throw UB("INT-SIGNED-OVF");
        return r;
    }
    if (op == "/") {
        if (b == 0) throw UB("INT-DIV-ZERO");
        if (a == INT_MIN_32 && b == -1) throw UB("INT-SIGNED-OVF");
        return a / b;
    }
    if (op == "%") {
        if (b == 0) throw UB("INT-DIV-ZERO");
        return a - (a / b) * b;
    }
    if (op == "<<") {
        if (b < 0 || b >= WIDTH) throw UB("INT-SHIFT-UB");
        if (a == 1 && b >= WIDTH - 1) throw UB("INT-SHIFT-UB");
        return i32(a << b);
    }
    if (op == ">>") {
        if (b < 0 || b >= WIDTH) throw UB("INT-SHIFT-UB");
        return i32(a >> b);
    }
    if (op == "&") return i32(a & b);
    if (op == "|") return i32(a | b);
    if (op == "^") return i32(a ^ b);
    if (op == "==") return a == b;
    if (op == "!=") return a != b;
    if (op == "<") return a < b;
    if (op == ">") return a > b;
    if (op == "<=") return a <= b;
    if (op == ">=") return a >= b;
    throw ParseFail("op " + op);
}

void eval_cstr_copy(St& st, const Node& dest, const Node& src, std::optional<int> n, bool cat) {
    if (dest.k != Nk::Id) {
        eval_tree(dest, st);
        eval_tree(src, st);
        return;
    }
    auto it = st.arrays.find(dest.a);
    if (it == st.arrays.end()) {
        eval_tree(src, st);
        return;
    }
    if (src.k != Nk::Str) {
        eval_tree(src, st);
        return;
    }
    auto& arr = it->second;
    std::size_t start = 0;
    if (cat) {
        while (start < arr.size() && arr[start]) ++start;
        if (start >= arr.size()) throw UB("MEM-OOB-WRITE");
    }
    std::vector<int64_t> payload;
    if (!n) {
        for (unsigned char c : src.s) payload.push_back(c);
        payload.push_back(0);
    } else {
        int nn = std::max(0, *n);
        std::string chars = src.s.substr(0, static_cast<std::size_t>(nn));
        for (unsigned char c : chars) payload.push_back(c);
        while (static_cast<int>(payload.size()) < nn) payload.push_back(0);
    }
    for (std::size_t i = 0; i < payload.size(); ++i) {
        auto idx = start + i;
        if (idx >= arr.size()) throw UB("MEM-OOB-WRITE");
        arr[idx] = payload[i];
    }
}

int64_t eval_tree(const Node& t, St& st) {
    switch (t.k) {
    case Nk::Num: return i32(t.n);
    case Nk::Id: {
        auto it = st.vars.find(t.a);
        if (it != st.vars.end()) {
            int w = st.bits.contains(t.a) ? st.bits[t.a] : WIDTH;
            return w <= 32 ? i32(it->second) : it->second;
        }
        if (st.enums.contains(t.a)) return i32(st.enums[t.a]);
        st.vars[t.a] = 0;
        return 0;
    }
    case Nk::Idx: {
        auto it = st.arrays.find(t.a);
        if (it == st.arrays.end()) throw ParseFail("unknown array " + t.a);
        auto idx = eval_tree(t.ch[0], st);
        auto& arr = it->second;
        if (tree_unsigned(t.ch[0], st)) {
            auto ui = u32(idx);
            if (ui >= arr.size()) throw UB("MEM-OOB-READ");
            return i32(arr[ui]);
        }
        if (idx < 0 || static_cast<std::size_t>(idx) >= arr.size()) throw UB("MEM-OOB-READ");
        return i32(arr[static_cast<std::size_t>(idx)]);
    }
    case Nk::Un: {
        auto v = eval_tree(t.ch[0], st);
        if (t.a == "-") {
            if (!tree_unsigned(t.ch[0], st) && v == INT_MIN_32) throw UB("INT-SIGNED-OVF");
            return i32(-v);
        }
        if (t.a == "!") return truth(v) ? 0 : 1;
        if (t.a == "~") return i32(~(v & 0xFFFFFFFF));
        throw ParseFail("unop " + t.a);
    }
    case Nk::Post:
    case Nk::Pre: {
        auto cur = st.vars.contains(t.s) ? st.vars[t.s] : 0;
        int w = st.bits.contains(t.s) ? st.bits[t.s] : WIDTH;
        bool u = st.uns.contains(t.s);
        auto nw = binop(cur, t.a == "++" ? "+" : "-", 1, u, w);
        st.vars[t.s] = nw;
        auto v = t.k == Nk::Post ? cur : nw;
        return w <= 32 ? i32(v) : v;
    }
    case Nk::Str: return 1;
    case Nk::Call: {
        if ((t.a == "strcpy" || t.a == "strcat") && t.ch.size() >= 2) {
            eval_cstr_copy(st, t.ch[0], t.ch[1], std::nullopt, t.a == "strcat");
            return 0;
        }
        if (t.a == "strncpy" && t.ch.size() >= 3) {
            auto n = eval_tree(t.ch[2], st);
            eval_cstr_copy(st, t.ch[0], t.ch[1], static_cast<int>(std::max<int64_t>(0, n)), false);
            return 0;
        }
        for (auto& a : t.ch) eval_tree(a, st);
        return 0;
    }
    case Nk::Tern:
        return truth(eval_tree(t.ch[0], st)) ? eval_tree(t.ch[1], st) : eval_tree(t.ch[2], st);
    case Nk::Comma:
        eval_tree(t.ch[0], st);
        return eval_tree(t.ch[1], st);
    case Nk::Bin: {
        if (t.a == "&&") {
            if (!truth(eval_tree(t.ch[0], st))) return 0;
            return truth(eval_tree(t.ch[1], st)) ? 1 : 0;
        }
        if (t.a == "||") {
            if (truth(eval_tree(t.ch[0], st))) return 1;
            return truth(eval_tree(t.ch[1], st)) ? 1 : 0;
        }
        auto a = eval_tree(t.ch[0], st);
        auto b = eval_tree(t.ch[1], st);
        bool u = tree_unsigned(t.ch[0], st) || tree_unsigned(t.ch[1], st);
        int w = std::max(tree_width(t.ch[0], st), tree_width(t.ch[1], st));
        return binop(a, t.a, b, u, w);
    }
    }
    throw ParseFail("bad tree");
}

int64_t eval_src(St& st, const std::string& src) {
    EParser p{st, tok(strip(src)), 0};
    auto tree = p.parse(0);
    if (p.pos != p.tokens.size()) throw ParseFail("trailing tokens");
    auto v = eval_tree(tree, st);
    return tree_width(tree, st) >= 64 ? v : i32(v);
}

std::pair<std::string, std::string> consume_stmt_src(std::string raw);

struct CParser {
    std::string body;
    St& st;
    CParser(std::string b, St& s) : body(std::move(b)), st(s) {}
    void run() { stmts(prep(body)); }
    static std::string prep(std::string b) {
        static Regex re("#.*");
        std::string out;
        std::size_t i = 0;
        for (auto& m : re.finditer(b)) {
            if (m.spans.empty()) continue;
            auto a = static_cast<std::size_t>(std::max(0, m.spans[0].first));
            auto e = static_cast<std::size_t>(std::max(0, m.spans[0].second));
            if (a < i) continue;
            out.append(b, i, a - i);
            out += ' ';
            i = e;
        }
        out.append(b, i, std::string::npos);
        return out;
    }
    void stmts(std::string text);
    void decl(std::string s);
    void assign_or_expr(std::string s);
    void astore(const std::string& name, const std::string& idx, const std::string& rhs);
    std::string do_if(const std::string& text);
    std::string do_while(const std::string& text);
    std::string do_do(const std::string& text);
    std::string do_for(const std::string& text);
    std::string do_switch(const std::string& text);
    std::string do_assert(const std::string& text);
    bool run_loop_body(const std::string& b) {
        try {
            stmts(b);
        } catch (const BreakEx&) {
            return false;
        } catch (const ContinueEx&) {
        }
        return true;
    }
};

void CParser::decl(std::string s) {
    s = strip(s);
    while (!s.empty() && s.back() == ';') s.pop_back();
    s = strip(s);
    static Regex re(
        "(?:int|unsigned(?:\\s+int)?|long|short|char|uint32_t|int32_t|size_t)"
        "\\s+([A-Za-z_]\\w*)(?:\\s*\\[(\\d+)\\])?(?:\\s*=\\s*(.*))?$");
    auto m = match_at(re, s);
    if (!m) {
        if (re_search("\\[[^\\]]+\\]", s)) throw ParseFail("VLA unencoded");
        throw ParseFail("unparsed decl: " + s.substr(0, 80));
    }
    auto name = m->group(1);
    auto dim = m->group(2);
    auto init = m->group(3);
    if (!dim.empty()) {
        st.arrays[name] = std::vector<int64_t>(static_cast<std::size_t>(std::stoi(dim)), 0);
        return;
    }
    auto start1 = m->spans.size() > 1 ? m->spans[1].first : 0;
    if (type_is_unsigned(s.substr(0, static_cast<std::size_t>(std::max(0, start1))))) st.uns.insert(name);
    st.vars[name] = init.empty() ? 0 : i32(eval_src(st, init));
}

void CParser::astore(const std::string& name, const std::string& idx, const std::string& rhs) {
    auto it = st.arrays.find(name);
    if (it == st.arrays.end()) throw ParseFail("unknown array " + name);
    auto i = eval_src(st, idx);
    auto v = eval_src(st, rhs);
    bool uidx = is_ident(strip(idx)) && st.uns.contains(strip(idx));
    if (uidx) {
        auto ui = u32(i);
        if (ui >= it->second.size()) throw UB("MEM-OOB-WRITE");
        it->second[ui] = i32(v);
        return;
    }
    if (i < 0 || static_cast<std::size_t>(i) >= it->second.size()) throw UB("MEM-OOB-WRITE");
    it->second[static_cast<std::size_t>(i)] = i32(v);
}

void CParser::assign_or_expr(std::string s) {
    s = strip(s);
    while (!s.empty() && s.back() == ';') s.pop_back();
    s = strip(s);
    if (s.empty()) return;
    auto parts = split_comma(s);
    if (parts.size() > 1) {
        for (auto& part : parts) {
            auto piece = strip(part);
            if (!piece.empty()) assign_or_expr(piece);
        }
        return;
    }
    static Regex arr("([A-Za-z_]\\w*)\\s*\\[(.+)\\]\\s*=\\s*(.+)$");
    if (auto m = match_at(arr, s)) {
        astore(m->group(1), m->group(2), m->group(3));
        return;
    }
    static Regex asg("([A-Za-z_]\\w*)\\s*([+\\-*/%|&^]?=)\\s*(.+)$");
    if (auto m = match_at(asg, s)) {
        auto name = m->group(1);
        auto op = m->group(2);
        auto val = eval_src(st, m->group(3));
        int w = st.bits.contains(name) ? st.bits[name] : WIDTH;
        if (op == "=") {
            st.vars[name] = w <= 32 ? i32(val) : val;
        } else {
            auto cur = st.vars.contains(name) ? st.vars[name] : 0;
            auto r = binop(cur, op.substr(0, 1), val, st.uns.contains(name), w);
            st.vars[name] = w <= 32 ? i32(r) : r;
        }
        return;
    }
    eval_src(st, s);
}

std::string CParser::do_assert(const std::string& text) {
    static Regex re("assert\\s*\\((.*)\\)\\s*;", false, true);
    if (auto m = match_at(re, text)) {
        if (!truth(eval_src(st, m->group(1)))) throw UB("FUNC-CONTRACT");
        return text.substr(static_cast<std::size_t>(m->spans[0].second));
    }
    auto lp = text.find('(');
    if (lp == std::string::npos) throw ParseFail("assert");
    auto [inner, rest] = paren(text.substr(lp));
    rest = lstrip(rest);
    if (rest.starts_with(";")) rest = rest.substr(1);
    if (!truth(eval_src(st, inner))) throw UB("FUNC-CONTRACT");
    return rest;
}

std::string CParser::do_if(const std::string& text) {
    auto rest = lstrip(text.substr(2));
    auto [cond, after] = paren(rest);
    std::string then_src;
    std::tie(then_src, after) = take_block(after);
    std::optional<std::string> else_src;
    auto stripped = lstrip(after);
    if (starts_kw(stripped, "else")) {
        auto [es, af] = take_block(stripped.substr(4));
        else_src = std::move(es);
        after = std::move(af);
    }
    if (truth(eval_src(st, cond))) stmts(then_src);
    else if (else_src) stmts(*else_src);
    return after;
}
std::string CParser::do_while(const std::string& text) {
    auto rest = lstrip(text.substr(5));
    auto [cond, after] = paren(rest);
    std::string body;
    std::tie(body, after) = take_block(after);
    while (truth(eval_src(st, cond))) {
        st.tick();
        if (!run_loop_body(body)) break;
    }
    return after;
}
std::string CParser::do_do(const std::string& text) {
    auto rest = lstrip(text.substr(2));
    auto [body, after] = take_block(rest);
    after = lstrip(after);
    if (!starts_kw(after, "while")) throw ParseFail("do without while");
    after = lstrip(after.substr(5));
    std::string cond;
    std::tie(cond, after) = paren(after);
    after = lstrip(after);
    if (after.starts_with(";")) after = after.substr(1);
    if (!run_loop_body(body)) return after;
    while (truth(eval_src(st, cond))) {
        st.tick();
        if (!run_loop_body(body)) break;
    }
    return after;
}
std::string CParser::do_for(const std::string& text) {
    auto rest = lstrip(text.substr(3));
    auto [head, after] = paren(rest);
    auto parts = split_semi(head);
    while (parts.size() < 3) parts.emplace_back();
    auto init = strip(parts[0]), cond = strip(parts[1]), incr = strip(parts[2]);
    if (!init.empty()) {
        auto init_stmt = init.ends_with(";") ? init : init + ";";
        if (auto miss = unencoded_layout_stmt(init_stmt)) throw ParseFail(*miss);
        assign_or_expr(init_stmt);
    }
    std::string body;
    std::tie(body, after) = take_block(after);
    if (cond.empty()) cond = "1";
    while (truth(eval_src(st, cond))) {
        st.tick();
        if (!run_loop_body(body)) break;
        if (!incr.empty()) assign_or_expr(incr.ends_with(";") ? incr : incr + ";");
    }
    return after;
}

struct SwitchArm {
    std::vector<std::optional<int64_t>> labels;
    std::string code;
    bool stops = false;
};

std::vector<SwitchArm> parse_switch_arms(CParser& p, std::string text) {
    std::vector<SwitchArm> arms;
    std::vector<std::optional<int64_t>> labels;
    std::vector<std::string> chunks;
    bool stops = false;
    auto flush = [&] {
        if (!labels.empty() || !chunks.empty())
            arms.push_back({labels, join_sv(chunks, "\n"), stops});
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
            auto [src, nxt] = upto_colon(rest);
            labels.push_back(eval_src(p.st, src));
            text = nxt;
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
            std::tie(std::ignore, text) = stmt(text);
            stops = true;
            continue;
        }
        std::string src;
        std::tie(src, text) = consume_stmt_src(text);
        if (stops) continue;
        if (!strip(src).empty()) chunks.push_back(strip(src));
    }
    flush();
    return arms;
}

std::pair<std::string, std::string> consume_stmt_src(std::string raw) {
    auto text = lstrip(raw);
    std::size_t skip = raw.size() - text.size();
    auto taken = [&](const std::string& rest) -> std::pair<std::string, std::string> {
        auto idx = raw.size() - rest.size();
        return {raw.substr(skip, idx - skip), rest};
    };
    if (text.empty()) return {"", ""};
    if (text.starts_with("{")) {
        auto [_, rest] = brace(text);
        return taken(rest);
    }
    if (starts_kw(text, "if") || starts_kw(text, "for") || starts_kw(text, "while") || starts_kw(text, "switch") ||
        starts_kw(text, "do")) {
        std::string rest = text;
        if (starts_kw(rest, "do")) {
            rest = lstrip(rest.substr(2));
            std::tie(std::ignore, rest) = take_block(rest);
            rest = lstrip(rest);
            if (starts_kw(rest, "while")) {
                rest = lstrip(rest.substr(5));
                std::tie(std::ignore, rest) = paren(rest);
                rest = lstrip(rest);
                if (rest.starts_with(";")) rest = rest.substr(1);
            }
            return taken(rest);
        }
        std::size_t kw = starts_kw(text, "if") ? 2 : starts_kw(text, "for") ? 3 : starts_kw(text, "while") ? 5 : 6;
        rest = lstrip(text.substr(kw));
        std::tie(std::ignore, rest) = paren(rest);
        std::tie(std::ignore, rest) = take_block(rest);
        auto stripped = lstrip(rest);
        if (starts_kw(text, "if") && starts_kw(stripped, "else")) {
            std::tie(std::ignore, rest) = take_block(stripped.substr(4));
        }
        return taken(rest);
    }
    auto [st, rest] = stmt(text);
    return taken(rest);
}

std::string CParser::do_switch(const std::string& text) {
    auto rest = lstrip(text.substr(6));
    auto [cond, after] = paren(rest);
    std::string body;
    std::tie(body, after) = take_block(after);
    auto scrut = eval_src(st, cond);
    auto arms = parse_switch_arms(*this, body);
    if (arms.empty()) return after;
    int idx = -1, def = -1;
    for (int i = 0; i < static_cast<int>(arms.size()); ++i) {
        for (auto& lab : arms[static_cast<std::size_t>(i)].labels) {
            if (!lab) def = i;
            else if (*lab == scrut) {
                idx = i;
                break;
            }
        }
        if (idx >= 0) break;
    }
    if (idx < 0) idx = def;
    if (idx < 0) return after;
    try {
        for (int j = idx; j < static_cast<int>(arms.size()); ++j) {
            if (!strip(arms[static_cast<std::size_t>(j)].code).empty())
                stmts(arms[static_cast<std::size_t>(j)].code);
            if (arms[static_cast<std::size_t>(j)].stops) break;
        }
    } catch (const BreakEx&) {
    }
    return after;
}

void CParser::stmts(std::string text) {
    text = strip(text);
    while (!text.empty()) {
        st.tick();
        text = lstrip(text);
        if (text.empty()) break;
        if (text.starts_with("{")) {
            auto [inner, rest] = brace(text);
            stmts(inner);
            text = rest;
            continue;
        }
        if (starts_kw(text, "if")) {
            text = do_if(text);
            continue;
        }
        if (starts_kw(text, "switch")) {
            text = do_switch(text);
            continue;
        }
        if (starts_kw(text, "do")) {
            text = do_do(text);
            continue;
        }
        if (starts_kw(text, "while")) {
            text = do_while(text);
            continue;
        }
        if (starts_kw(text, "for")) {
            text = do_for(text);
            continue;
        }
        if (starts_kw(text, "assert")) {
            text = do_assert(text);
            continue;
        }
        if (starts_kw(text, "return")) {
            auto [stt, rest] = stmt(text);
            text = rest;
            auto restv = strip(stt);
            if (starts_kw(restv, "return")) restv = strip(restv.substr(6));
            if (!restv.empty() && restv.back() == ';') restv.pop_back();
            restv = strip(restv);
            std::optional<int64_t> val;
            if (!restv.empty()) val = eval_src(st, restv);
            throw ReturnEx(val);
        }
        if (starts_kw(text, "break")) {
            std::tie(std::ignore, text) = stmt(text);
            throw BreakEx();
        }
        if (starts_kw(text, "continue")) {
            std::tie(std::ignore, text) = stmt(text);
            throw ContinueEx();
        }
        if (is_nested_function(text)) throw ParseFail("nested function unencoded");
        if (starts_kw(text, "goto")) {
            if (is_computed_goto(text)) throw ParseFail("computed goto unencoded");
            throw ParseFail("goto unencoded");
        }
        if (starts_kw(text, "throw")) throw ParseFail("throw unencoded");
        if (starts_kw(text, "asm") || starts_kw(text, "__asm__") || starts_kw(text, "__asm"))
            throw ParseFail("asm unencoded");
        if (starts_kw(text, "try") || starts_kw(text, "catch")) throw ParseFail("try unencoded");
        if (starts_kw(text, "case") || starts_kw(text, "default")) throw ParseFail("case/default outside switch");
        if (auto miss = unencoded_layout_prefix(text)) throw ParseFail(*miss);
        auto [stt, rest] = stmt(text);
        text = rest;
        if (auto miss = unencoded_layout_stmt(stt)) throw ParseFail(*miss);
        if (stt.starts_with("int ") || stt.starts_with("unsigned ") || stt.starts_with("long ") ||
            stt.starts_with("short ") || stt.starts_with("char ") || stt.starts_with("uint32_t ") ||
            stt.starts_with("int32_t ") || stt.starts_with("size_t "))
            decl(stt);
        else
            assign_or_expr(stt);
    }
}

struct ExecResult {
    std::string ub;
    std::optional<int64_t> value;
    std::string error;
    int steps = 0;
};

ExecResult execute(const FunctionInfo& fn, const std::map<std::string, int>& args,
                   std::optional<std::map<std::string, int>> enums = std::nullopt) {
    if (fn.kind == "POINTER") return {"", std::nullopt, "skip-pointer", 0};
    auto en = enums ? *enums : enums_for(fn);
    St st(fn.params, args, std::move(en));
    try {
        CParser(fn.body, st).run();
    } catch (const UB& u) {
        return {u.cls, std::nullopt, "", st.steps};
    } catch (const ReturnEx& r) {
        return {"", r.value ? std::optional<int64_t>(i32(*r.value)) : std::nullopt, "", st.steps};
    } catch (const ParseFail& ex) {
        return {"", std::nullopt, ex.what(), st.steps};
    } catch (const BreakEx&) {
        return {"", st.vars.contains("__ret") ? std::optional<int64_t>(st.vars["__ret"]) : std::nullopt, "",
                st.steps};
    } catch (const std::exception& ex) {
        return {"", std::nullopt, ex.what(), st.steps};
    }
    return {"", st.vars.contains("__ret") ? std::optional<int64_t>(st.vars["__ret"]) : std::nullopt, "", st.steps};
}

int param_nbytes(const std::vector<std::pair<std::string, std::string>>& params) {
    int n = 0;
    for (auto& [typ, _] : params) {
        std::string key;
        for (char c : typ)
            if (c != '*') key.push_back(c);
        key = strip(key);
        auto it = kCTypeSize.find(key);
        n += it == kCTypeSize.end() ? 4 : it->second;
    }
    return n ? n : 1;
}

std::map<std::string, int> decode_args(const FunctionInfo& fn, const std::vector<uint8_t>& data) {
    std::map<std::string, int> args;
    std::size_t off = 0;
    for (auto& [typ, name] : fn.params) {
        if (name.empty()) continue;
        std::string key;
        for (char c : typ)
            if (c != '*') key.push_back(c);
        key = strip(key);
        int sz = kCTypeSize.contains(key) ? kCTypeSize.at(key) : 4;
        std::vector<uint8_t> chunk(static_cast<std::size_t>(sz), 0);
        for (int i = 0; i < sz && off + static_cast<std::size_t>(i) < data.size(); ++i)
            chunk[static_cast<std::size_t>(i)] = data[off + static_cast<std::size_t>(i)];
        bool uns = key.starts_with("uint") || key.starts_with("unsigned") || key == "size_t" || key == "uintptr_t";
        int64_t raw = 0;
        for (int i = sz - 1; i >= 0; --i) raw = (raw << 8) | chunk[static_cast<std::size_t>(i)];
        if (!uns && sz <= 4 && (raw & (int64_t{1} << (sz * 8 - 1))))
            raw -= int64_t{1} << (sz * 8);
        args[name] = sz >= 8 ? static_cast<int>(raw) : i32(raw);
        off += static_cast<std::size_t>(sz);
    }
    return args;
}

std::vector<std::vector<uint8_t>> interesting_seeds(const FunctionInfo& fn) {
    int n = param_nbytes(fn.params);
    if (n <= 0) return {{0}};
    std::vector<int64_t> values{0, -1, INT_MAX_32, INT_MIN_32, 1, 4, 31, 32, 100, INT_MAX_32 - 100, INT_MAX_32 - 99};
    std::vector<std::vector<uint8_t>> out;
    std::set<std::vector<uint8_t>> seen;
    auto add = [&](std::vector<uint8_t> b) {
        if (static_cast<int>(b.size()) > n) b.resize(static_cast<std::size_t>(n));
        while (static_cast<int>(b.size()) < n) b.push_back(0);
        if (seen.insert(b).second) out.push_back(std::move(b));
    };
    auto pack_i = [](int32_t v) {
        uint32_t u = static_cast<uint32_t>(v);
        return std::vector<uint8_t>{static_cast<uint8_t>(u), static_cast<uint8_t>(u >> 8),
                                    static_cast<uint8_t>(u >> 16), static_cast<uint8_t>(u >> 24)};
    };
    add(std::vector<uint8_t>(static_cast<std::size_t>(n), 0));
    add(std::vector<uint8_t>(static_cast<std::size_t>(n), 0xFF));
    std::vector<std::string> slots;
    for (auto& [t, name] : fn.params)
        if (!name.empty()) slots.push_back(name);
    if (slots.empty()) slots.emplace_back("_");
    for (auto v : values) {
        std::vector<uint8_t> blob;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            auto p = pack_i(i32(v));
            blob.insert(blob.end(), p.begin(), p.end());
        }
        add(std::move(blob));
    }
    if (slots.size() >= 2) {
        auto imax = pack_i(static_cast<int32_t>(INT_MAX_32));
        auto z = pack_i(0);
        std::vector<uint8_t> a = imax;
        for (std::size_t i = 1; i < slots.size(); ++i) a.insert(a.end(), z.begin(), z.end());
        add(std::move(a));
        std::vector<uint8_t> b = z;
        b.insert(b.end(), imax.begin(), imax.end());
        for (std::size_t i = 2; i < slots.size(); ++i) b.insert(b.end(), z.begin(), z.end());
        add(std::move(b));
        auto one = pack_i(1);
        std::vector<uint8_t> c = one;
        for (std::size_t i = 1; i < slots.size(); ++i) c.insert(c.end(), z.begin(), z.end());
        add(std::move(c));
    }
    return out;
}

std::optional<std::vector<uint8_t>> bytes_from_cex(const std::string& cex, int nbytes) {
    if (cex.empty()) return std::nullopt;
    std::vector<int> vals;
    std::string tmp = cex;
    std::istringstream ss(tmp);
    std::string part;
    while (std::getline(ss, part, ',')) {
        auto eq = part.find('=');
        if (eq == std::string::npos) continue;
        auto v = strip(part.substr(eq + 1));
        try {
            vals.push_back(std::stoi(v, nullptr, 0));
        } catch (...) {
            return std::nullopt;
        }
    }
    std::vector<uint8_t> raw;
    for (int v : vals) {
        uint32_t u = static_cast<uint32_t>(v);
        raw.push_back(static_cast<uint8_t>(u));
        raw.push_back(static_cast<uint8_t>(u >> 8));
        raw.push_back(static_cast<uint8_t>(u >> 16));
        raw.push_back(static_cast<uint8_t>(u >> 24));
    }
    if (static_cast<int>(raw.size()) > nbytes) raw.resize(static_cast<std::size_t>(nbytes));
    while (static_cast<int>(raw.size()) < nbytes) raw.push_back(0);
    return raw;
}

Finding bmc_one(const FunctionInfo& fn, int unwind) {
    auto recs = run_bmc({fn}, unwind);
    if (recs.empty()) {
        auto f = make_find("bmc", laws::ERROR, fn, "", "BMC produced no result", laws::STRENGTH_PROVES);
        return f;
    }
    auto r = recs[0];
    if (!r.function) r.function = fn.name;
    if (r.file.empty()) r.file = fn.file;
    if (!r.line) r.line = fn.line;
    return r;
}

struct Spec {
    std::vector<std::string> requires_;
    std::vector<std::string> ensures;
    std::vector<std::string> invariant;
    std::vector<std::string> decreases;
    std::optional<std::string> diff;
};

void parse_acsl_body(std::string body, Spec& spec) {
    auto pos = body.find("\\result");
    while (pos != std::string::npos) {
        body.replace(pos, 7, "result");
        pos = body.find("\\result", pos + 6);
    }
    std::istringstream ss(body);
    std::string part;
    static Regex kw("^(requires|ensures|invariant|decreases|assigns)\\s+(.+)$");
    while (std::getline(ss, part, ';')) {
        part = strip(part);
        if (part.empty()) continue;
        auto m = match_at(kw, part);
        if (!m) continue;
        auto kind = lower_copy(m->group(1));
        if (kind == "assigns") continue;
        auto val = strip(m->group(2));
        if (kind == "requires") spec.requires_.push_back(val);
        else if (kind == "ensures") spec.ensures.push_back(val);
        else if (kind == "invariant") spec.invariant.push_back(val);
        else if (kind == "decreases") spec.decreases.push_back(val);
    }
}

std::vector<std::string> extract_acsl_blocks(const std::string& text) {
    std::vector<std::string> bodies;
    std::size_t i = 0;
    while (i < text.size()) {
        auto start = text.find("/*@", i);
        if (start == std::string::npos) break;
        auto end = text.find("*/", start + 3);
        if (end == std::string::npos) break;
        bodies.push_back(text.substr(start + 3, end - (start + 3)));
        i = end + 2;
    }
    return bodies;
}

std::string acsl_preamble(const std::vector<std::string>& lines, int body_start) {
    int i = body_start - 1;
    bool in_block = false;
    while (i >= 0) {
        auto raw = lines[static_cast<std::size_t>(i)];
        auto s = strip(raw);
        if (in_block) {
            if (raw.find("/*@") != std::string::npos || s.starts_with("/*")) in_block = false;
            --i;
            continue;
        }
        if (s.empty()) {
            --i;
            continue;
        }
        if (s.starts_with("//")) {
            --i;
            continue;
        }
        if (s.ends_with("*/") || s.starts_with("/*") || s.starts_with("*")) {
            if (raw.find("/*") != std::string::npos && raw.find("*/") != std::string::npos) {
                --i;
                continue;
            }
            in_block = true;
            --i;
            continue;
        }
        break;
    }
    std::string out;
    for (int j = i + 1; j < body_start && j < static_cast<int>(lines.size()); ++j) {
        if (!out.empty()) out += '\n';
        out += lines[static_cast<std::size_t>(j)];
    }
    return out;
}

Spec parse_comments(const FunctionInfo& fn) {
    Spec spec;
    auto text = read_fn_source(fn);
    if (text.empty()) return spec;
    std::vector<std::string> lines;
    {
        std::istringstream ss(text);
        std::string ln;
        while (std::getline(ss, ln)) lines.push_back(ln);
    }
    int start = std::max(0, (fn.line ? fn.line : 1) - 2);
    int end = (fn.span.second ? fn.span.second : (fn.line ? fn.line : 1) + 40);
    end = std::min(static_cast<int>(lines.size()), end);
    int fn_line = fn.line ? fn.line : 1;
    int body_start = std::max(0, fn_line - 1);
    for (auto& b : extract_acsl_blocks(acsl_preamble(lines, body_start))) parse_acsl_body(b, spec);
    std::string region_body;
    for (int i = body_start; i < end; ++i) {
        if (!region_body.empty()) region_body += '\n';
        region_body += lines[static_cast<std::size_t>(i)];
    }
    for (auto& b : extract_acsl_blocks(region_body)) parse_acsl_body(b, spec);
    static Regex clause("(?://|/\\*|\\*)\\s*(requires|ensures|invariant|decreases|diff)\\s*:\\s*(.+?)(?:\\*/)?\\s*$");
    for (int i = start; i < end; ++i) {
        auto ln = lines[static_cast<std::size_t>(i)];
        auto stripped = strip(ln);
        if (stripped.starts_with("//@")) {
            parse_acsl_body(stripped.substr(3), spec);
            continue;
        }
        auto m = clause.search_match(ln);
        if (!m) continue;
        auto kind = lower_copy(m->group(1));
        auto val = strip(m->group(2));
        if (val.ends_with("*/")) val = strip(val.substr(0, val.size() - 2));
        if (kind == "diff") {
            auto sp = val.find(' ');
            spec.diff = sp == std::string::npos ? val : val.substr(0, sp);
            if (spec.diff->empty()) spec.diff.reset();
        } else if (kind == "requires") spec.requires_.push_back(val);
        else if (kind == "ensures") spec.ensures.push_back(val);
        else if (kind == "invariant") spec.invariant.push_back(val);
        else if (kind == "decreases") spec.decreases.push_back(val);
    }
    return spec;
}

Spec spec_comments_rapid(const FunctionInfo& fn) {
    Spec spec;
    auto text = read_fn_source(fn);
    if (text.empty()) return spec;
    std::vector<std::string> lines;
    {
        std::istringstream ss(text);
        std::string ln;
        while (std::getline(ss, ln)) lines.push_back(ln);
    }
    int start = std::max(0, (fn.line ? fn.line : 1) - 2);
    int end = (fn.span.second ? fn.span.second : (fn.line ? fn.line : 1) + 20);
    end = std::min(static_cast<int>(lines.size()), end);
    static Regex clause("(?://|/\\*|\\*)\\s*(requires|ensures|invariant|decreases|diff)\\s*:\\s*(.+?)(?:\\*/)?\\s*$");
    for (int i = start; i < end; ++i) {
        auto m = clause.search_match(lines[static_cast<std::size_t>(i)]);
        if (!m) continue;
        auto kind = lower_copy(m->group(1));
        auto val = strip(m->group(2));
        if (val.ends_with("*/")) val = strip(val.substr(0, val.size() - 2));
        if (kind == "diff") {
            auto sp = val.find(' ');
            spec.diff = sp == std::string::npos ? val : val.substr(0, sp);
        } else if (kind == "requires") spec.requires_.push_back(val);
        else if (kind == "ensures") spec.ensures.push_back(val);
        else if (kind == "invariant") spec.invariant.push_back(val);
        else if (kind == "decreases") spec.decreases.push_back(val);
    }
    if (spec.ensures.empty() && spec.requires_.empty()) {
        auto acsl = parse_comments(fn);
        if (!acsl.ensures.empty() || !acsl.requires_.empty()) return acsl;
    }
    return spec;
}

bool has_loop(const std::string& body) { return re_search("\\b(while|for)\\b", body); }
bool encode_decreases(const std::string& expr) {
    // Simple identifier only. Compound measures (n - i, n - 1, *, tuples)
    // are ERROR, never PROVED-ASSUMING: Python engine does not decide their
    // well-foundedness the way Dafny's VC generator does.
    static Regex re("^[A-Za-z_]\\w*$");
    std::string compact;
    for (char c : expr)
        if (!std::isspace(static_cast<unsigned char>(c))) compact.push_back(c);
    return !compact.empty() && fullmatch(re, compact);
}
bool encode_invariant(const std::string& expr) {
    static Regex re("^[A-Za-z_]\\w*\\s*(<=|>=|<|>|==|!=)\\s*([A-Za-z_]\\w*|0|[1-9]\\d*)$");
    auto parts = split_comma(expr); // wrong - split on &&
    std::vector<std::string> ps;
    std::string cur;
    for (std::size_t i = 0; i < expr.size();) {
        if (i + 1 < expr.size() && expr[i] == '&' && expr[i + 1] == '&') {
            auto p = strip(cur);
            if (!p.empty()) ps.push_back(p);
            cur.clear();
            i += 2;
        } else {
            cur.push_back(expr[i++]);
        }
    }
    auto tail = strip(cur);
    if (!tail.empty()) ps.push_back(tail);
    if (ps.empty()) return false;
    for (auto& p : ps)
        if (p.empty() || !fullmatch(re, p)) return false;
    return true;
}

std::string wrap_loop_inv(std::string loop_body, const std::string& inv) {
    auto inner = strip(loop_body);
    if (inner.starts_with("{") && inner.ends_with("}")) inner = inner.substr(1, inner.size() - 2);
    else inner = loop_body;
    return "{ { assert(" + inv + "); } " + inner + " { assert(" + inv + "); } }";
}
std::string wrap_loop_dec(std::string loop_body, const std::string& dec, int vid) {
    auto vn = "__prism_d" + std::to_string(vid);
    auto inner = strip(loop_body);
    if (inner.starts_with("{") && inner.ends_with("}")) inner = inner.substr(1, inner.size() - 2);
    else inner = loop_body;
    return "{ { assert((" + dec + ") >= 0); } { int " + vn + " = (" + dec + "); " + inner + " assert((" + dec +
           ") < " + vn + "); } }";
}

std::string transform_stmt_stream(const std::string& text_in,
                                  const std::function<std::string(const std::string&, const std::string&,
                                                                 const std::string&)>& on_loop,
                                  bool& ok) {
    std::string out;
    std::string text = text_in;
    std::size_t i = 0;
    try {
        while (i < text.size()) {
            auto rest = text.substr(i);
            auto stripped = lstrip(rest);
            if (stripped.empty()) {
                out += rest;
                break;
            }
            auto pad = rest.substr(0, rest.size() - stripped.size());
            out += pad;
            i += pad.size();
            if (stripped.starts_with("{")) {
                auto [inner, after] = brace(stripped);
                out += "{" + transform_stmt_stream(inner, on_loop, ok) + "}";
                i += stripped.size() - after.size();
                continue;
            }
            bool matched = false;
            for (auto* kw : {"while", "for"}) {
                if (starts_kw(stripped, kw)) {
                    auto [header, after_paren] = paren(stripped.substr(std::strlen(kw)));
                    auto [loop_body, after_body] = take_block_keep_braces(after_paren);
                    out += on_loop(kw, header, loop_body);
                    i += stripped.size() - after_body.size();
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
            if (starts_kw(stripped, "if")) {
                auto [cond, after_paren] = paren(stripped.substr(2));
                auto [then_src, after_then] = take_block_keep_braces(after_paren);
                after_then = lstrip(after_then);
                std::string clause = "if (" + cond + ") " + transform_stmt_stream(then_src, on_loop, ok);
                if (starts_kw(after_then, "else")) {
                    auto [else_src, after_else] = take_block_keep_braces(after_then.substr(4));
                    clause += " else " + transform_stmt_stream(else_src, on_loop, ok);
                    after_then = after_else;
                }
                out += clause;
                i += stripped.size() - after_then.size();
                continue;
            }
            auto [stt, after] = take_block_keep_braces(stripped);
            out += stt;
            i += stripped.size() - after.size();
        }
    } catch (const ParseFail&) {
        ok = false;
        return text_in;
    } catch (const std::exception&) {
        ok = false;
        return text_in;
    }
    return out;
}

std::pair<std::string, bool> instrument_invariant(const std::string& body, const std::string& invariant) {
    auto inv = strip(invariant);
    if (!encode_invariant(inv)) return {body, false};
    bool ok = true;
    auto out = transform_stmt_stream(body,
                                     [&](const std::string& kw, const std::string& header, const std::string& lb) {
                                         return "{ assert(" + inv + "); } " + kw + " (" + header + ") " +
                                                wrap_loop_inv(lb, inv);
                                     },
                                     ok);
    return {ok ? out : body, ok};
}
std::pair<std::string, bool> instrument_decreases(const std::string& body, const std::string& decreases) {
    auto dec = strip(decreases);
    if (!encode_decreases(dec)) return {body, false};
    int vid = 0;
    bool ok = true;
    auto out = transform_stmt_stream(body,
                                     [&](const std::string& kw, const std::string& header, const std::string& lb) {
                                         ++vid;
                                         return kw + " (" + header + ") " + wrap_loop_dec(lb, dec, vid);
                                     },
                                     ok);
    return {ok ? out : body, ok};
}

std::string rewrite_returns(const std::string& body, const std::string& ensures) {
    static Regex re("\\breturn\\s+([^;]+);");
    std::string out;
    std::size_t i = 0;
    for (auto& m : re.finditer(body)) {
        if (m.spans.empty()) continue;
        auto a = static_cast<std::size_t>(std::max(0, m.spans[0].first));
        auto b = static_cast<std::size_t>(std::max(0, m.spans[0].second));
        if (a < i) continue;
        out.append(body, i, a - i);
        auto expr = strip(m.group(1));
        if (expr.empty()) out.append(body, a, b - a);
        else
            out += "{ int result = " + expr + "; assert(" + ensures + "); return result; }";
        i = b;
    }
    out.append(body, i, std::string::npos);
    return out;
}

std::string instrument_body(const std::string& body, const std::optional<std::string>& requires_,
                            const std::optional<std::string>& ensures) {
    auto core = body;
    if (ensures) core = rewrite_returns(body, *ensures);
    std::string parts;
    if (requires_) parts += "if (!(" + *requires_ + ")) return 0;\n";
    parts += core;
    if (ensures && !re_search("\\breturn\\b", body)) parts += "\nassert(" + *ensures + ");";
    return parts;
}

Finding bmc_with_assume(const FunctionInfo& fn, int unwind, const std::optional<std::string>& requires_,
                        const std::optional<std::string>& ensures, const std::optional<std::string>& decreases,
                        const std::optional<std::string>& invariant) {
    auto core = fn.body;
    std::map<std::string, std::string> inv_extra, dec_extra;
    auto fail = [&](std::string msg, std::map<std::string, std::string> extra) {
        auto f = make_find("contracts", laws::ERROR, fn, "FUNC-CONTRACT", std::move(msg), laws::STRENGTH_PROVES);
        f.extra = std::move(extra);
        if (requires_) f.extra["requires"] = *requires_;
        if (ensures) f.extra["ensures"] = *ensures;
        return f;
    };
    if (invariant && has_loop(core)) {
        if (!encode_invariant(*invariant))
            return fail("cannot encode invariant (" + *invariant + ") for BMC",
                        {{"invariant", *invariant}, {"invariant_unencoded", "true"}});
        bool ok = false;
        std::tie(core, ok) = instrument_invariant(core, *invariant);
        inv_extra["invariant"] = *invariant;
        inv_extra["invariant_encoded"] = ok ? "true" : "false";
        if (!ok)
            return fail("invariant (" + *invariant + ") not instrumented",
                        {{"invariant", *invariant}, {"invariant_unencoded", "true"}});
    }
    if (decreases) {
        // Honesty: a non-identifier measure is ERROR even when there is no
        // loop to instrument. Never silently drop it into PROVED-ASSUMING.
        if (!encode_decreases(*decreases))
            return fail("cannot encode decreases (" + *decreases + ") for BMC",
                        {{"decreases", *decreases}, {"decreases_unencoded", "true"}});
        if (has_loop(core)) {
            bool ok = false;
            std::tie(core, ok) = instrument_decreases(core, *decreases);
            dec_extra["decreases"] = *decreases;
            dec_extra["decreases_encoded"] = ok ? "true" : "false";
            if (!ok)
                return fail("decreases (" + *decreases + ") not instrumented",
                            {{"decreases", *decreases}, {"decreases_unencoded", "true"}});
        }
    }
    auto cloned = fn;
    cloned.body = instrument_body(core, requires_, ensures);
    auto r = bmc_one(cloned, unwind);
    r.stage = "contracts";
    if (r.cls.empty()) r.cls = "FUNC-CONTRACT";
    r.extra["requires"] = requires_ ? *requires_ : "";
    r.extra["ensures"] = ensures ? *ensures : "";
    r.extra["assumed"] = (requires_ || invariant) ? "true" : "false";
    r.extra["original_status"] = r.status;
    for (auto& [k, v] : inv_extra) r.extra[k] = v;
    for (auto& [k, v] : dec_extra) r.extra[k] = v;
    if (laws::is_proof(r.status) && (requires_ || invariant)) {
        r.status = std::string(laws::PROVED_ASSUMING);
        std::vector<std::string> parts;
        if (requires_) parts.push_back("(" + *requires_ + ")");
        if (invariant) parts.push_back("invariant (" + *invariant + ")");
        r.message = "ensures holds assuming " + join_sv(parts, " and ") + "; never an unconditional PROVED";
    }
    return r;
}

std::vector<std::pair<std::string, std::string>> ptr_params(const FunctionInfo& fn) {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& [t, n] : fn.params)
        if (!n.empty() && (t.find('*') != std::string::npos || t.find('[') != std::string::npos))
            out.push_back({t, n});
    return out;
}
std::vector<std::pair<std::string, std::string>> scalar_params(const FunctionInfo& fn) {
    auto ptrs = ptr_params(fn);
    std::unordered_set<std::string> pn;
    for (auto& [t, n] : ptrs) pn.insert(n);
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& [t, n] : fn.params)
        if (!n.empty() && !pn.contains(n)) out.push_back({t, n});
    return out;
}

std::optional<int> honest_size(const std::vector<std::tuple<std::string, std::string, int, std::string>>& atoms,
                               const std::set<std::string>& ptr_names, const std::set<std::string>& scalar_names) {
    std::vector<int> sizes;
    for (auto& [name, op, val, req] : atoms) {
        if (ptr_names.contains(name)) continue;
        if (!scalar_names.contains(name) && !scalar_names.empty()) continue;
        if (op == "<" && val > 0) sizes.push_back(val);
        else if (op == "<=" && val >= 0) sizes.push_back(val + 1);
    }
    if (!sizes.empty()) {
        int k = *std::min_element(sizes.begin(), sizes.end());
        return k > 0 ? std::optional<int>(k) : std::nullopt;
    }
    std::set<std::string> null_ok;
    for (auto& [name, op, val, req] : atoms)
        if (ptr_names.contains(name) && op == "!=" && val == 0) null_ok.insert(name);
    if (!ptr_names.empty()) {
        bool all = true;
        for (auto& n : ptr_names)
            if (!null_ok.contains(n)) all = false;
        if (all) return 1;
    }
    return std::nullopt;
}

std::optional<FunctionInfo> materialize(const FunctionInfo& fn) {
    if (fn.kind != "POINTER") return std::nullopt;
    auto spec = parse_comments(fn);
    std::vector<std::string> reqs;
    for (auto& r : spec.requires_) {
        auto s = strip(r);
        if (!s.empty()) reqs.push_back(s);
    }
    if (reqs.empty()) return std::nullopt;
    auto ptrs = ptr_params(fn);
    if (ptrs.empty()) return std::nullopt;
    auto scalars = scalar_params(fn);
    std::set<std::string> ptr_names, scalar_names;
    for (auto& [t, n] : ptrs) ptr_names.insert(n);
    for (auto& [t, n] : scalars) scalar_names.insert(n);
    static Regex atom("^([A-Za-z_]\\w*)\\s*(==|!=|<=|>=|<|>)\\s*(0|[1-9]\\d*)$");
    std::vector<std::tuple<std::string, std::string, int, std::string>> atoms;
    for (auto& req : reqs) {
        auto m = match_at(atom, req);
        if (m) atoms.emplace_back(m->group(1), m->group(2), std::stoi(m->group(3)), req);
    }
    auto k = honest_size(atoms, ptr_names, scalar_names);
    if (!k) return std::nullopt;
    std::vector<std::string> decls;
    for (auto& [t, name] : ptrs) {
        decls.push_back("int _h_" + name + "[" + std::to_string(*k) + "];");
        decls.push_back("int *" + name + " = _h_" + name + ";");
    }
    std::string guard;
    for (std::size_t i = 0; i < reqs.size(); ++i) {
        if (i) guard += " && ";
        guard += "(" + reqs[i] + ")";
    }
    auto parts = decls;
    if (!guard.empty()) parts.push_back("if (!(" + guard + ")) return 0;");
    parts.push_back(fn.body);
    auto out = fn;
    out.kind = scalars.empty() ? "VOID" : "SCALAR";
    out.params = scalars;
    std::string param_s;
    for (std::size_t i = 0; i < scalars.size(); ++i) {
        if (i) param_s += ", ";
        param_s += strip(scalars[i].first + " " + scalars[i].second);
    }
    if (param_s.empty()) param_s = "void";
    auto ret = fn.return_type.empty() ? "int" : fn.return_type;
    out.signature = ret + " " + fn.name + "(" + param_s + ")";
    out.body = join_sv(parts, "\n");
    return out;
}

int c_type_nbytes_key(std::string typ) {
    typ = strip(typ);
    auto it = kCTypeSize.find(typ);
    return it == kCTypeSize.end() ? 4 : it->second;
}

std::string afl_harness_source(const FunctionInfo& fn, std::string src_rel);
std::pair<bool, std::string> compile_afl_harness(const fs::path& harness, const fs::path& exe);

Finding fuzz_function(const FunctionInfo& fn, const fs::path& src, double budget, int iters,
                      const std::vector<std::vector<uint8_t>>* seeds) {
    if (fn.kind == "POINTER")
        return make_find("fuzz", laws::NEEDS_HARNESS, fn, "",
                         "POINTER: no honest fuzzer harness (would invent a buffer or pass NULL)",
                         laws::STRENGTH_FINDS);
    if (fn.kind == "OTHER")
        return make_find("fuzz", laws::NEEDS_HARNESS, fn, "", "OTHER signature, not harnessed",
                         laws::STRENGTH_FINDS);
    if (auto syn = unencoded_syntax_reason(fn, "fuzzer"))
        return make_find("fuzz", laws::NEEDS_HARNESS, fn, "", *syn, laws::STRENGTH_FINDS);
    if (body_needs_pointer_harness(fn.body))
        return make_find("fuzz", laws::NEEDS_HARNESS, fn, "",
                         "local pointer or heap object: fuzzer would invent a buffer", laws::STRENGTH_FINDS);
    int nbytes = param_nbytes(fn.params);
    std::vector<std::vector<uint8_t>> corpus;
    for (auto& s : interesting_seeds(fn)) {
        auto b = s;
        if (static_cast<int>(b.size()) > nbytes) b.resize(static_cast<std::size_t>(nbytes));
        while (static_cast<int>(b.size()) < nbytes) b.push_back(0);
        corpus.push_back(std::move(b));
    }
    if (seeds)
        for (auto s : *seeds) {
            if (static_cast<int>(s.size()) > nbytes) s.resize(static_cast<std::size_t>(nbytes));
            while (static_cast<int>(s.size()) < nbytes) s.push_back(0);
            corpus.push_back(std::move(s));
        }
    if (corpus.empty()) {
        std::vector<uint8_t> rnd(static_cast<std::size_t>(nbytes));
        std::mt19937 rng{std::random_device{}()};
        for (auto& b : rnd) b = static_cast<uint8_t>(rng());
        corpus.push_back(std::move(rnd));
    }
    bool noseed = !seeds;
    auto t0 = std::chrono::steady_clock::now();
    std::set<uint64_t> seen;
    int new_cov = 0, stall = 0, i = 0;
    auto note_cov = [&](const std::vector<uint8_t>& child) {
        auto h = coverage_hash(child.data(), child.size());
        if (!seen.contains(h)) {
            seen.insert(h);
            corpus.push_back(child);
            ++new_cov;
            stall = 0;
        } else {
            ++stall;
        }
    };
    auto crash_from = [&](const std::vector<uint8_t>& child, const std::map<std::string, int>& args,
                          const std::string& cls, int ii) {
        std::string argstr;
        for (auto& [k, v] : args) {
            if (!argstr.empty()) argstr += ", ";
            argstr += k + "=" + std::to_string(v);
        }
        std::string hex;
        for (std::size_t k = 0; k < child.size() && k < 16; ++k) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "%02x", child[k]);
            hex += buf;
        }
        auto f = make_find("fuzz", laws::CRASH, fn, cls, cls + " on " + hex + " " + argstr, laws::STRENGTH_FINDS);
        f.counterexample = hex + " " + argstr;
        f.extra["iters"] = std::to_string(ii);
        f.extra["corpus"] = std::to_string(corpus.size());
        f.extra["new_cov"] = std::to_string(new_cov);
        f.extra["noseed"] = noseed ? "true" : "false";
        f.extra["oracle"] = "concrete";
        return f;
    };
    std::deque<std::vector<uint8_t>> queue(corpus.begin(), corpus.end());
    while (!queue.empty()) {
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= std::max(budget, 1.0)) break;
        auto child = queue.front();
        queue.pop_front();
        ++i;
        auto args = decode_args(fn, child);
        auto rec = execute(fn, args);
        if (!rec.ub.empty()) return crash_from(child, args, rec.ub, i);
        note_cov(child);
    }
    uint64_t hseed = 1;
    while (i < iters) {
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= budget) break;
        auto parent = corpus[static_cast<std::size_t>(i) % corpus.size()];
        auto child = parent;
        havoc(child.data(), child.size(), ++hseed);
        ++i;
        auto args = decode_args(fn, child);
        auto rec = execute(fn, args);
        if (!rec.ub.empty()) return crash_from(child, args, rec.ub, i);
        note_cov(child);
    }
    auto extra_iters = std::to_string(i);
    auto extra_corpus = std::to_string(corpus.size());
    auto extra_cov = std::to_string(new_cov);
    auto extra_noseed = noseed ? "true" : "false";
    auto extra_stall = std::to_string(stall);
    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double remain = std::max(0.05, budget - elapsed);
    int bin_iters = std::min(32, std::max(1, iters));
    // Law 9: the compiled harness runs the scanned function; without
    // --allow-exec only the concrete oracle above ran.
    const bool exec_ok = sandbox::allowed();
    auto cc = exec_ok ? Config{}.which({"gcc", "clang"}) : std::nullopt;
    if (cc && fs::exists(src)) {
        auto td = fs::temp_directory_path() / ("prism_fuzzbin_" + std::to_string(std::random_device{}()));
        fs::create_directories(td);
        struct Guard {
            fs::path p;
            ~Guard() {
                std::error_code ec;
                fs::remove_all(p, ec);
            }
        } guard{td};
        auto src_copy = td / src.filename();
        try {
            std::ofstream out(src_copy);
            out << read_text_file(src);
        } catch (...) {
            cc.reset();
        }
        if (cc) {
            auto hpath = td / ("harness_" + fn.name + ".c");
            {
                std::ofstream out(hpath);
                out << afl_harness_source(fn, src.filename().string());
            }
            auto exe = td / ("harness_" + fn.name + ".exe");
            auto [ok, err] = compile_afl_harness(hpath, exe);
            if (!ok) {
                (void)err;
                auto f = make_find("fuzz", laws::CLEAN, fn, "",
                                   "no crash in " + extra_iters + " iters / " +
                                       std::format("{:.1f}", budget) + "s (not a proof)",
                                   laws::STRENGTH_FINDS);
                f.extra["iters"] = extra_iters;
                f.extra["corpus"] = extra_corpus;
                f.extra["new_cov"] = extra_cov;
                f.extra["noseed"] = extra_noseed;
                f.extra["stall"] = extra_stall;
                f.extra["oracle"] = "concrete";
                f.extra["binary"] = "compile-failed";
                return f;
            }
            auto bin_crash = [&](const std::vector<uint8_t>& child, const std::string& detail, int n) {
                auto args = decode_args(fn, child);
                std::string hex;
                for (std::size_t k = 0; k < child.size() && k < 16; ++k) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "%02x", child[k]);
                    hex += buf;
                }
                auto f = make_find("fuzz", laws::CRASH, fn, "FUZZ-CRASH", "crash on " + hex + "…",
                                   laws::STRENGTH_FINDS);
                f.extra["sandbox"] = sandbox::kind();
                std::string full;
                for (auto b : child) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "%02x", b);
                    full += buf;
                }
                f.counterexample = full;
                f.evidence = detail.substr(0, 800);
                f.extra["iters"] = extra_iters;
                f.extra["corpus"] = extra_corpus;
                f.extra["new_cov"] = extra_cov;
                f.extra["noseed"] = extra_noseed;
                f.extra["stall"] = extra_stall;
                f.extra["oracle"] = "binary";
                f.extra["binary_iters"] = std::to_string(n);
                std::string argstr;
                for (auto& [k, v] : args) {
                    if (!argstr.empty()) argstr += ", ";
                    argstr += k + "=" + std::to_string(v);
                }
                f.extra["args"] = argstr;
                return f;
            };
            auto is_bin_crash = [&](const ProcRun& rr) {
                if (rr.timeout) return false;
                if (rr.crashed || rr.rc < 0) return true;
                auto low = lower_copy(rr.err);
                return low.find("runtime error") != std::string::npos ||
                       low.find("undefinedbehaviorsanitizer") != std::string::npos ||
                       low.find("addresssanitizer") != std::string::npos ||
                       low.find("heap-buffer-overflow") != std::string::npos ||
                       low.find("heap-use-after-free") != std::string::npos;
            };
            auto tbin = std::chrono::steady_clock::now();
            int n = 0;
            int take = std::max(1, std::min(bin_iters, static_cast<int>(corpus.size())));
            for (int k = 0; k < take; ++k) {
                auto elapsed_bin =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - tbin).count();
                if (elapsed_bin > remain) break;
                ++n;
                std::string in(corpus[static_cast<std::size_t>(k)].begin(),
                               corpus[static_cast<std::size_t>(k)].end());
                auto rr = run_argv(sandbox::wrap_argv({exe.string()}, td), in, 1.0,
                                   sandbox::limits_for(1.0, /*limit_as=*/false));
                if (is_bin_crash(rr))
                    return bin_crash(corpus[static_cast<std::size_t>(k)], rr.err, n);
            }
            uint64_t hbin = 91;
            while (n < bin_iters) {
                auto elapsed_bin =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - tbin).count();
                if (elapsed_bin >= remain) break;
                auto child = corpus[static_cast<std::size_t>(n) % corpus.size()];
                havoc(child.data(), child.size(), ++hbin);
                ++n;
                std::string in(child.begin(), child.end());
                auto rr = run_argv(sandbox::wrap_argv({exe.string()}, td), in, 1.0,
                                   sandbox::limits_for(1.0, /*limit_as=*/false));
                if (is_bin_crash(rr)) return bin_crash(child, rr.err, n);
            }
            auto f = make_find("fuzz", laws::CLEAN, fn, "",
                               "no crash in " + extra_iters + " iters / " +
                                   std::format("{:.1f}", budget) + "s (not a proof)",
                               laws::STRENGTH_FINDS);
            f.extra["iters"] = extra_iters;
            f.extra["corpus"] = extra_corpus;
            f.extra["new_cov"] = extra_cov;
            f.extra["noseed"] = extra_noseed;
            f.extra["stall"] = extra_stall;
            f.extra["oracle"] = "concrete";
            f.extra["binary_iters"] = std::to_string(n);
            f.extra["sandbox"] = sandbox::kind();
            return f;
        }
    }
    auto f = make_find("fuzz", laws::CLEAN, fn, "",
                       "no crash in " + extra_iters + " iters / " +
                           std::format("{:.1f}", budget) + "s (not a proof)",
                       laws::STRENGTH_FINDS);
    f.extra["iters"] = extra_iters;
    f.extra["corpus"] = extra_corpus;
    f.extra["new_cov"] = extra_cov;
    f.extra["noseed"] = extra_noseed;
    f.extra["stall"] = extra_stall;
    f.extra["oracle"] = "concrete";
    if (!exec_ok) {
        f.extra["binary"] = std::string(laws::NOTRUN);
        f.extra["exec"] = std::string(laws::NOTRUN);
    }
    return f;
}

using Args = std::map<std::string, int>;
using ArgsKey = std::vector<std::pair<std::string, int>>;
ArgsKey args_key(const Args& a) {
    ArgsKey k;
    for (auto& [x, y] : a) k.emplace_back(x, i32(y));
    std::sort(k.begin(), k.end());
    return k;
}

std::vector<Args> initial_seeds(const FunctionInfo& fn) {
    std::vector<std::string> names;
    for (auto& [t, n] : fn.params)
        if (!n.empty()) names.push_back(n);
    if (names.empty()) return {Args{}};
    std::vector<Args> out;
    std::set<ArgsKey> seen;
    auto add = [&](Args args) {
        Args norm;
        for (auto& n : names) norm[n] = i32(args.contains(n) ? args[n] : 0);
        auto key = args_key(norm);
        if (seen.insert(key).second) out.push_back(std::move(norm));
    };
    Args z;
    for (auto& n : names) z[n] = 0;
    add(z);
    for (auto& blob : interesting_seeds(fn)) add(decode_args(fn, blob));
    const int64_t extremes[] = {0, 1, -1, INT_MAX_32, INT_MIN_32};
    for (auto& name : names) {
        for (auto v : extremes) {
            auto nxt = z;
            nxt[name] = i32(v);
            add(nxt);
        }
    }
    if (names.size() <= 3) {
        std::function<void(std::size_t, Args)> rec = [&](std::size_t i, Args cur) {
            if (i == names.size()) {
                add(cur);
                return;
            }
            for (auto v : extremes) {
                cur[names[i]] = i32(v);
                rec(i + 1, cur);
            }
        };
        rec(0, {});
    }
    return out;
}

std::vector<std::string> branch_conditions(const FunctionInfo& fn) {
    static Regex re("\\bif\\s*\\(([^)]+)\\)");
    std::vector<std::string> seen;
    for (auto& m : re.finditer(fn.body)) {
        auto cond = m.group(1);
        std::string n;
        bool sp = false;
        for (char c : cond) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!sp && !n.empty()) {
                    n.push_back(' ');
                    sp = true;
                }
            } else {
                n.push_back(c);
                sp = false;
            }
        }
        while (!n.empty() && n.back() == ' ') n.pop_back();
        if (!n.empty() && std::find(seen.begin(), seen.end(), n) == seen.end()) seen.push_back(n);
    }
    return seen;
}

std::string norm_ws(std::string_view s) {
    std::string n;
    bool sp = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!sp && !n.empty()) {
                n.push_back(' ');
                sp = true;
            }
        } else {
            n.push_back(c);
            sp = false;
        }
    }
    while (!n.empty() && n.back() == ' ') n.pop_back();
    return n;
}

void add_goal(std::vector<std::string>& seen, std::string cond) {
    cond = norm_ws(cond);
    if (!cond.empty() && std::find(seen.begin(), seen.end(), cond) == seen.end())
        seen.push_back(std::move(cond));
}

std::vector<std::string> branch_goals(const FunctionInfo& fn) {
    // FuSeBMC MyVisitor::check / checkStmt: then, implicit else, loop-exit, switch cases.
    static Regex if_re("\\bif\\s*\\(([^)]+)\\)");
    static Regex while_re("\\bwhile\\s*\\(([^)]+)\\)");
    static Regex for_re("\\bfor\\s*\\(([^)]*)\\)");
    static Regex switch_re("\\bswitch\\s*\\(([^)]+)\\)");
    static Regex case_re("\\bcase\\s+([^:]+):");
    std::vector<std::string> seen;
    for (auto& m : if_re.finditer(fn.body)) {
        auto cond = norm_ws(m.group(1));
        if (cond.empty()) continue;
        add_goal(seen, cond);
        add_goal(seen, "!(" + cond + ")");
    }
    for (auto& m : while_re.finditer(fn.body)) {
        auto cond = norm_ws(m.group(1));
        if (cond.empty()) continue;
        add_goal(seen, cond);
        add_goal(seen, "!(" + cond + ")");
    }
    for (auto& m : for_re.finditer(fn.body)) {
        auto inner = m.group(1);
        auto semi = inner.find(';');
        if (semi == std::string::npos) continue;
        auto semi2 = inner.find(';', semi + 1);
        auto mid = inner.substr(semi + 1, semi2 == std::string::npos ? std::string::npos : semi2 - semi - 1);
        auto cond = norm_ws(mid);
        if (cond.empty()) continue;
        add_goal(seen, cond);
        add_goal(seen, "!(" + cond + ")");
    }
    auto switches = switch_re.finditer(fn.body);
    for (std::size_t i = 0; i < switches.size(); ++i) {
        auto expr = norm_ws(switches[i].group(1));
        if (expr.empty()) continue;
        std::size_t from = switches[i].spans.empty() ? 0 : static_cast<std::size_t>(std::max(0, switches[i].spans[0].second));
        std::size_t to = fn.body.size();
        if (i + 1 < switches.size() && !switches[i + 1].spans.empty() && switches[i + 1].spans[0].first >= 0)
            to = static_cast<std::size_t>(switches[i + 1].spans[0].first);
        if (from > to) from = to;
        auto rest = fn.body.substr(from, to - from);
        for (auto& cm : case_re.finditer(rest)) {
            auto lab = norm_ws(cm.group(1));
            if (!lab.empty()) add_goal(seen, "(" + expr + ") == (" + lab + ")");
        }
    }
    return seen;
}

std::vector<std::pair<std::string, std::string>> numbered_goals(const FunctionInfo& fn) {
    // FuSeBMC GoalCounter::GetNewGoalForFunc: ++counter, "GOAL_" + counter.
    auto conds = branch_goals(fn);
    std::vector<std::pair<std::string, std::string>> out;
    unsigned long long counter = 0;
    for (auto& c : conds) {
        ++counter;
        out.emplace_back("GOAL_" + std::to_string(counter), c);
    }
    return out;
}

std::optional<bool> eval_cond(const FunctionInfo& fn, const Args& args, const std::string& cond) {
    try {
        auto en = enums_for(fn);
        St st(fn.params, args, en);
        return truth(eval_src(st, cond));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Args> heuristic_flip(const FunctionInfo& fn, const Args& args, const std::string& cond0, bool want) {
    std::unordered_set<std::string> param_names;
    for (auto& [t, n] : fn.params)
        if (!n.empty()) param_names.insert(n);
    auto cond = cond0;
    {
        std::string n;
        bool sp = false;
        for (char c : cond) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!sp && !n.empty()) {
                    n.push_back(' ');
                    sp = true;
                }
            } else {
                n.push_back(c);
                sp = false;
            }
        }
        cond = strip(n);
    }
    static Regex m1("^(\\w+)\\s*(>=|<=|==|!=|>|<)\\s*(-?\\d+)$");
    if (auto m = match_at(m1, cond)) {
        auto var = m->group(1);
        auto op = m->group(2);
        int val = i32(std::stoi(m->group(3)));
        if (!param_names.contains(var)) return std::nullopt;
        auto nxt = args;
        if (op == ">") nxt[var] = want ? val + 1 : val;
        else if (op == ">=") nxt[var] = want ? val : val - 1;
        else if (op == "<") nxt[var] = want ? val - 1 : val;
        else if (op == "<=") nxt[var] = want ? val : val + 1;
        else if (op == "==") nxt[var] = want ? val : val + 1;
        else if (op == "!=") nxt[var] = want ? val + 1 : val;
        else return std::nullopt;
        nxt[var] = i32(nxt[var]);
        auto got = eval_cond(fn, nxt, cond);
        if (got && *got == want) return nxt;
        return std::nullopt;
    }
    static Regex m2("^(\\w+)\\s*(>=|<=|==|!=|>|<)\\s*(\\w+)$");
    if (auto m = match_at(m2, cond)) {
        auto left = m->group(1), op = m->group(2), right = m->group(3);
        if (!param_names.contains(left)) return std::nullopt;
        auto nxt = args;
        int rhs = 0;
        if (param_names.contains(right)) rhs = i32(args.contains(right) ? args.at(right) : 0);
        else {
            try {
                rhs = i32(std::stoi(right, nullptr, 0));
            } catch (...) {
                return std::nullopt;
            }
        }
        if (op == "==") nxt[left] = want ? rhs : rhs + 1;
        else if (op == "!=") nxt[left] = want ? rhs + 1 : rhs;
        else if (op == "<") nxt[left] = want ? rhs - 1 : rhs;
        else if (op == ">") nxt[left] = want ? rhs + 1 : rhs;
        else if (op == "<=") nxt[left] = want ? rhs : rhs + 1;
        else if (op == ">=") nxt[left] = want ? rhs : rhs - 1;
        else return std::nullopt;
        nxt[left] = i32(nxt[left]);
        auto got = eval_cond(fn, nxt, cond);
        if (got && *got == want) return nxt;
        return std::nullopt;
    }
    static Regex m3("^(\\w+)$");
    if (auto m = match_at(m3, cond)) {
        auto var = m->group(1);
        if (!param_names.contains(var)) return std::nullopt;
        auto nxt = args;
        nxt[var] = want ? 1 : 0;
        auto got = eval_cond(fn, nxt, cond);
        if (got && *got == want) return nxt;
    }
    for (auto& name : param_names) {
        auto nxt = args;
        int v = i32(args.contains(name) ? args.at(name) : 0);
        for (int64_t cand : {int64_t{0}, int64_t{1}, int64_t{-1}, int64_t{INT_MAX_32},
                             int64_t{INT_MIN_32}, int64_t{-v}, int64_t{v + 1}, int64_t{v - 1}}) {
            nxt[name] = i32(cand);
            auto got = eval_cond(fn, nxt, cond);
            if (got && *got == want && args_key(nxt) != args_key(args)) return nxt;
        }
    }
    return std::nullopt;
}

std::string oracle_tag(bool this_from_z3, int z3_seeds) {
    if (this_from_z3 || z3_seeds > 0) return "z3";
    return "concrete";
}

// Returns (next_args, via_z3, was_unsat). KLEE Executor::fork drops an
// unsat side; we do the same and do not invent a heuristic neighbor.
std::tuple<std::optional<Args>, bool, bool> neighbor_for_cond(const FunctionInfo& fn, const Args& args,
                                                             const std::string& cond) {
    auto cur = eval_cond(fn, args, cond);
    if (!cur) return {std::nullopt, false, false};
    bool want = !*cur;
    auto solved = solve_fork_flip(fn, args, cond, want);
    if (solved.kind == ForkFlipKind::Unsat) return {std::nullopt, false, true};
    if (solved.kind == ForkFlipKind::Model) return {solved.args, true, false};
    auto nxt = heuristic_flip(fn, args, cond, want);
    return {nxt, false, false};
}

Finding concolic_function(const FunctionInfo& fn, int budget) {
    auto base_nh = [&](std::string msg) {
        return make_find("concolic", laws::NEEDS_HARNESS, fn, "", std::move(msg), laws::STRENGTH_FINDS);
    };
    if (fn.kind == "POINTER")
        return base_nh("pointer parameter: concolic engine does not invent buffers");
    if (fn.kind == "OTHER")
        return base_nh("non-scalar parameter: concolic engine does not invent objects");
    if (body_needs_pointer_harness(fn.body))
        return base_nh("local pointer or heap object: concolic engine does not invent buffers");
    if (has_self_call(fn))
        return base_nh("recursive call unencoded: concrete bound is not a proof of the callee");
    if (has_unencoded_float(fn))
        return base_nh("float/double unencoded: concrete oracle is not an IEEE model");
    if (has_unencoded_cxx(fn))
        return base_nh("C++ view/span unencoded: concolic engine is not a lifetime model");
    if (has_unencoded_throw(fn))
        return base_nh("C++ throw unencoded: concolic engine is not an exception model");
    if (has_unencoded_setjmp(fn))
        return base_nh(
            "setjmp/longjmp/va_list unencoded: concolic engine is not a nonlocal-control model");
    if (auto syn = unencoded_syntax_reason(fn, "concolic engine")) return base_nh(*syn);
    auto queue = initial_seeds(fn);
    std::set<ArgsKey> seen;
    std::set<ArgsKey> z3_keys;
    int tried = 0, generated = 0, z3_seeds = 0, skipped_unsat = 0, max_new = std::max(0, budget);
    auto fill_extra = [&](Finding& f, const Args* crash_args = nullptr, bool this_from_z3 = false) {
        f.extra["tried"] = std::to_string(tried);
        f.extra["generated"] = std::to_string(generated);
        f.extra["oracle"] = oracle_tag(this_from_z3, z3_seeds);
        f.extra["z3_seeds"] = std::to_string(z3_seeds);
        f.extra["skipped_unsat"] = std::to_string(skipped_unsat);
        if (crash_args) {
            std::string argstr;
            for (auto& [k, v] : *crash_args) {
                if (!argstr.empty()) argstr += ", ";
                argstr += k + "=" + std::to_string(v);
            }
            f.extra["args"] = argstr;
        }
    };
    while (!queue.empty()) {
        auto args = queue.front();
        queue.erase(queue.begin());
        auto key = args_key(args);
        if (seen.contains(key)) continue;
        seen.insert(key);
        ++tried;
        auto rec = execute(fn, args);
        if (!rec.error.empty()) {
            if (rec.error == "skip-pointer")
                return base_nh("pointer parameter: concolic engine does not invent buffers");
            if (auto msg = harness_for_parsefail(rec.error, "concolic engine")) {
                auto f = base_nh(*msg);
                f.extra["tried"] = std::to_string(tried);
                return f;
            }
            auto f = make_find("concolic", laws::ERROR, fn, "", rec.error, laws::STRENGTH_FINDS);
            f.extra["tried"] = std::to_string(tried);
            return f;
        }
        if (!rec.ub.empty()) {
            std::string argstr;
            for (auto& [k, v] : args) {
                if (!argstr.empty()) argstr += ", ";
                argstr += k + "=" + std::to_string(v);
            }
            auto f = make_find("concolic", laws::CRASH, fn, rec.ub, rec.ub + " on " + argstr,
                               laws::STRENGTH_FINDS);
            f.counterexample = argstr;
            fill_extra(f, &args, z3_keys.contains(key));
            return f;
        }
        if (generated >= max_new) continue;
        for (auto& cond : branch_conditions(fn)) {
            if (generated >= max_new) break;
            auto [nxt, via_z3, was_unsat] = neighbor_for_cond(fn, args, cond);
            if (was_unsat) {
                ++skipped_unsat;
                continue;
            }
            if (!nxt) continue;
            auto nkey = args_key(*nxt);
            if (seen.contains(nkey)) continue;
            ++generated;
            if (via_z3) {
                ++z3_seeds;
                z3_keys.insert(nkey);
            }
            queue.push_back(*nxt);
        }
    }
    auto f = make_find("concolic", laws::CLEAN, fn, "",
                       "no UB in " + std::to_string(tried) + " concolic inputs (not a proof)",
                       laws::STRENGTH_FINDS);
    fill_extra(f);
    return f;
}

std::vector<std::vector<uint8_t>> seeds_from_bmc(const FunctionInfo& fn, const std::vector<Finding>& bmc_findings) {
    int n = param_nbytes(fn.params);
    std::vector<std::vector<uint8_t>> out;
    for (auto& f : bmc_findings) {
        if (!f.function || *f.function != fn.name) continue;
        if (f.status != laws::FAILED) continue;
        auto b = bytes_from_cex(f.counterexample, n);
        if (b) out.push_back(*b);
    }
    return out;
}

struct LlamaEngine;
bool llama_engine_up(LlamaEngine* e);
std::vector<std::vector<uint8_t>> fuzz4all_seeds(LlamaEngine& engine, const FunctionInfo& fn, int nbytes);
std::vector<std::vector<uint8_t>> chatfuzz_mutants(LlamaEngine& engine, const FunctionInfo& fn,
                                                   const std::string& seed_hex);
std::vector<std::vector<uint8_t>> fuzz4all_mutate_interesting(LlamaEngine& engine, const FunctionInfo& fn,
                                                              const std::string& seed_hex,
                                                              const std::string& prev_hex = {},
                                                              int strategy = 1);
std::vector<std::vector<uint8_t>> fuzz4all_combine(LlamaEngine& engine, const FunctionInfo& fn,
                                                   const std::string& seed_hex, const std::string& prev_hex);
void pad_seed_nbytes(std::vector<uint8_t>& b, int n) {
    if (n < 0) n = 0;
    if (static_cast<int>(b.size()) > n) b.resize(static_cast<std::size_t>(n));
    while (static_cast<int>(b.size()) < n) b.push_back(0);
}
std::string bytes_to_hex(const std::vector<uint8_t>& b) {
    std::string hex;
    for (auto byte : b) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%02x", byte);
        hex += buf;
    }
    return hex;
}

bool env_flag_is_one(const char* key) {
    const char* v = std::getenv(key);
    return v != nullptr && std::string_view(v) == "1";
}

void env_setdefault(const char* key, const char* val) {
    const char* cur = std::getenv(key);
    if (cur && *cur) return;
#ifdef _WIN32
    SetEnvironmentVariableA(key, val);
#else
    ::setenv(key, val, 0);
#endif
}

std::optional<fs::path> afl_fuzz_which() {
    return Config{}.which({"afl-fuzz", "afl-fuzz.exe"});
}

std::string afl_harness_source(const FunctionInfo& fn, std::string src_rel) {
    for (char& c : src_rel)
        if (c == '\\') c = '/';
    int nbytes = param_nbytes(fn.params);
    std::string dlines, reads, args;
    int off = 0;
    for (auto& [typ, name] : fn.params) {
        if (name.empty()) continue;
        std::string key;
        for (char c : typ)
            if (c != '*') key.push_back(c);
        key = strip(key);
        int sz = kCTypeSize.contains(key) ? kCTypeSize.at(key) : 4;
        std::string decl = strip(typ);
        if (decl.empty()) decl = "int";
        dlines += "    " + decl + " " + name + ";\n";
        reads += "    memcpy(&" + name + ", buf + " + std::to_string(off) + ", " + std::to_string(sz) + ");\n";
        if (!args.empty()) args += ", ";
        args += name;
        off += sz;
    }
    return "#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n#include \"" + src_rel +
           "\"\n\nint main(void) {\n    unsigned char buf[" + std::to_string(nbytes) +
           "];\n    if (fread(buf, 1, " + std::to_string(nbytes) +
           ", stdin) != " + std::to_string(nbytes) + ") return 0;\n" + dlines + reads + "    (void)" +
           fn.name + "(" + args + ");\n    return 0;\n}\n";
}

std::pair<bool, std::string> compile_afl_harness(const fs::path& harness, const fs::path& exe) {
    // Python engine prism/afl.py compile_afl_harness sanitizer fallback:
    //   1. -fsanitize=address,undefined  -fno-sanitize-recover=address,undefined
    //   2. -fsanitize=undefined          -fno-sanitize-recover=undefined
    //   3. -fsanitize=address            -fno-sanitize-recover=address
    //   4. bare (no sanitizer)
    // Prefer ASan+UBSan together; fall back to one sanitizer, then bare.
    // TSan cannot combine with ASan. Missing sanitizer runtime is not a fake
    // CLEAN. Missing gcc/clang is mapped by the caller to NOTRUN, never ERROR.
    auto cc = Config{}.which({"gcc", "clang"});
    if (!cc) return {false, "no C compiler on PATH"};
    std::vector<std::string> cmd{cc->string(), "-O0", "-g", "-std=c11", harness.string(), "-o",
                                 exe.string()};
    const char* san_tries[][2] = {
        {"-fsanitize=address,undefined", "-fno-sanitize-recover=address,undefined"},
        {"-fsanitize=undefined", "-fno-sanitize-recover=undefined"},
        {"-fsanitize=address", "-fno-sanitize-recover=address"},
    };
    ProcRun p;
    bool compiled = false;
    for (auto& flags : san_tries) {
        std::vector<std::string> san = cmd;
        san.insert(san.begin() + 1, flags[0]);
        san.insert(san.begin() + 2, flags[1]);
        p = run_argv(san, {}, 30.0);
        if (p.timeout) return {false, "compile timeout"};
        if (p.rc == 0) {
            compiled = true;
            break;
        }
    }
    if (!compiled) {
        p = run_argv(cmd, {}, 30.0);
        if (p.timeout) return {false, "compile timeout"};
        if (p.rc != 0) {
            std::string err = p.err.empty() ? p.out : p.err;
            if (err.size() > 200) err.resize(200);
            return {false, err};
        }
    }
    return {true, {}};
}

// Bounded AFL++ campaign on a SCALAR stdin harness. CLEAN is not a proof.
std::optional<Finding> run_afl_fuzz(const FunctionInfo& fn, const fs::path& src, double timeout = 2.0) {
    auto afl = afl_fuzz_which();
    if (!afl || fn.kind != "SCALAR") return std::nullopt;
    auto afl_base = [&](std::string_view st, std::string cls, std::string msg) {
        auto f = make_find("fuse", st, fn, std::move(cls), std::move(msg), laws::STRENGTH_FINDS);
        if (st != laws::NOTRUN) f.extra["engine"] = "afl";
        return f;
    };
    if (!sandbox::allowed()) {
        // Law 9: the harness runs the scanned function.
        auto f = sandbox::exec_notrun("fuse", "AFL++ harness", {{"exec", std::string(laws::NOTRUN)}});
        f.file = fn.file;
        f.function = fn.name;
        f.line = fn.line;
        return f;
    }
    auto cc = Config{}.which({"gcc", "clang"});
    if (!cc) {
        auto f = afl_base(laws::NOTRUN, "", "AFL: no C compiler on PATH");
        f.extra["install"] = "install gcc or clang";
        return f;
    }
    auto td = fs::temp_directory_path() / ("prism_afl_" + std::to_string(std::random_device{}()));
    fs::create_directories(td);
    struct Guard {
        fs::path p;
        ~Guard() {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
    } guard{td};
    auto src_copy = td / src.filename();
    if (!fs::exists(src_copy)) {
        std::ofstream out(src_copy);
        out << read_text_file(src);
    }
    auto hpath = td / ("harness_" + fn.name + ".c");
    {
        std::ofstream out(hpath);
        out << afl_harness_source(fn, src.filename().string());
    }
    auto exe = td / ("harness_" + fn.name + ".exe");
    auto [ok, err] = compile_afl_harness(hpath, exe);
    if (!ok) {
        auto low = lower_copy(err);
        if (low.find("no c compiler") != std::string::npos ||
            low.find("gcc/clang") != std::string::npos ||
            (low.find("not on path") != std::string::npos &&
             (low.find("gcc") != std::string::npos || low.find("clang") != std::string::npos))) {
            auto f = afl_base(laws::NOTRUN, "", "AFL: no C compiler on PATH");
            f.extra["install"] = "install gcc or clang";
            return f;
        }
        return afl_base(laws::ERROR, "", "AFL: compile failed: " + err);
    }
    auto in_dir = td / "in";
    auto out_dir = td / "out";
    fs::create_directories(in_dir);
    fs::create_directories(out_dir);
    int nbytes = param_nbytes(fn.params);
    {
        std::ofstream seed(in_dir / "seed", std::ios::binary);
        std::vector<char> zeros(static_cast<std::size_t>(std::max(1, nbytes)), 0);
        seed.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    env_setdefault("AFL_NO_UI", "1");
    env_setdefault("AFL_SKIP_CPUFREQ", "1");
    env_setdefault("AFL_NO_AFFINITY", "1");
    int vsec = std::max(1, static_cast<int>(timeout));
    run_argv(sandbox::wrap_argv({afl->string(), "-i", in_dir.string(), "-o", out_dir.string(), "-V",
                                 std::to_string(vsec), "--", exe.string()},
                                td),
             {}, timeout + 10.0, sandbox::limits_for(timeout + 10.0, /*limit_as=*/false));
    std::vector<fs::path> crash_dirs{out_dir / "crashes", out_dir / "default" / "crashes"};
    for (auto& cdir : crash_dirs) {
        std::error_code ec;
        if (!fs::is_directory(cdir, ec)) continue;
        std::vector<fs::path> crashes;
        for (auto& ent : fs::directory_iterator(cdir, ec)) {
            if (ec) break;
            if (!ent.is_regular_file()) continue;
            if (ent.path().filename() == "README.txt") continue;
            crashes.push_back(ent.path());
        }
        if (crashes.empty()) continue;
        std::string data = read_text_file(crashes[0]);
        std::vector<uint8_t> raw(data.begin(), data.end());
        if (raw.size() > 16) raw.resize(16);
        auto f = afl_base(laws::CRASH, "AFL-CRASH", "AFL crash on " + bytes_to_hex(raw));
        f.counterexample = bytes_to_hex(std::vector<uint8_t>(data.begin(), data.end()));
        f.extra["afl_crashes"] = std::to_string(crashes.size());
        return f;
    }
    return afl_base(laws::CLEAN, "", "no AFL crash in " + std::to_string(vsec) + "s (not a proof)");
}

const char* kLibfuzzerInstall = "clang -fsanitize=fuzzer is a system tool: apt install clang-18 (see third_party/MANIFEST.toml)";

std::string libfuzzer_harness_source(const FunctionInfo& fn, std::string src_rel) {
    for (char& c : src_rel)
        if (c == '\\') c = '/';
    int nbytes = param_nbytes(fn.params);
    std::string dlines, reads, args;
    int off = 0;
    for (auto& [typ, name] : fn.params) {
        if (name.empty()) continue;
        std::string key;
        for (char c : typ)
            if (c != '*') key.push_back(c);
        key = strip(key);
        int sz = kCTypeSize.contains(key) ? kCTypeSize.at(key) : 4;
        std::string decl = strip(typ);
        if (decl.empty()) decl = "int";
        dlines += "    " + decl + " " + name + ";\n";
        reads += "    memcpy(&" + name + ", Data + " + std::to_string(off) + ", " + std::to_string(sz) + ");\n";
        if (!args.empty()) args += ", ";
        args += name;
        off += sz;
    }
    return "#include <stdint.h>\n#include <stddef.h>\n#include <string.h>\n#include \"" + src_rel +
           "\"\n\nint LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {\n    if (Size < " +
           std::to_string(nbytes) + ") return 0;\n" + dlines + reads + "    (void)" + fn.name + "(" + args +
           ");\n    return 0;\n}\n";
}

bool libfuzzer_flag_rejected(const std::string& text) {
    auto low = lower_copy(text);
    return low.find("unsupported") != std::string::npos || low.find("unknown") != std::string::npos ||
           low.find("unrecognized") != std::string::npos;
}

std::pair<std::string, std::string> compile_libfuzzer(const fs::path& clang, const fs::path& src,
                                                      const fs::path& exe) {
    const char* san_tries[][2] = {
        {"-fsanitize=fuzzer,address,undefined", "-fno-sanitize-recover=address,undefined"},
        {"-fsanitize=fuzzer,undefined", "-fno-sanitize-recover=undefined"},
        {"-fsanitize=fuzzer,address", "-fno-sanitize-recover=address"},
        {"-fsanitize=fuzzer", nullptr},
    };
    std::string last_text;
    bool rejected = false;
    for (auto& flags : san_tries) {
        std::vector<std::string> cmd{clang.string(), flags[0]};
        if (flags[1]) cmd.push_back(flags[1]);
        cmd.insert(cmd.end(), {"-O0", "-g", "-std=c11", src.string(), "-o", exe.string()});
        auto p = run_argv(cmd, {}, 30.0);
        last_text = p.err.empty() ? p.out : p.err + p.out;
        if (p.timeout) return {"timeout", last_text};
        if (p.rc == 0) return {"ok", {}};
        if (libfuzzer_flag_rejected(last_text)) rejected = true;
    }
    if (rejected) return {"notrun", last_text};
    return {"error", last_text};
}

Finding run_libfuzzer(const FunctionInfo& fn, const fs::path& src, double timeout = 2.0) {
    auto lf_base = [&](std::string_view st, std::string cls, std::string msg) {
        auto f = make_find("libfuzzer", st, fn, std::move(cls), std::move(msg), laws::STRENGTH_FINDS);
        if (st != laws::NOTRUN && st != laws::NEEDS_HARNESS) f.extra["engine"] = "libfuzzer";
        return f;
    };
    if (fn.kind == "POINTER")
        return lf_base(laws::NEEDS_HARNESS, "",
                       "POINTER: libFuzzer harness would invent a buffer or pass NULL");
    if (fn.kind == "OTHER")
        return lf_base(laws::NEEDS_HARNESS, "", "OTHER signature, not harnessed");
    if (auto syn = unencoded_syntax_reason(fn, "libFuzzer"))
        return lf_base(laws::NEEDS_HARNESS, "", *syn);
    if (body_needs_pointer_harness(fn.body))
        return lf_base(laws::NEEDS_HARNESS, "",
                       "local pointer or heap object: libFuzzer harness would invent a buffer");
    if (!sandbox::allowed()) {
        // Law 9: the libFuzzer binary runs the scanned function.
        auto f = sandbox::exec_notrun("libfuzzer", "libFuzzer harness",
                                      {{"exec", std::string(laws::NOTRUN)}});
        f.file = fn.file;
        f.function = fn.name;
        f.line = fn.line;
        return f;
    }
    auto clang = Config{}.which({"clang", "clang.exe"});
    if (!clang) {
        auto f = lf_base(laws::NOTRUN, "", "clang not on PATH");
        f.extra["install"] = kLibfuzzerInstall;
        return f;
    }
    auto td = fs::temp_directory_path() / ("prism_libfuzzer_" + std::to_string(std::random_device{}()));
    fs::create_directories(td);
    struct Guard {
        fs::path p;
        ~Guard() {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
    } guard{td};
    auto src_copy = td / src.filename();
    if (!fs::exists(src_copy)) {
        std::ofstream out(src_copy);
        out << read_text_file(src);
    }
    auto hpath = td / ("lfuzzer_" + fn.name + ".c");
    {
        std::ofstream out(hpath);
        out << libfuzzer_harness_source(fn, src.filename().string());
    }
    auto exe = td / ("lfuzzer_" + fn.name + ".exe");
    auto [st, err] = compile_libfuzzer(*clang, hpath, exe);
    if (st == "timeout") {
        auto f = lf_base(laws::TIMEOUT, "", "libFuzzer compile timeout");
        f.extra["install"] = kLibfuzzerInstall;
        f.extra["exe"] = clang->string();
        return f;
    }
    if (st == "notrun") {
        auto f = lf_base(laws::NOTRUN, "", "clang has no libFuzzer (-fsanitize=fuzzer)");
        f.extra["install"] = kLibfuzzerInstall;
        f.extra["exe"] = clang->string();
        return f;
    }
    if (st != "ok") {
        std::string msg = err;
        if (msg.size() > 200) msg.resize(200);
        auto f = lf_base(laws::ERROR, "", "libFuzzer compile failed: " + msg);
        f.extra["exe"] = clang->string();
        return f;
    }
    auto corpus = td / "corpus";
    fs::create_directories(corpus);
    int nbytes = param_nbytes(fn.params);
    {
        std::ofstream seed(corpus / "seed", std::ios::binary);
        std::string zeros(static_cast<std::size_t>(std::max(0, nbytes)), '\0');
        seed.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    int vsec = std::max(1, static_cast<int>(timeout));
    // crash-* artifacts land in the scratch dir (the jail's only writable dir).
    auto r = run_argv(sandbox::wrap_argv({exe.string(), corpus.string(),
                                          "-max_total_time=" + std::to_string(vsec), "-timeout=1",
                                          "-artifact_prefix=" + (td / "").string()},
                                         td),
                      {}, timeout + 10.0, sandbox::limits_for(timeout + 10.0, /*limit_as=*/false));
    std::vector<fs::path> crashes;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(td, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        auto name = it->path().filename().string();
        if (name.rfind("crash-", 0) == 0) crashes.push_back(it->path());
    }
    if (!crashes.empty()) {
        std::string data = read_text_file(crashes[0]);
        std::vector<uint8_t> raw(data.begin(), data.end());
        if (raw.size() > 16) raw.resize(16);
        auto f = lf_base(laws::CRASH, "LIBFUZZER-CRASH", "libFuzzer crash on " + bytes_to_hex(raw));
        f.counterexample = bytes_to_hex(std::vector<uint8_t>(data.begin(), data.end()));
        f.extra["exe"] = clang->string();
        f.extra["libfuzzer_crashes"] = std::to_string(crashes.size());
        return f;
    }
    std::string text = lower_copy(r.err + r.out);
    if (text.find("addresssanitizer") != std::string::npos ||
        text.find("undefinedbehaviorsanitizer") != std::string::npos) {
        auto f = lf_base(laws::CRASH, "LIBFUZZER-CRASH", "libFuzzer sanitizer crash");
        f.extra["exe"] = clang->string();
        return f;
    }
    auto f = lf_base(laws::CLEAN, "", "no libFuzzer crash in " + std::to_string(vsec) + "s (not a proof)");
    f.extra["exe"] = clang->string();
    return f;
}

Finding fuse_one(const FunctionInfo& fn, const std::vector<Finding>& bmc_findings, const fs::path& root,
                 double budget, int iters, int rounds, LlamaEngine* engine) {
    auto nh = [&](std::string msg) {
        return make_find("fuse", laws::NEEDS_HARNESS, fn, "", std::move(msg), laws::STRENGTH_FINDS);
    };
    if (fn.kind == "POINTER")
        return nh("POINTER: FuSeBMC harness would invent a buffer or pass NULL");
    if (fn.kind == "OTHER") return nh("OTHER signature, not harnessed");
    if (auto syn = unencoded_syntax_reason(fn, "FuSeBMC")) return nh(*syn);
    if (body_needs_pointer_harness(fn.body))
        return nh("local pointer or heap object: FuSeBMC harness would invent a buffer");
    bool afl_on_path = afl_fuzz_which().has_value();
    bool opted_afl = env_flag_is_one("PRISM_AFL");
    bool use_afl = opted_afl && afl_on_path;
    bool opted_libfuzzer = env_flag_is_one("PRISM_LIBFUZZER");
    fs::path src = fn.file;
    if (!src.is_absolute()) src = root / fn.file;
    if (!fs::exists(src) && fs::is_regular_file(root)) src = root;
    if (!fs::exists(src) && fs::exists(root / fs::path(fn.file).filename()))
        src = root / fs::path(fn.file).filename();
    auto seeds = seeds_from_bmc(fn, bmc_findings);
    int from_bmc = static_cast<int>(seeds.size());
    auto labeled = numbered_goals(fn);
    std::set<std::string> covered;
    Finding last;
    std::map<std::string, std::string> extra{
        {"from_bmc", std::to_string(from_bmc)},
        {"rounds", "0"},
        {"new_bmc_seeds", "0"},
        {"noseed", from_bmc == 0 ? "true" : "false"},
    };
    {
        nlohmann::json ids = nlohmann::json::array();
        nlohmann::json gmap = nlohmann::json::object();
        for (auto& [lab, cond] : labeled) {
            ids.push_back(lab);
            gmap[lab] = cond;
        }
        extra["goals"] = ids.dump();
        extra["goal_ids"] = ids.dump();
        extra["goal_map"] = gmap.dump();
        extra["new_goals"] = nlohmann::json::array().dump();
    }
    extra["llm"] = engine ? "true" : "false";
    bool chatfuzz_done = false;
    bool mutate_done = false;
    bool combine_done = false;
    std::vector<uint8_t> prev_interesting;
    bool afl_tried = false;
    bool libfuzzer_tried = false;
    if (opted_afl && !afl_on_path) {
        extra["afl"] = "NOTRUN";
        extra["install"] = adapter_install("afl-fuzz");
    } else if (afl_on_path && !use_afl) {
        extra["afl_available"] = "true";
    }
    bool llm_up = llama_engine_up(engine);
    if (engine && !llm_up) {
        extra["autoprompt"] = "NOTRUN";
        extra["chatfuzz"] = "NOTRUN";
        extra["install"] = "ollama serve  (qwen3.5:9b) or build prism with -DPRISM_LLAMA=ON";
    }
    if (llm_up) {
        try {
            int n = param_nbytes(fn.params);
            int added = 0;
            for (auto b : fuzz4all_seeds(*engine, fn, n)) {
                pad_seed_nbytes(b, n);
                if (!b.empty() && std::find(seeds.begin(), seeds.end(), b) == seeds.end()) {
                    seeds.push_back(std::move(b));
                    ++added;
                }
            }
            extra["fuzz4all"] = std::to_string(added);
            extra["autoprompt"] = "ok";
        } catch (const std::exception& ex) {
            extra["fuzz4all_error"] = std::string(ex.what()).substr(0, 200);
        }
    }
    for (int r = 0; r < rounds; ++r) {
        extra["rounds"] = std::to_string(r + 1);
        try {
            last = fuzz_function(fn, src, budget, iters, seeds.empty() ? nullptr : &seeds);
        } catch (const std::exception& ex) {
            auto f = make_find("fuse", laws::ERROR, fn, "", ex.what(), laws::STRENGTH_FINDS);
            f.extra = extra;
            return f;
        }
        last.stage = "fuse";
        if (last.extra.contains("iters")) extra["fuzz_iters"] = last.extra["iters"];
        if (last.extra.contains("corpus")) extra["corpus"] = last.extra["corpus"];
        if (last.extra.contains("new_cov")) extra["new_cov"] = last.extra["new_cov"];
        if (last.extra.contains("sandbox")) extra["sandbox"] = last.extra["sandbox"];
        if (auto ex = last.extra.find("exec"); ex != last.extra.end() && ex->second == laws::NOTRUN) {
            // Law 9: the compiled-harness half was held back (no --allow-exec).
            extra["binary"] = std::string(laws::NOTRUN);
            extra["exec"] = std::string(laws::NOTRUN);
        }
        if (last.status == laws::CRASH || last.status == laws::ERROR || last.status == laws::NEEDS_HARNESS) {
            for (auto& [k, v] : extra) last.extra[k] = v;
            return last;
        }
        if (use_afl && fn.kind == "SCALAR" && !afl_tried) {
            afl_tried = true;
            auto afl_last = run_afl_fuzz(fn, src, 2.0);
            if (afl_last) {
                if (afl_last->status == laws::NOTRUN) {
                    extra["afl"] = "NOTRUN";
                    extra.erase("engine");
                    if (afl_last->extra.contains("install")) extra["install"] = afl_last->extra["install"];
                    if (afl_last->extra.contains("exec")) extra["exec"] = afl_last->extra["exec"];
                } else if (afl_last->status == laws::CRASH || afl_last->status == laws::ERROR) {
                    extra["engine"] = "afl";
                    for (auto& [k, v] : extra) afl_last->extra[k] = v;
                    return *afl_last;
                } else {
                    extra["engine"] = "afl";
                }
            }
        }
        if (opted_libfuzzer && fn.kind == "SCALAR" && !libfuzzer_tried) {
            libfuzzer_tried = true;
            auto lf_last = run_libfuzzer(fn, src, 2.0);
            if (lf_last.status == laws::NOTRUN) {
                extra["libfuzzer"] = "NOTRUN";
                auto eng = extra.find("engine");
                if (eng != extra.end() && eng->second == "libfuzzer") extra.erase(eng);
                if (lf_last.extra.contains("install")) extra["install"] = lf_last.extra["install"];
                else extra["install"] = kLibfuzzerInstall;
                if (lf_last.extra.contains("exec")) extra["exec"] = lf_last.extra["exec"];
            } else if (lf_last.status == laws::NEEDS_HARNESS) {
                for (auto& [k, v] : extra) lf_last.extra[k] = v;
                return lf_last;
            } else if (lf_last.status == laws::CRASH || lf_last.status == laws::ERROR) {
                extra["engine"] = "libfuzzer";
                for (auto& [k, v] : extra) lf_last.extra[k] = v;
                return lf_last;
            } else {
                extra["engine"] = "libfuzzer";
                extra["libfuzzer"] = "ok";
            }
        }
        int new_cov = 0;
        if (last.extra.contains("new_cov")) {
            try {
                new_cov = std::stoi(last.extra["new_cov"]);
            } catch (...) {
            }
        }
        nlohmann::json newly = nlohmann::json::array();
        for (auto& s : seeds) {
            auto args = decode_args(fn, s);
            for (auto& [lab, cond] : labeled) {
                if (covered.contains(lab)) continue;
                auto v = eval_cond(fn, args, cond);
                if (v && *v) {
                    covered.insert(lab);
                    newly.push_back(lab);
                }
            }
        }
        if (!newly.empty()) {
            nlohmann::json ng = nlohmann::json::array();
            if (extra.contains("new_goals")) {
                try {
                    ng = nlohmann::json::parse(extra["new_goals"]);
                } catch (...) {
                }
            }
            for (auto& x : newly) ng.push_back(x);
            extra["new_goals"] = ng.dump();
        }
        if (llm_up && !chatfuzz_done && new_cov == 0) {
            chatfuzz_done = true;
            try {
                int n = param_nbytes(fn.params);
                std::string seed_hex = seeds.empty() ? std::string(static_cast<std::size_t>(std::max(0, n)) * 2, '0')
                                                     : bytes_to_hex(seeds[0]);
                extra["chatfuzz"] = "true";
                for (auto m : chatfuzz_mutants(*engine, fn, seed_hex)) {
                    pad_seed_nbytes(m, n);
                    if (!m.empty() && std::find(seeds.begin(), seeds.end(), m) == seeds.end())
                        seeds.push_back(std::move(m));
                }
            } catch (const std::exception& ex) {
                extra["chatfuzz_error"] = std::string(ex.what()).substr(0, 200);
            }
        }
        if (llm_up && new_cov > 0) {
            try {
                int n = param_nbytes(fn.params);
                std::vector<uint8_t> parent = seeds.empty() ? std::vector<uint8_t>(static_cast<std::size_t>(std::max(0, n)), 0)
                                                           : seeds.back();
                pad_seed_nbytes(parent, n);
                std::string parent_hex = bytes_to_hex(parent);
                if (!mutate_done) {
                    mutate_done = true;
                    extra["fuzz4all_mutate"] = "true";
                    for (auto m : fuzz4all_mutate_interesting(*engine, fn, parent_hex)) {
                        pad_seed_nbytes(m, n);
                        if (!m.empty() && std::find(seeds.begin(), seeds.end(), m) == seeds.end())
                            seeds.push_back(std::move(m));
                    }
                }
                std::string prev_hex;
                if (!prev_interesting.empty())
                    prev_hex = bytes_to_hex(prev_interesting);
                else if (seeds.size() >= 2)
                    prev_hex = bytes_to_hex(seeds[seeds.size() - 2]);
                if (!prev_hex.empty() && !combine_done) {
                    combine_done = true;
                    extra["fuzz4all_combine"] = "true";
                    for (auto m : fuzz4all_combine(*engine, fn, parent_hex, prev_hex)) {
                        pad_seed_nbytes(m, n);
                        if (!m.empty() && std::find(seeds.begin(), seeds.end(), m) == seeds.end())
                            seeds.push_back(std::move(m));
                    }
                }
                prev_interesting = parent;
            } catch (const std::exception& ex) {
                extra["fuzz4all_mutate_error"] = std::string(ex.what()).substr(0, 200);
            }
        }
#ifdef PRISM_HAS_Z3
        for (auto& [lab, cond] : labeled) {
            if (covered.contains(lab)) continue;
            auto cloned = fn;
            cloned.body = "if (!(" + cond + ")) return 0;\n" + fn.body;
            auto g = bmc_one(cloned, 8);
            extra["rounds"] = std::to_string(r + 1);
            if (g.status == laws::FAILED && !g.counterexample.empty()) {
                covered.insert(lab);
                nlohmann::json ids = nlohmann::json::array();
                if (extra.contains("bmc_goals")) {
                    try {
                        ids = nlohmann::json::parse(extra["bmc_goals"]);
                    } catch (...) {
                    }
                }
                ids.push_back(lab);
                extra["bmc_goals"] = ids.dump();
                int n = param_nbytes(fn.params);
                auto b = bytes_from_cex(g.counterexample, n);
                if (b && std::find(seeds.begin(), seeds.end(), *b) == seeds.end()) {
                    seeds.push_back(*b);
                    extra["new_bmc_seeds"] = std::to_string(std::stoi(extra["new_bmc_seeds"]) + 1);
                }
            } else if (g.status == laws::PROVED || g.status == laws::PROVED_UNBOUNDED ||
                       g.status == laws::BOUNDED) {
                covered.insert(lab);
            }
        }
#endif
    }
    extra["covered_goals"] = nlohmann::json(std::vector<std::string>(covered.begin(), covered.end())).dump();
    extra["seeds"] = std::to_string(seeds.size());
    auto eng = extra.find("engine");
    auto afl_flag = extra.find("afl");
    if (afl_flag != extra.end() && afl_flag->second == "NOTRUN" && eng != extra.end() &&
        eng->second == "afl")
        extra.erase("engine");
    auto lf_flag = extra.find("libfuzzer");
    eng = extra.find("engine");
    if (lf_flag != extra.end() && lf_flag->second == "NOTRUN" && eng != extra.end() &&
        eng->second == "libfuzzer")
        extra.erase("engine");
    if (use_afl && afl_tried) {
        auto it = extra.find("afl");
        if (it == extra.end() || it->second != "NOTRUN") extra["engine"] = "afl";
    }
    auto f = make_find("fuse", laws::CLEAN, fn, "",
                       "no crash in FuSeBMC loop (" + extra["rounds"] + " rounds; not a proof)",
                       laws::STRENGTH_FINDS);
    f.extra = extra;
    return f;
}

const char* SYSTEM_AUDITOR =
    "You are PRISM, a code auditor. You READ code. You never claim a proof. "
    "Every defect you name is a HYPOTHESIS that must be checked by BMC, a "
    "fuzzer, or a human. Reply with JSON only: "
    "{\"hypotheses\":[{\"function\":\"...\",\"line\":0,\"cls\":\"INT-SIGNED-OVF\",\"why\":\"...\"}]}";
const char* SYSTEM_REPAIR =
    "You receive C source plus compiler/sanitizer/BMC output. Reply with "
    "a full corrected C file only, no markdown fences unless the file itself "
    "needs them. Preserve function names.";
const char* SYSTEM_HARNESS =
    "Write a complete C file that includes the target as a string in comments "
    "and a main() that tests the stated property. No markdown.";
const char* SYSTEM_FUZZ4ALL =
    "You distill a fuzzing prompt. Given C source, reply JSON: "
    "{\"prompt\":\"...\", \"seeds\":[\"hexbytes\", \"...\"]}. Seeds are little-endian "
    "argument encodings as lowercase hex. No prose.";
const char* AP_SYSTEM_MESSAGE = "You are an auto-prompting tool";
const char* AP_INSTRUCTION =
    "Please summarize the above documentation in a concise manner to describe the usage and "
    "functionality of the target ";
const char* SYSTEM_FUZZ4ALL_MUTATE =
    "The previous generation was interesting. Mutate it into a new valid encoding. "
    "Given the function and a hex seed, reply JSON: "
    "{\"mutants\":[\"hex\", \"...\"]}. No prose.";
const char* SYSTEM_FUZZ4ALL_COMBINE =
    "Combine the two previous interesting encodings into one valid encoding. "
    "Reply JSON: "
    "{\"mutants\":[\"hex\", \"...\"]}. No prose.";
const char* SYSTEM_CHATFUZZ =
    "Coverage has stalled. Given the function and a hex seed, reply JSON: "
    "{\"mutants\":[\"hex\", \"...\"]} semantically valid argument encodings. No prose.";
const char* LLM_UNAVAILABLE_MSG = "llama.cpp/Ollama not reachable";
const char* LLM_SKIP_FUSE_MSG = "llama.cpp/Ollama not reachable; stall mutants / autoprompt skipped";
const char* LLM_INSTALL = "ollama serve  (qwen3.5:9b) or build prism with -DPRISM_LLAMA=ON";

// Python engine llm_complete_unavailable: HTTP/connection is a missing backend.
auto llm_httpish = [](const std::string& err) {
    auto low = lower_copy(err);
    return low.find("http error") != std::string::npos || low.find("http ") != std::string::npos ||
           low.find("urlopen") != std::string::npos || low.find("urlerror") != std::string::npos ||
           low.find("connection refused") != std::string::npos ||
           low.find("connection reset") != std::string::npos ||
           low.find("connection aborted") != std::string::npos ||
           low.find("not reachable") != std::string::npos ||
           low.find("not loaded") != std::string::npos ||
           low.find("failed to connect") != std::string::npos ||
           low.find("name or service not known") != std::string::npos ||
           low.find("winerror") != std::string::npos || low.find("errno 111") != std::string::npos ||
           low.find("errno 104") != std::string::npos;
};

nlohmann::json extract_json(std::string text) {
    text = strip(text);
    if (text.starts_with("```")) {
        while (!text.empty() && text.front() == '`') text.erase(text.begin());
        auto nl = text.find('\n');
        if (nl != std::string::npos) text = text.substr(nl + 1);
        auto end = text.rfind("```");
        if (end != std::string::npos) text = text.substr(0, end);
    }
    try {
        return nlohmann::json::parse(text);
    } catch (...) {
        auto a = text.find('{');
        auto b = text.rfind('}');
        if (a != std::string::npos && b > a) {
            try {
                return nlohmann::json::parse(text.substr(a, b - a + 1));
            } catch (...) {
            }
        }
    }
    return nullptr;
}

struct ChatResult {
    std::string text;
    std::string backend;
    std::string error;
};

#ifdef PRISM_HAS_LLAMA
struct NativeLlama {
    std::mutex mu;
    llama_model* model = nullptr;
    std::string loaded_path;
    bool backends = false;

    ~NativeLlama() {
        if (model) llama_model_free(model);
    }

    llama_model* get(const fs::path& path, std::string& err) {
        std::lock_guard<std::mutex> lock(mu);
        if (!backends) {
            llama_log_set(
                [](enum ggml_log_level level, const char* text, void*) {
                    if (level >= GGML_LOG_LEVEL_ERROR) std::fputs(text, stderr);
                },
                nullptr);
            ggml_backend_load_all();
            llama_backend_init();
            backends = true;
        }
        auto s = path.string();
        if (model && loaded_path == s) return model;
        if (model) {
            llama_model_free(model);
            model = nullptr;
        }
        auto params = llama_model_default_params();
        params.n_gpu_layers = 0;
        model = llama_model_load_from_file(s.c_str(), params);
        if (!model) {
            err = "failed to load GGUF " + s;
            return nullptr;
        }
        loaded_path = std::move(s);
        return model;
    }
};

static NativeLlama g_native_llama;

static ChatResult native_llama_complete(const Config& cfg,
                                        const std::vector<std::pair<std::string, std::string>>& messages) {
    if (cfg.gguf.empty() || !fs::exists(cfg.gguf)) {
        return {"", "llama.cpp", "missing GGUF (set PRISM_GGUF)"};
    }
    std::string err;
    llama_model* model = g_native_llama.get(cfg.gguf, err);
    if (!model) return {"", "llama.cpp", err};

    const llama_vocab* vocab = llama_model_get_vocab(model);
    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = 2048;
    ctx_params.n_batch = 2048;
    llama_context* ctx = llama_init_from_model(model, ctx_params);
    if (!ctx) return {"", "llama.cpp", "failed to create llama_context"};

    std::vector<llama_chat_message> chat;
    chat.reserve(messages.size());
    for (auto& [role, content] : messages) chat.push_back({role.c_str(), content.c_str()});
    const char* tmpl = llama_model_chat_template(model, nullptr);
    std::vector<char> formatted(llama_n_ctx(ctx));
    int n = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, formatted.data(),
                                      static_cast<int32_t>(formatted.size()));
    if (n > static_cast<int>(formatted.size())) {
        formatted.resize(static_cast<std::size_t>(n));
        n = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, formatted.data(),
                                      static_cast<int32_t>(formatted.size()));
    }
    std::string prompt;
    if (n < 0) {
        for (auto& [role, content] : messages) {
            prompt += role;
            prompt += ": ";
            prompt += content;
            prompt += "\n";
        }
        prompt += "assistant:";
    } else {
        prompt.assign(formatted.data(), static_cast<std::size_t>(n));
    }

    const int n_prompt = -llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), nullptr, 0,
                                         true, true);
    if (n_prompt <= 0) {
        llama_free(ctx);
        return {"", "llama.cpp", "failed to tokenize prompt"};
    }
    std::vector<llama_token> prompt_tokens(static_cast<std::size_t>(n_prompt));
    if (llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), prompt_tokens.data(),
                       static_cast<int32_t>(prompt_tokens.size()), true, true) < 0) {
        llama_free(ctx);
        return {"", "llama.cpp", "failed to tokenize prompt"};
    }

    llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.2f));
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    llama_batch batch = llama_batch_get_one(prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()));
    std::string text;
    constexpr int kMaxNew = 256;
    for (int i = 0; i < kMaxNew; ++i) {
        if (llama_decode(ctx, batch) != 0) {
            llama_sampler_free(smpl);
            llama_free(ctx);
            return {"", "llama.cpp", "llama_decode failed"};
        }
        llama_token id = llama_sampler_sample(smpl, ctx, -1);
        if (llama_vocab_is_eog(vocab, id)) break;
        char buf[256];
        int n_piece = llama_token_to_piece(vocab, id, buf, sizeof(buf), 0, true);
        if (n_piece < 0) break;
        text.append(buf, static_cast<std::size_t>(n_piece));
        batch = llama_batch_get_one(&id, 1);
    }
    llama_sampler_free(smpl);
    llama_free(ctx);
    return {text, "llama.cpp", ""};
}
#endif

struct LlamaEngine {
    Config cfg;
    std::string backend = "none";
    explicit LlamaEngine(Config c) : cfg(std::move(c)) { bind(); }
    void bind() {
        if (http_ok(cfg.llama_server + "/health") || http_ok(cfg.llama_server + "/v1/models")) {
            backend = "llama-server";
            return;
        }
        if (!cfg.ollama_host.empty() && http_ok(cfg.ollama_host + "/api/tags")) {
            backend = "ollama-llamacpp";
            return;
        }
#ifdef PRISM_HAS_LLAMA
        if (!cfg.gguf.empty() && fs::exists(cfg.gguf)) {
            backend = "llama.cpp";
            return;
        }
#endif
        backend = "none";
    }
    bool available() const { return backend != "none"; }
    ChatResult complete(const std::vector<std::pair<std::string, std::string>>& messages, double timeout = 180.0) {
        if (backend == "none")
            return {"", "none", "llama.cpp not loaded and Ollama not reachable"};
#ifdef PRISM_HAS_LLAMA
        if (backend == "llama.cpp") return native_llama_complete(cfg, messages);
#endif
        nlohmann::json msgs = nlohmann::json::array();
        for (auto& [role, content] : messages) msgs.push_back({{"role", role}, {"content", content}});
        int ms = static_cast<int>(timeout * 1000);
        if (backend == "llama-server") {
            nlohmann::json body{{"model", cfg.model}, {"messages", msgs}, {"temperature", 0.2}};
            auto resp = http_request("POST", cfg.llama_server + "/v1/chat/completions", body.dump(), ms);
            if (!resp) return {"", "llama-server", "HTTP error"};
            try {
                auto raw = nlohmann::json::parse(*resp);
                std::string text = raw.value("choices", nlohmann::json::array()).empty()
                                       ? ""
                                       : raw["choices"][0]["message"].value("content", "");
                return {text, "llama-server", ""};
            } catch (const std::exception& ex) {
                return {"", "llama-server", ex.what()};
            }
        }
        nlohmann::json body{{"model", cfg.model},
                            {"messages", msgs},
                            {"stream", false},
                            {"think", false},
                            {"options", {{"temperature", 0.2}, {"num_ctx", 8192}}}};
        auto resp = http_request("POST", cfg.ollama_host + "/api/chat", body.dump(), ms);
        if (!resp) return {"", "ollama-llamacpp", "HTTP error"};
        try {
            auto raw = nlohmann::json::parse(*resp);
            std::string text = raw.value("message", nlohmann::json::object()).value("content", "");
            return {text, "ollama-llamacpp", ""};
        } catch (const std::exception& ex) {
            return {"", "ollama-llamacpp", ex.what()};
        }
    }
};

bool llama_engine_up(LlamaEngine* e) { return e && e->available(); }

std::optional<std::vector<uint8_t>> parse_hex_bytes(std::string h) {
    h = strip(h);
    if (h.size() >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
    if (h.size() % 2 != 0) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(h.size() / 2);
    for (std::size_t i = 0; i < h.size(); i += 2) {
        int a = nibble(h[i]), b = nibble(h[i + 1]);
        if (a < 0 || b < 0) return std::nullopt;
        out.push_back(static_cast<uint8_t>((a << 4) | b));
    }
    return out;
}

int score_prompt_seeds(const std::vector<std::vector<uint8_t>>& seeds, int nbytes) {
    int n = nbytes > 0 ? nbytes : 1;
    std::set<std::vector<uint8_t>> uniq;
    for (auto s : seeds) {
        if (s.empty()) continue;
        if (static_cast<int>(s.size()) > n) s.resize(static_cast<std::size_t>(n));
        while (static_cast<int>(s.size()) < n) s.push_back(0);
        uniq.insert(std::move(s));
    }
    return static_cast<int>(uniq.size());
}

std::vector<std::vector<uint8_t>> json_hex_list(const nlohmann::json& data, const char* key) {
    std::vector<std::vector<uint8_t>> out;
    if (!data.is_object() || !data.contains(key) || !data[key].is_array()) return out;
    for (auto& h : data[key]) {
        auto raw = parse_hex_bytes(h.is_string() ? h.get<std::string>() : std::string{});
        if (raw && !raw->empty()) out.push_back(std::move(*raw));
    }
    return out;
}

std::string documentation_from_comments(const std::string& source) {
    std::vector<std::string> parts;
    auto is_contract = [](const std::string& s) {
        auto l = lower_copy(s);
        return l.find("requires:") != std::string::npos || l.find("ensures:") != std::string::npos ||
               l.find("invariant:") != std::string::npos || l.find("decreases:") != std::string::npos;
    };
    for (std::size_t i = 0; i < source.size();) {
        if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '*') {
            if (i + 2 < source.size() && source[i + 2] == '@') {
                auto end = source.find("*/", i + 2);
                i = end == std::string::npos ? source.size() : end + 2;
                continue;
            }
            auto end = source.find("*/", i + 2);
            if (end == std::string::npos) break;
            auto inner = source.substr(i + 2, end - (i + 2));
            i = end + 2;
            std::string blob;
            std::istringstream ss(inner);
            std::string ln;
            while (std::getline(ss, ln)) {
                ln = strip(ln);
                while (!ln.empty() && ln.front() == '*') ln = strip(ln.substr(1));
                if (!blob.empty() && !ln.empty()) blob.push_back(' ');
                blob += ln;
            }
            blob = strip(blob);
            if (!blob.empty() && !is_contract(blob)) parts.push_back(blob);
            continue;
        }
        if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '/') {
            auto eol = source.find('\n', i);
            auto line = strip(source.substr(i + 2, (eol == std::string::npos ? source.size() : eol) - (i + 2)));
            i = eol == std::string::npos ? source.size() : eol + 1;
            if (!line.empty() && !is_contract(line)) parts.push_back(line);
            continue;
        }
        ++i;
    }
    std::string out;
    for (std::size_t k = 0; k < parts.size() && k < 8; ++k) {
        if (!out.empty()) out.push_back('\n');
        out += parts[k];
    }
    return out;
}

std::string fuzz4all_autoprompt_text(LlamaEngine& engine, const FunctionInfo& fn) {
    if (!engine.available()) return {};
    auto src_file = read_fn_source(fn);
    auto docs = documentation_from_comments(src_file);
    auto src = docs.empty() ? (fn.signature + "\n{" + fn.body + "\n}") : docs;
    if (src.size() > 6000) src.resize(6000);
    auto r = engine.complete({{"system", AP_SYSTEM_MESSAGE}, {"user", src + "\n" + AP_INSTRUCTION}}, 90.0);
    if (!r.error.empty()) return {};
    auto t = strip(r.text);
    if (t.size() > 2000) t.resize(2000);
    return t;
}

std::string fuzz4all_update_strategy(const std::string& new_hex, const std::string& prev_hex, int strategy) {
    if (strategy == 0) return "seed=" + new_hex + "\ngenerate a new encoding";
    if (strategy == 1) return "seed=" + new_hex + "\nmutate the previous generation";
    if (strategy == 2) return "seed=" + new_hex + "\nsemantically equivalent encoding";
    if (!prev_hex.empty())
        return "prev=" + prev_hex + "\nseed=" + new_hex + "\ncombine the two previous encodings";
    return "seed=" + new_hex + "\nmutate the previous generation";
}

std::vector<std::vector<uint8_t>> fuzz4all_seeds(LlamaEngine& engine, const FunctionInfo& fn, int nbytes) {
    if (!engine.available()) return {};
    auto user = "nbytes=" + std::to_string(nbytes) + "\n" + fn.signature + "\n{" + fn.body + "\n}";
    auto r = engine.complete({{"system", SYSTEM_FUZZ4ALL}, {"user", user}}, 90.0);
    auto data = r.error.empty() ? extract_json(r.text) : nlohmann::json(nullptr);
    auto seeds_a = json_hex_list(data, "seeds");
    int best_sc = score_prompt_seeds(seeds_a, nbytes);
    auto best = seeds_a;
    auto distilled = fuzz4all_autoprompt_text(engine, fn);
    if (!distilled.empty()) {
        auto r2 = engine.complete(
            {{"system", std::string(SYSTEM_FUZZ4ALL) + "\n" + distilled}, {"user", user}}, 90.0);
        auto data2 = r2.error.empty() ? extract_json(r2.text) : nlohmann::json(nullptr);
        auto seeds_b = json_hex_list(data2, "seeds");
        if (score_prompt_seeds(seeds_b, nbytes) > best_sc) best = std::move(seeds_b);
    }
    return best;
}

std::vector<std::vector<uint8_t>> fuzz4all_mutate_interesting(LlamaEngine& engine, const FunctionInfo& fn,
                                                              const std::string& seed_hex,
                                                              const std::string& prev_hex, int strategy) {
    if (!engine.available()) return {};
    auto user = fuzz4all_update_strategy(seed_hex, prev_hex, strategy);
    user += "\n" + fn.signature + "\n{" + fn.body + "\n}";
    const char* sys = (strategy == 3 && !prev_hex.empty()) ? SYSTEM_FUZZ4ALL_COMBINE : SYSTEM_FUZZ4ALL_MUTATE;
    auto r = engine.complete({{"system", sys}, {"user", user}}, 90.0);
    if (!r.error.empty()) return {};
    return json_hex_list(extract_json(r.text), "mutants");
}

std::vector<std::vector<uint8_t>> fuzz4all_combine(LlamaEngine& engine, const FunctionInfo& fn,
                                                   const std::string& seed_hex, const std::string& prev_hex) {
    return fuzz4all_mutate_interesting(engine, fn, seed_hex, prev_hex, 3);
}

std::vector<std::vector<uint8_t>> chatfuzz_mutants(LlamaEngine& engine, const FunctionInfo& fn,
                                                   const std::string& seed_hex) {
    if (!engine.available()) return {};
    auto r = engine.complete({{"system", SYSTEM_CHATFUZZ},
                              {"user", "seed=" + seed_hex + "\n" + fn.signature + "\n{" + fn.body + "\n}"}},
                             90.0);
    if (!r.error.empty()) return {};
    auto data = extract_json(r.text);
    std::vector<std::vector<uint8_t>> out;
    if (!data.is_object() || !data.contains("mutants") || !data["mutants"].is_array()) return out;
    for (auto& h : data["mutants"]) {
        auto raw = parse_hex_bytes(h.is_string() ? h.get<std::string>() : std::string{});
        if (raw) out.push_back(std::move(*raw));
    }
    return out;
}

// Law 9: the program is LLM-written; it runs only with --allow-exec and then
// inside the sandbox (bubblewrap when available, rlimits always).
nlohmann::json sandbox_run(const std::string& source, double timeout = 8.0, bool allow_exec = false) {
    if (!allow_exec)
        return {{"ok", false}, {"error", "exec-disabled"}, {"stdout", ""}, {"stderr", ""}, {"code", nullptr}};
    auto cc = which_cc();
    if (!cc) return {{"ok", false}, {"error", "no compiler"}, {"stdout", ""}, {"stderr", ""}, {"code", nullptr}};
    auto td = fs::temp_directory_path() / ("prism_oci_" + std::to_string(std::random_device{}()));
    fs::create_directories(td);
    struct Guard {
        fs::path p;
        ~Guard() {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
    } guard{td};
    auto src = td / "prog.c";
    {
        std::ofstream out(src);
        out << source;
    }
#ifdef _WIN32
    auto exe = td / "prog.exe";
#else
    auto exe = td / "prog";
#endif
    auto cr = run_argv({cc->string(), "-std=c11", "-O0", "-Wall", src.string(), "-o", exe.string()}, {}, 30.0);
    if (cr.timeout)
        return {{"ok", false}, {"error", "compile-timeout"}, {"stdout", ""}, {"stderr", ""}, {"code", nullptr}};
    if (cr.rc != 0)
        return {{"ok", false},
                {"error", "compile"},
                {"stdout", cr.out.substr(0, 2000)},
                {"stderr", cr.err.substr(0, 2000)},
                {"code", cr.rc}};
    auto rr = run_argv(sandbox::wrap_argv({exe.string()}, td), {}, timeout, sandbox::limits_for(timeout));
    bool ok = rr.rc == 0 && !rr.timeout;
    bool crashed = rr.crashed || rr.rc < 0 || static_cast<unsigned>(rr.rc) >= 0xC0000000u ||
                   rr.err.find("Aborted") != std::string::npos;
    nlohmann::json err = nlohmann::json(nullptr);
    if (!ok) {
        if (rr.timeout) err = "timeout";
        else if (crashed) err = "crash";
        else err = "exit";
    }
    return {{"ok", ok},
            {"error", err},
            {"stdout", rr.out.substr(0, 2000)},
            {"stderr", rr.err.substr(0, 2000)},
            {"code", rr.timeout ? nlohmann::json(nullptr) : nlohmann::json(rr.rc)},
            {"sandbox", sandbox::kind()}};
}

std::string sandbox_err(const nlohmann::json& result) {
    if (!result.contains("error") || result["error"].is_null()) return {};
    if (result["error"].is_string()) return result["error"].get<std::string>();
    return {};
}

std::string sandbox_verdict(const nlohmann::json& result) {
    auto err = sandbox_err(result);
    if (err == "no compiler" || err == "exec-disabled") return std::string(laws::NOTRUN);
    if (result.value("ok", false)) return std::string(laws::CLEAN);
    if (err == "crash") return std::string(laws::CRASH);
    return std::string(laws::FAILED);
}

std::string c_from_llm(std::string src) {
    src = strip(src);
    if (src.empty()) return {};
    auto fence = src.find("```");
    if (fence != std::string::npos) {
        src = src.substr(fence + 3);
        auto nl = src.find('\n');
        if (nl != std::string::npos) src = src.substr(nl + 1);
        auto end = src.rfind("```");
        if (end != std::string::npos) src = src.substr(0, end);
    }
    return strip(src);
}

struct RlefScore {
    int score = 0;
    std::string proved;
};

RlefScore rlef_reward(const nlohmann::json& result, std::string_view bmc_status) {
    RlefScore s;
    auto err = sandbox_err(result);
    if (err != "compile" && err != "compile-timeout" && err != "no compiler") s.score += 1;
    if (result.value("ok", false)) s.score += 2;
    if (err == "crash") s.score -= 1;
    if (laws::is_proof(bmc_status)) {
        s.proved = std::string(bmc_status);
        return s;
    }
    if (bmc_status == laws::FAILED) s.score -= 2;
    return s;
}

std::string bmc_status_of_source(const std::string& source, int unwind = 2) {
    auto src = strip(source);
    if (src.empty()) return {};
    auto td = fs::temp_directory_path() / ("prism_rlef_bmc_" + std::to_string(std::random_device{}()));
    std::error_code ec;
    fs::create_directories(td, ec);
    auto p = td / "cand.c";
    {
        std::ofstream out(p);
        out << src;
    }
    auto fns = extract_functions(p, "cand.c");
    fs::remove_all(td, ec);
    const FunctionInfo* fn = nullptr;
    for (auto& f : fns)
        if (f.kind == "SCALAR") {
            fn = &f;
            break;
        }
    if (!fn) return {};
    auto recs = run_bmc({*fn}, unwind);
    if (recs.empty()) return {};
    auto st = recs[0].status;
    if (st == laws::NOTRUN || st == laws::ERROR || st == laws::TIMEOUT || st == laws::NEEDS_HARNESS ||
        st == laws::UNKNOWN || st == laws::CLEAN)
        return {};
    return st;
}

std::vector<Finding> interpreter_loop(LlamaEngine& engine, const std::string& prompt, int rounds,
                                      bool allow_exec) {
    if (!engine.available()) {
        auto f = nr("execute", LLM_UNAVAILABLE_MSG);
        f.extra["install"] = LLM_INSTALL;
        return {f};
    }
    if (!which_cc()) {
        auto f = nr("execute", "gcc/clang not on PATH", std::string(laws::STRENGTH_FINDS));
        f.extra["install"] = "install gcc or clang";
        return {f};
    }
    if (!allow_exec)
        return {sandbox::exec_notrun("execute", "execute (LLM-written C)", {{"half", "llm"}})};
    std::vector<std::pair<std::string, std::string>> messages{{"system", SYSTEM_HARNESS}, {"user", prompt}};
    std::string last_src;
    bool ran = false;
    for (int i = 0; i < rounds; ++i) {
        auto r = engine.complete(messages, 120);
        if (!r.error.empty()) {
            if (llm_httpish(r.error)) {
                auto f = make_find("execute", laws::NOTRUN, FunctionInfo{}, "", r.error,
                                   laws::STRENGTH_READS);
                f.extra["install"] = LLM_INSTALL;
                f.extra["backend"] = r.backend;
                return {f};
            }
            return {make_find("execute", laws::ERROR, FunctionInfo{}, "", r.error, laws::STRENGTH_READS)};
        }
        auto src = c_from_llm(r.text);
        if (src.empty()) {
            messages.push_back({"assistant", r.text});
            messages.push_back({"user", "Reply with a complete C file only."});
            continue;
        }
        last_src = src;
        auto result = sandbox_run(src, 8.0, /*allow_exec=*/true);
        ran = true;
        auto verdict = sandbox_verdict(result);
        if (verdict == laws::NOTRUN) {
            auto f = nr("execute", "gcc/clang not on PATH", std::string(laws::STRENGTH_FINDS));
            f.extra["install"] = "install gcc or clang";
            return {f};
        }
        if (verdict == laws::CLEAN) {
            Finding f;
            f.stage = "execute";
            f.status = std::string(laws::CLEAN);
            f.message = "interpreter harness passed on round " + std::to_string(i + 1) + " (not a proof)";
            f.strength = std::string(laws::STRENGTH_FINDS);
            f.extra["stdout"] = result.value("stdout", std::string{}).substr(0, 400);
            f.extra["rounds"] = std::to_string(i + 1);
            return {f};
        }
        if (verdict == laws::CRASH) {
            Finding f;
            f.stage = "execute";
            f.status = std::string(laws::CRASH);
            f.message = "sandbox crash on round " + std::to_string(i + 1) + " (not a proof)";
            f.strength = std::string(laws::STRENGTH_FINDS);
            f.extra["stderr"] = result.value("stderr", std::string{}).substr(0, 400);
            f.extra["rounds"] = std::to_string(i + 1);
            return {f};
        }
        messages.push_back({"assistant", r.text});
        messages.push_back({"user", "execution failed: " + result.dump().substr(0, 1500) + ". Fix the C file."});
    }
    if (!ran) {
        Finding f;
        f.stage = "execute";
        f.status = std::string(laws::HYPOTHESIS);
        f.message = "LLM produced no C to run";
        f.strength = std::string(laws::STRENGTH_READS);
        return {f};
    }
    Finding f;
    f.stage = "execute";
    f.status = std::string(laws::FAILED);
    f.message = "interpreter loop exhausted (" + std::to_string(rounds) + " rounds)";
    f.strength = std::string(laws::STRENGTH_FINDS);
    f.extra["last_src"] = last_src.substr(0, 500);
    return {f};
}

constexpr int DEFAULT_LO = -256;
constexpr int DEFAULT_HI = 256;

struct Bound {
    int lo = DEFAULT_LO;
    int hi = DEFAULT_HI;
    std::set<int> neq;
};
bool unsigned_typ(std::string t) {
    t = lower_copy(t);
    for (char& c : t)
        if (c == '*') c = ' ';
    t = strip(t);
    return t.find("unsigned") != std::string::npos || t.starts_with("uint") || t == "size_t" || t == "_bool" ||
           t == "bool";
}
void apply_atom(std::map<std::string, Bound>& bounds, const std::string& atom) {
    static Regex req("^([A-Za-z_]\\w*)\\s*(<|>=|!=)\\s*(0|[1-9]\\d*)$");
    static Regex extra("^([A-Za-z_]\\w*)\\s*(<=|>|==)\\s*(0|[1-9]\\d*|-?[1-9]\\d*)$");
    auto s = strip(atom);
    auto m = match_at(req, s);
    if (!m) m = match_at(extra, s);
    if (!m) return;
    auto name = m->group(1);
    auto op = m->group(2);
    int raw = std::stoi(m->group(3));
    if (!bounds.contains(name)) bounds[name] = {};
    auto& b = bounds[name];
    if (op == "<") b.hi = std::min(b.hi, raw - 1);
    else if (op == "<=") b.hi = std::min(b.hi, raw);
    else if (op == ">") b.lo = std::max(b.lo, raw + 1);
    else if (op == ">=") b.lo = std::max(b.lo, raw);
    else if (op == "!=") b.neq.insert(raw);
    else if (op == "==") {
        b.lo = std::max(b.lo, raw);
        b.hi = std::min(b.hi, raw);
    }
}
int pick_bound(std::mt19937& rng, const Bound& b, const std::vector<int>* interesting) {
    if (interesting) {
        std::vector<int> cand;
        for (int v : *interesting)
            if (v >= b.lo && v <= b.hi && !b.neq.contains(v)) cand.push_back(v);
        if (!cand.empty()) {
            std::uniform_int_distribution<int> d(0, static_cast<int>(cand.size()) - 1);
            return cand[static_cast<std::size_t>(d(rng))];
        }
    }
    for (int i = 0; i < 64; ++i) {
        std::uniform_int_distribution<int> d(b.lo, b.hi);
        int v = d(rng);
        if (!b.neq.contains(v)) return v;
    }
    for (int v = b.lo; v <= b.hi; ++v)
        if (!b.neq.contains(v)) return v;
    throw std::runtime_error("empty domain");
}

struct RapidPlan {
    Spec spec;
    std::vector<std::string> requires_;
    std::vector<std::string> ensures;
    std::vector<Args> samples;
    std::map<std::string, Bound> bounds;
    std::string error;
    int trials = 0;
};

std::optional<RapidPlan> plan_trials(const FunctionInfo& fn, int trials) {
    if (fn.kind != "SCALAR") return std::nullopt;
    auto spec = spec_comments_rapid(fn);
    if (spec.ensures.empty()) return std::nullopt;
    RapidPlan plan;
    plan.spec = spec;
    plan.requires_ = spec.requires_;
    plan.ensures = spec.ensures;
    plan.trials = trials;
    try {
        std::map<std::string, Bound> bounds;
        for (auto& [typ, name] : fn.params) {
            if (name.empty()) continue;
            Bound b;
            if (unsigned_typ(typ)) b.lo = 0;
            bounds[name] = b;
        }
        for (auto& atom : plan.requires_) apply_atom(bounds, atom);
        for (auto& [name, b] : bounds) {
            if (b.lo > b.hi) throw std::runtime_error("empty domain for " + name + " after requires");
            int viable = b.hi - b.lo + 1;
            for (int x : b.neq)
                if (x >= b.lo && x <= b.hi) --viable;
            if (viable <= 0) throw std::runtime_error("empty domain for " + name + " after requires");
        }
        std::string seed_s = fn.file + ":" + fn.name + ":" + std::to_string(trials);
        uint32_t seed = 2166136261u;
        for (unsigned char c : seed_s) seed = (seed ^ c) * 16777619u;
        std::mt19937 rng(seed);
        std::vector<int> interesting{0, 1, -1, 2, -2, 42, 99, 100, 127, -128, 255, -256};
        std::vector<std::string> names;
        for (auto& [t, n] : fn.params)
            if (!n.empty()) names.push_back(n);
        for (int i = 0; i < std::max(0, trials); ++i) {
            Args env;
            for (auto& name : names)
                env[name] = pick_bound(rng, bounds[name], i < 12 ? &interesting : nullptr);
            plan.samples.push_back(env);
        }
        plan.bounds = std::move(bounds);
    } catch (const std::exception& ex) {
        plan.error = ex.what();
    }
    return plan;
}

std::optional<std::string> failing_ensures(const std::vector<std::string>& ensures, const Args& env, int result) {
    auto full = env;
    full["result"] = result;
    static Regex ens("^result\\s*(==|>=)\\s*(.+)$");
    for (auto& clause : ensures) {
        auto s = strip(clause);
        if (auto m = match_at(ens, s)) {
            auto op = m->group(1);
            auto rhs = m->group(2);
            try {
                St st({}, {}, {});
                for (auto& [k, v] : full) st.vars[k] = v;
                auto rv = static_cast<int>(eval_src(st, rhs));
                if (op == "==" && result != rv) return clause;
                if (op == ">=" && !(result >= rv)) return clause;
                continue;
            } catch (...) {
                try {
                    St st({}, {}, {});
                    for (auto& [k, v] : full) st.vars[k] = v;
                    if (!truth(eval_src(st, s))) return clause;
                    continue;
                } catch (...) {
                }
            }
        }
        try {
            St st({}, {}, {});
            for (auto& [k, v] : full) st.vars[k] = v;
            if (!truth(eval_src(st, s))) return clause;
        } catch (...) {
            return clause;
        }
    }
    return std::nullopt;
}

bool env_in_bounds(const Args& env, const std::map<std::string, Bound>& bounds) {
    for (auto& [name, v] : env) {
        auto it = bounds.find(name);
        if (it == bounds.end()) continue;
        auto& b = it->second;
        if (v < b.lo || v > b.hi || b.neq.contains(v)) return false;
    }
    return true;
}

std::vector<int> shrink_cands(int v) {
    std::vector<int> out;
    if (v != 0) out.push_back(0);
    if (v > 1 || v < -1) out.push_back(v / 2);
    if (v > 0) out.push_back(v - 1);
    else if (v < 0) out.push_back(v + 1);
    std::vector<int> uniq;
    std::set<int> seen;
    for (int x : out) {
        if (seen.insert(x).second) uniq.push_back(x);
    }
    return uniq;
}

std::pair<Args, int> shrink_counterexample(const FunctionInfo& fn, Args env,
                                           const std::vector<std::string>& ensures,
                                           const std::map<std::string, Bound>& bounds, bool ub) {
    auto fails = [&](const Args& cand) {
        auto rec = execute(fn, cand);
        if (!rec.error.empty()) return false;
        if (!rec.ub.empty()) return true;
        if (ub) return false;
        int result = rec.value ? static_cast<int>(*rec.value) : 0;
        return failing_ensures(ensures, cand, result).has_value();
    };
    int shrinks = 0;
    bool progress = true;
    while (progress && shrinks < 64) {
        progress = false;
        for (auto& [k, v] : env) {
            for (int c : shrink_cands(v)) {
                auto trial = env;
                trial[k] = c;
                if (trial == env || !env_in_bounds(trial, bounds)) continue;
                if (fails(trial)) {
                    env = std::move(trial);
                    ++shrinks;
                    progress = true;
                    break;
                }
            }
            if (progress) break;
        }
    }
    return {env, shrinks};
}

struct PlanRun {
    bool ok = false;
    std::string error;
    std::string counterexample;
    std::string engine;
    std::string clause;
    std::optional<int> result;
    Args env;
    int n = 0;
    int shrinks = 0;
};

PlanRun run_plan(const FunctionInfo& fn, const RapidPlan& plan) {
    if (!plan.error.empty()) return {false, plan.error, "", "", "", std::nullopt, {}, 0};
    std::vector<std::optional<int>> results;
    std::string ub_err;
    try {
        for (auto& env : plan.samples) {
            auto rec = execute(fn, env);
            if (!rec.error.empty()) {
                ub_err = rec.error;
                break;
            }
            if (!rec.ub.empty()) {
                results.emplace_back(std::nullopt);
                continue;
            }
            results.push_back(rec.value ? static_cast<int>(*rec.value) : 0);
        }
        if (ub_err.empty() && results.size() == plan.samples.size()) {
            for (std::size_t i = 0; i < plan.samples.size(); ++i) {
                if (!results[i]) {
                    auto [env, nsh] = shrink_counterexample(fn, plan.samples[i], plan.ensures, plan.bounds, true);
                    std::string cex;
                    for (auto& [k, v] : env) {
                        if (!cex.empty()) cex += ", ";
                        cex += k + "=" + std::to_string(v);
                    }
                    return {false, "", cex + " -> undefined-behavior", "concrete", "undefined-behavior",
                            std::nullopt, env, 0, nsh};
                }
                auto failed = failing_ensures(plan.ensures, plan.samples[i], *results[i]);
                if (failed) {
                    auto [env, nsh] = shrink_counterexample(fn, plan.samples[i], plan.ensures, plan.bounds, false);
                    auto rec2 = execute(fn, env);
                    std::optional<int> result = results[i];
                    if (rec2.error.empty() && rec2.ub.empty() && rec2.value)
                        result = static_cast<int>(*rec2.value);
                    if (result) {
                        auto again = failing_ensures(plan.ensures, env, *result);
                        if (again) failed = again;
                    }
                    std::string cex;
                    for (auto& [k, v] : env) {
                        if (!cex.empty()) cex += ", ";
                        cex += k + "=" + std::to_string(v);
                    }
                    return {false, "", cex + " -> result=" + std::to_string(result.value_or(0)), "concrete", *failed,
                            result, env, 0, nsh};
                }
            }
            return {true, "", "", "concrete", "", std::nullopt, {}, static_cast<int>(plan.samples.size())};
        }
    } catch (...) {
    }
    return {false, ub_err.empty() ? "cannot evaluate " + fn.name : ub_err, "", "interp", "", std::nullopt, {},
            0};
}

Finding finding_from_plan(const FunctionInfo& fn, const RapidPlan& plan, const std::string& stage) {
    auto extra_req = nlohmann::json(plan.requires_).dump();
    auto extra_ens = nlohmann::json(plan.ensures).dump();
    auto fill = [&](Finding f) {
        f.extra["requires"] = extra_req;
        f.extra["ensures"] = extra_ens;
        f.extra["trials"] = std::to_string(plan.trials);
        f.extra["sampled"] = "true";
        return f;
    };
    auto compiler_missing = [](const std::string& err) {
        auto text = lower_copy(err);
        if (text.find("no gcc") != std::string::npos || text.find("gcc/clang") != std::string::npos)
            return true;
        if (text.find("not on path") != std::string::npos &&
            (text.find("gcc") != std::string::npos || text.find("clang") != std::string::npos ||
             text.find("compiler") != std::string::npos))
            return true;
        Config cfg;
        return !cfg.which({"gcc", "clang"}).has_value();
    };
    if (!plan.error.empty() && plan.samples.empty()) {
        bool missing = compiler_missing(plan.error);
        auto f = fill(make_find(stage, missing ? laws::NOTRUN : laws::ERROR, fn, "", plan.error,
                                laws::STRENGTH_SOME));
        if (missing) f.extra["install"] = "install gcc or clang";
        return f;
    }
    auto info = run_plan(fn, plan);
    if (!info.error.empty() && info.counterexample.empty()) {
        bool missing = compiler_missing(info.error);
        auto f = fill(make_find(stage, missing ? laws::NOTRUN : laws::ERROR, fn, "", info.error,
                                laws::STRENGTH_SOME));
        if (missing) f.extra["install"] = "install gcc or clang";
        return f;
    }
    if (!info.ok) {
        auto clause = info.clause.empty() ? join_sv(plan.ensures, " && ") : info.clause;
        auto f = make_find(stage, laws::FAILED, fn, "FUNC-CONTRACT",
                           "ensures (" + clause + ") failed on " + info.counterexample, laws::STRENGTH_FINDS);
        f.evidence = fn.body.substr(0, 400);
        f.counterexample = info.counterexample;
        f = fill(std::move(f));
        f.extra["engine"] = info.engine;
        // extra.shrinks counts cex minimization; FAILED is a cex, never a proof.
        f.extra["shrinks"] = std::to_string(info.shrinks);
        return f;
    }
    auto ens = join_sv(plan.ensures, " && ");
    int n = info.n ? info.n : static_cast<int>(plan.samples.size());
    auto f = make_find(stage, laws::CLEAN, fn, "",
                       "all " + std::to_string(n) + " trials hold for ensures (" + ens +
                           "); not a proof - sampling is not forall",
                       laws::STRENGTH_SOME);
    f = fill(std::move(f));
    f.extra["engine"] = info.engine;
    return f;
}

std::vector<std::tuple<int, int, std::string, std::string>> iter_mutations(const std::string& body) {
    std::vector<std::tuple<int, int, std::string, std::string>> sites;
    std::size_t i = 0;
    char quote = 0;
    while (i < body.size()) {
        char c = body[i];
        if (quote) {
            if (c == '\\' && i + 1 < body.size()) {
                i += 2;
                continue;
            }
            if (c == quote) quote = 0;
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            ++i;
            continue;
        }
        if (i + 2 <= body.size() && body.compare(i, 2, "==") == 0) {
            char prev = i ? body[i - 1] : '\0';
            if (prev != '=' && prev != '!') sites.emplace_back(static_cast<int>(i), static_cast<int>(i + 2), "==", "!=");
            i += 2;
            continue;
        }
        if (c == '+') {
            char nxt = i + 1 < body.size() ? body[i + 1] : '\0';
            char prev = i ? body[i - 1] : '\0';
            if (nxt != '+' && nxt != '=' && prev != '+')
                sites.emplace_back(static_cast<int>(i), static_cast<int>(i + 1), "+", "-");
            ++i;
            continue;
        }
        if (c == '<') {
            char nxt = i + 1 < body.size() ? body[i + 1] : '\0';
            char prev = i ? body[i - 1] : '\0';
            if (nxt != '<' && nxt != '=' && prev != '<')
                sites.emplace_back(static_cast<int>(i), static_cast<int>(i + 1), "<", ">");
            ++i;
            continue;
        }
        ++i;
    }
    return sites;
}

// --- LTL ---
std::string strip_parens(std::string s) {
    s = strip(s);
    while (s.starts_with("(") && s.ends_with(")")) {
        int depth = 0;
        bool ok = true;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '(') ++depth;
            else if (s[i] == ')') {
                --depth;
                if (depth == 0 && i + 1 != s.size()) {
                    ok = false;
                    break;
                }
            }
        }
        if (!ok || depth != 0) break;
        s = strip(s.substr(1, s.size() - 2));
    }
    return s;
}
std::vector<std::string> split_top(const std::string& s, const std::string& sep) {
    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '(') {
            ++depth;
            cur.push_back(s[i++]);
        } else if (s[i] == ')') {
            --depth;
            cur.push_back(s[i++]);
        } else if (depth == 0 && i + sep.size() <= s.size() && s.compare(i, sep.size(), sep) == 0) {
            parts.push_back(cur);
            cur.clear();
            i += sep.size();
        } else {
            cur.push_back(s[i++]);
        }
    }
    parts.push_back(cur);
    std::vector<std::string> out;
    for (auto& p : parts) {
        auto t = strip(p);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}
bool eval_pred(std::string pred, const std::string& state) {
    pred = strip_parens(strip(pred));
    if (pred.empty()) throw PredFail("empty predicate");
    auto low = lower_copy(pred);
    if (low == "true" || low == "1") return true;
    if (low == "false" || low == "0") return false;
    auto imps = split_top(pred, "->");
    if (imps.size() > 1) {
        std::string rest;
        for (std::size_t i = 1; i < imps.size(); ++i) {
            if (i > 1) rest += "->";
            rest += imps[i];
        }
        return !eval_pred(imps[0], state) || eval_pred(rest, state);
    }
    auto ors = split_top(pred, "||");
    if (ors.size() > 1) {
        for (auto& p : ors)
            if (eval_pred(p, state)) return true;
        return false;
    }
    auto ands = split_top(pred, "&&");
    if (ands.size() > 1) {
        for (auto& p : ands)
            if (!eval_pred(p, state)) return false;
        return true;
    }
    if (pred.starts_with("!")) return !eval_pred(pred.substr(1), state);
    static Regex eq("^state\\s*==\\s*([A-Za-z_]\\w*|\\d+)$");
    if (auto m = match_at(eq, pred)) return state == m->group(1);
    static Regex ne("^state\\s*!=\\s*([A-Za-z_]\\w*|\\d+)$");
    if (auto m = match_at(ne, pred)) {
        auto tokv = m->group(1);
        auto up = tokv;
        for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (up == "BAD" || up == "ERROR") {
            auto su = state;
            for (auto& c : su) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return su.find("BAD") == std::string::npos && su.find("ERROR") == std::string::npos;
        }
        return state != tokv;
    }
    static Regex ident("^[A-Za-z_]\\w*$");
    if (fullmatch(ident, pred)) {
        auto up = pred;
        for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (up == "BAD" || up == "ERROR") {
            auto su = state;
            for (auto& c : su) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return su.find("BAD") != std::string::npos || su.find("ERROR") != std::string::npos;
        }
        return state == pred;
    }
    throw PredFail(pred);
}

struct Fsm {
    std::vector<std::string> states, cases, assigns;
    std::vector<std::pair<std::string, std::string>> transitions;
};

std::optional<Fsm> extract_fsm(const std::string& body) {
    if (body.find("switch") == std::string::npos || body.find("state") == std::string::npos) return std::nullopt;
    static Regex case_re("case\\s+([A-Za-z_]\\w*|\\d+)\\s*:");
    static Regex asg_re("\\bstate\\s*=\\s*([A-Za-z_]\\w*|\\d+)");
    std::vector<std::string> cases;
    for (auto& m : case_re.finditer(body)) cases.push_back(m.group(1));
    std::vector<std::string> assigns;
    for (auto& m : asg_re.finditer(body)) assigns.push_back(m.group(1));
    if (cases.size() < 2) return std::nullopt;
    std::vector<std::pair<std::string, std::string>> trans;
    static Regex split_re("\\bcase\\s+([A-Za-z_]\\w*|\\d+)\\s*:");
    std::vector<std::pair<std::string, std::string>> chunks;
    std::size_t last = 0;
    std::string last_lab;
    bool have = false;
    for (auto& m : split_re.finditer(body)) {
        if (have) chunks.push_back({last_lab, body.substr(last, static_cast<std::size_t>(m.spans[0].first) - last)});
        last_lab = m.group(1);
        last = static_cast<std::size_t>(m.spans[0].second);
        have = true;
    }
    if (have) chunks.push_back({last_lab, body.substr(last)});
    for (auto& [lab, content0] : chunks) {
        auto content = content0;
        auto def = content.find("default");
        static Regex defre("\\bdefault\\s*:");
        if (auto dm = defre.search_match(content))
            content = content.substr(0, static_cast<std::size_t>(dm->spans[0].first));
        std::vector<std::string> dests;
        for (auto& m : asg_re.finditer(content)) dests.push_back(m.group(1));
        if (dests.empty()) trans.emplace_back(lab, lab);
        else {
            for (auto& d : dests) trans.emplace_back(lab, d);
            if (re_search("\\bif\\b", content) && !re_search("\\belse\\b", content)) trans.emplace_back(lab, lab);
        }
    }
    std::set<std::string> stset(cases.begin(), cases.end());
    for (auto& a : assigns) stset.insert(a);
    for (auto& [a, b] : trans) {
        stset.insert(a);
        stset.insert(b);
    }
    std::set<std::string> has_out;
    for (auto& [s, _] : trans) has_out.insert(s);
    std::vector<std::string> states(stset.begin(), stset.end());
    for (auto& s : states)
        if (!has_out.contains(s)) trans.emplace_back(s, s);
    return Fsm{states, cases, assigns, trans};
}

enum class LtlKind {
    Invariant, Next, BoundedF, Nonsafety, GfApprox, FgApprox, UntilApprox, FApprox
};
struct LtlClass {
    LtlKind kind = LtlKind::Nonsafety;
    std::string a, b;
    int k = 0;
    std::string raw;
};

constexpr std::string_view kStrixNote =
    "strix realizability is not PROVED unless the formula is in PRISM's "
    "safety fragment (G p, G (p -> X q), G (req -> F_k ack), G (F_k p))";

bool ltl_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

const Regex& ltl_until_re() {
    static Regex until("(?<![A-Za-z_])U(?![A-Za-z_])");
    return until;
}
const Regex& ltl_gflive_re() {
    static Regex gflive("\\bG\\s*F\\b|\\bF\\s*G\\b|\\bGF\\b|\\bFG\\b");
    return gflive;
}

std::optional<std::pair<std::string, std::string>> split_until(const std::string& s) {
    int depth = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') --depth;
        else if (depth == 0 && s[i] == 'U') {
            bool prev_ok = i == 0 || !ltl_word_char(s[i - 1]);
            bool next_ok = i + 1 >= s.size() || !ltl_word_char(s[i + 1]);
            if (prev_ok && next_ok) {
                auto left = strip(s.substr(0, i));
                auto right = strip(s.substr(i + 1));
                if (!left.empty() && !right.empty() && !ltl_until_re().search(left) &&
                    !ltl_until_re().search(right))
                    return std::pair{left, right};
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

std::optional<LtlClass> g_f_inner(const std::string& inner0, const std::string& raw) {
    auto inner = strip_parens(inner0);
    if (split_top(inner, "->").size() == 2) return std::nullopt;
    static Regex fm("^F(?:_(\\d+))?\\s*(.+)$", false, true);
    auto m = match_at(fm, inner);
    if (!m) return std::nullopt;
    auto rest = strip_parens(strip(m->group(2)));
    if (rest.empty() || ltl_until_re().search(rest) || ltl_gflive_re().search(rest))
        return std::nullopt;
    if (!m->group(1).empty())
        return LtlClass{LtlKind::BoundedF, "true", rest, std::stoi(m->group(1)), raw};
    return LtlClass{LtlKind::GfApprox, rest, {}, F_BOUND, raw};
}

std::optional<LtlClass> liveness_approx(const std::string& raw) {
    auto s = strip_parens(strip(raw));
    static Regex gf("^(?:G\\s*F|GF)\\s*(.+)$");
    if (auto m = match_at(gf, s)) {
        auto inner = strip_parens(strip(m->group(1)));
        if (!inner.empty() && !ltl_until_re().search(inner) && !ltl_gflive_re().search(inner))
            return LtlClass{LtlKind::GfApprox, inner, {}, F_BOUND, raw};
    }
    static Regex fg("^(?:F\\s*G|FG)\\s*(.+)$");
    if (auto m = match_at(fg, s)) {
        auto inner = strip_parens(strip(m->group(1)));
        if (!inner.empty() && !ltl_until_re().search(inner) && !ltl_gflive_re().search(inner))
            return LtlClass{LtlKind::FgApprox, inner, {}, F_BOUND, raw};
    }
    static Regex gstart("^G\\b");
    if (match_at(gstart, s)) {
        static Regex safety("^G\\s*\\((.+)\\)$");
        static Regex gbare("^G\\s+(.+)$");
        auto m = match_at(safety, s);
        if (!m) m = match_at(gbare, s);
        if (m) return g_f_inner(strip_parens(m->group(1)), raw);
        return std::nullopt;
    }
    if (auto parts = split_until(s)) {
        if (!ltl_gflive_re().search(parts->first) && !ltl_gflive_re().search(parts->second))
            return LtlClass{LtlKind::UntilApprox, parts->first, parts->second, F_BOUND, raw};
    }
    return std::nullopt;
}

bool is_approx_kind(LtlKind k) {
    return k == LtlKind::GfApprox || k == LtlKind::FgApprox || k == LtlKind::UntilApprox ||
           k == LtlKind::FApprox;
}

LtlClass classify_ltl(const std::string& formula) {
    auto raw = strip(formula);
    if (ltl_until_re().search(raw) || ltl_gflive_re().search(raw)) {
        if (auto a = liveness_approx(raw)) return *a;
        return {LtlKind::Nonsafety, {}, {}, 0, raw};
    }
    static Regex safety("^G\\s*\\((.+)\\)$");
    static Regex gbare("^G\\s+(.+)$");
    auto m = match_at(safety, raw);
    if (!m) m = match_at(gbare, raw);
    if (!m) {
        static Regex fx("^F\\b|^X\\b");
        if (match_at(fx, raw)) {
            if (auto a = liveness_approx(raw)) return *a;
        }
        return {LtlKind::Nonsafety, {}, {}, 0, raw};
    }
    auto inner = strip_parens(m->group(1));
    auto parts = split_top(inner, "->");
    if (parts.size() == 2) {
        auto lhs = strip(parts[0]);
        auto rhs = strip(parts[1]);
        static Regex xm("^X\\s*\\(?(.+?)\\)?$", false, true);
        if (auto xm_ = match_at(xm, rhs))
            return {LtlKind::Next, lhs, strip_parens(xm_->group(1)), 0, raw};
        static Regex fm("^F(?:_(\\d+))?\\s*\\(?(.+?)\\)?$", false, true);
        if (auto fm_ = match_at(fm, rhs)) {
            auto ack = strip_parens(fm_->group(2));
            if (!fm_->group(1).empty())
                return {LtlKind::BoundedF, lhs, ack, std::stoi(fm_->group(1)), raw};
            return {LtlKind::FApprox, lhs, ack, F_BOUND, raw};
        }
        return {LtlKind::Invariant, inner, {}, 0, raw};
    }
    if (auto gf = g_f_inner(inner, raw)) return *gf;
    static Regex lonef("(?<![A-Za-z_])F(?:_\\d+)?(?![A-Za-z_])");
    if (lonef.search(inner)) return {LtlKind::Nonsafety, {}, {}, 0, raw};
    return {LtlKind::Invariant, inner, {}, 0, raw};
}

Finding ltl_finding(std::string_view status, const std::string& formula, const std::string& message,
                   std::map<std::string, std::string> extra, std::string_view strength = laws::STRENGTH_PROVES) {
    Finding f;
    f.stage = "ltl";
    f.status = std::string(status);
    f.cls = "LTL-SAFETY";
    f.message = message;
    f.strength = std::string(strength);
    extra["formula"] = formula;
    f.extra = std::move(extra);
    return f;
}

Finding check_g(const std::string& pred, const Fsm& fsm, const std::string& formula) {
    std::vector<std::string> bad;
    for (auto& s : fsm.states)
        if (!eval_pred(pred, s)) bad.push_back(s);
    if (!bad.empty()) {
        std::set<std::string> live(fsm.assigns.begin(), fsm.assigns.end());
        live.insert(fsm.cases.begin(), fsm.cases.end());
        std::vector<std::string> live_bad;
        if (!live.empty()) {
            for (auto& s : bad)
                if (live.contains(s)) live_bad.push_back(s);
        } else {
            live_bad = bad;
        }
        if (!live_bad.empty()) {
            bool errst = false;
            for (auto& x : live_bad) {
                auto u = x;
                for (auto& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (u.find("BAD") != std::string::npos || u.find("ERROR") != std::string::npos) errst = true;
            }
            std::string msg = errst ? "formula " + formula + " violated: FSM assigns an error state"
                                    : "formula " + formula + " violated on states " + join_sv(live_bad, ", ");
            return ltl_finding(laws::FAILED, formula, msg, {});
        }
    }
    return ltl_finding(laws::PROVED, formula, "safety " + formula + " holds on extracted FSM", {});
}

std::optional<Finding> synthesize_missing(const Fsm& fsm, const std::string& formula) {
    auto cl = classify_ltl(formula);
    if (cl.kind != LtlKind::Next) return std::nullopt;
    try {
        for (auto& [s, sp] : fsm.transitions)
            if (eval_pred(cl.a, s) && !eval_pred(cl.b, sp)) return std::nullopt;
        std::map<std::string, std::vector<std::string>> succ;
        for (auto& [s, sp] : fsm.transitions) succ[s].push_back(sp);
        std::vector<std::string> incomplete, q_states;
        std::vector<std::pair<std::string, std::string>> synthesis;
        for (auto& t : fsm.states)
            if (eval_pred(cl.b, t)) q_states.push_back(t);
        for (auto& s : fsm.states) {
            if (!eval_pred(cl.a, s)) continue;
            auto& outs = succ[s];
            bool any = false;
            for (auto& sp : outs)
                if (eval_pred(cl.b, sp)) any = true;
            if (outs.empty() || !any) {
                incomplete.push_back(s);
                for (auto& t : q_states) synthesis.emplace_back(s, t);
            }
        }
        if (incomplete.empty()) return std::nullopt;
        nlohmann::json syn = nlohmann::json::array();
        for (std::size_t i = 0; i < synthesis.size() && i < 24; ++i)
            syn.push_back(nlohmann::json::array({synthesis[i].first, synthesis[i].second}));
        std::vector<std::string> inc = incomplete;
        if (inc.size() > 12) inc.resize(12);
        auto msg_inc = incomplete;
        if (msg_inc.size() > 6) msg_inc.resize(6);
        return ltl_finding(laws::HYPOTHESIS, formula,
                           "missing transition from " + join_sv(msg_inc, ", ") +
                               " to a state satisfying q; synthesis is HYPOTHESIS",
                           {{"synthesis", syn.dump()}, {"incomplete", nlohmann::json(inc).dump()}},
                           laws::STRENGTH_READS);
    } catch (const PredFail&) {
        return std::nullopt;
    }
}

Finding check_next(const std::string& p, const std::string& q, const Fsm& fsm, const std::string& formula) {
    std::vector<std::pair<std::string, std::string>> viol;
    for (auto& [s, sp] : fsm.transitions)
        if (eval_pred(p, s) && !eval_pred(q, sp)) viol.emplace_back(s, sp);
    if (!viol.empty()) {
        nlohmann::json j = nlohmann::json::array();
        for (std::size_t i = 0; i < viol.size() && i < 12; ++i)
            j.push_back(nlohmann::json::array({viol[i].first, viol[i].second}));
        std::string vs;
        for (std::size_t i = 0; i < viol.size() && i < 6; ++i) {
            if (i) vs += ", ";
            vs += viol[i].first + "->" + viol[i].second;
        }
        return ltl_finding(laws::FAILED, formula, "formula " + formula + " violated on transitions " + vs,
                           {{"violations", j.dump()}});
    }
    if (auto syn = synthesize_missing(fsm, formula)) return *syn;
    return ltl_finding(laws::PROVED, formula, "safety " + formula + " holds on extracted FSM", {});
}

bool avoids_ack(const std::string& start, const std::string& ack,
                const std::map<std::string, std::vector<std::string>>& succ, int k) {
    if (eval_pred(ack, start)) return false;
    std::deque<std::pair<std::string, int>> q;
    std::set<std::pair<std::string, int>> seen;
    q.push_back({start, 0});
    seen.insert({start, 0});
    while (!q.empty()) {
        auto [s, d] = q.front();
        q.pop_front();
        if (d >= k) return true;
        auto it = succ.find(s);
        std::vector<std::string> nxt = it == succ.end() || it->second.empty() ? std::vector<std::string>{s}
                                                                             : it->second;
        bool progressed = false;
        for (auto& sp : nxt) {
            if (eval_pred(ack, sp)) {
                progressed = true;
                continue;
            }
            progressed = true;
            auto key = std::pair<std::string, int>{sp, d + 1};
            if (!seen.contains(key)) {
                seen.insert(key);
                q.push_back(key);
            }
        }
        if (!progressed && d < k) return true;
    }
    return false;
}

Finding check_bounded_f(const std::string& req, const std::string& ack, int k, const Fsm& fsm,
                        const std::string& formula) {
    std::map<std::string, std::vector<std::string>> succ;
    for (auto& [s, sp] : fsm.transitions) succ[s].push_back(sp);
    for (auto& s : fsm.states)
        if (!succ.contains(s)) succ[s] = {s};
    std::vector<std::string> viol;
    for (auto& s : fsm.states) {
        if (!eval_pred(req, s)) continue;
        if (avoids_ack(s, ack, succ, k)) viol.push_back(s);
    }
    if (!viol.empty())
        return ltl_finding(laws::FAILED, formula,
                           "formula " + formula + " violated: from " + join_sv(viol, ", ") +
                               " ack is avoidable within F_" + std::to_string(k),
                           {});
    return ltl_finding(laws::PROVED, formula,
                       "safety " + formula + " holds on extracted FSM (F bound " + std::to_string(k) + ")",
                       {{"k", std::to_string(k)}});
}

std::map<std::string, std::vector<std::string>> ltl_succ_map(const Fsm& fsm) {
    std::map<std::string, std::vector<std::string>> succ;
    for (auto& [s, sp] : fsm.transitions) succ[s].push_back(sp);
    for (auto& s : fsm.states)
        if (!succ.contains(s)) succ[s] = {s};
    return succ;
}

bool invariant_from(const std::string& pred, const std::string& start,
                    const std::map<std::string, std::vector<std::string>>& succ) {
    std::set<std::string> seen;
    std::vector<std::string> stack{start};
    while (!stack.empty()) {
        auto s = stack.back();
        stack.pop_back();
        if (seen.contains(s)) continue;
        seen.insert(s);
        if (!eval_pred(pred, s)) return false;
        auto it = succ.find(s);
        auto nxt = it == succ.end() || it->second.empty() ? std::vector<std::string>{s} : it->second;
        stack.insert(stack.end(), nxt.begin(), nxt.end());
    }
    return true;
}

bool avoids_states(const std::string& start, const std::set<std::string>& targets,
                   const std::map<std::string, std::vector<std::string>>& succ, int k) {
    if (targets.contains(start)) return false;
    std::deque<std::pair<std::string, int>> q;
    std::set<std::pair<std::string, int>> seen;
    q.push_back({start, 0});
    seen.insert({start, 0});
    while (!q.empty()) {
        auto [s, d] = q.front();
        q.pop_front();
        if (d >= k) return true;
        auto it = succ.find(s);
        auto nxt = it == succ.end() || it->second.empty() ? std::vector<std::string>{s} : it->second;
        bool progressed = false;
        for (auto& sp : nxt) {
            progressed = true;
            if (targets.contains(sp)) continue;
            auto key = std::pair<std::string, int>{sp, d + 1};
            if (!seen.contains(key)) {
                seen.insert(key);
                q.push_back(key);
            }
        }
        if (!progressed && d < k) return true;
    }
    return false;
}

Finding approx_finding(Finding f, const std::string& original, const std::string& approx,
                       const std::string& kind) {
    f.extra["formula"] = original;
    f.extra["safety_approx"] = approx;
    f.extra["approx_kind"] = kind;
    f.extra["strix_not_proved"] = "true";
    f.extra["strix_note"] = std::string(kStrixNote);
    f.extra["approx_note"] =
        "safety approximation of unbounded " + kind + "; not PROVED of the original formula";
    if (f.status == laws::PROVED) {
        f.status = std::string(laws::BOUNDED);
        f.message = "safety approximation " + approx + " of " + original +
                    " holds on extracted FSM; not a proof of unbounded " + kind;
    } else if (f.status == laws::FAILED) {
        f.message = "safety approximation " + approx + " of " + original + " violated";
    }
    return f;
}

Finding check_fg_approx(const std::string& pred, int k, const Fsm& fsm, const std::string& formula) {
    auto succ = ltl_succ_map(fsm);
    std::set<std::string> good;
    for (auto& s : fsm.states)
        if (invariant_from(pred, s, succ)) good.insert(s);
    std::vector<std::string> viol;
    for (auto& s : fsm.states)
        if (avoids_states(s, good, succ, k)) viol.push_back(s);
    auto approx = "F_" + std::to_string(k) + " (G (" + pred + "))";
    std::map<std::string, std::string> extra{
        {"safety_approx", approx},
        {"approx_kind", "FG"},
        {"k", std::to_string(k)},
        {"strix_not_proved", "true"},
        {"strix_note", std::string(kStrixNote)},
        {"approx_note", "safety approximation of unbounded FG; not PROVED of the original formula"},
    };
    if (!viol.empty())
        return ltl_finding(laws::FAILED, formula,
                           "safety approximation " + approx + " of " + formula + " violated from " +
                               join_sv(viol, ", "),
                           extra);
    return ltl_finding(laws::BOUNDED, formula,
                       "safety approximation " + approx + " of " + formula +
                           " holds on extracted FSM; not a proof of unbounded FG",
                       extra);
}

std::string until_from(const std::string& start, const std::string& p, const std::string& q,
                       const std::map<std::string, std::vector<std::string>>& succ, int k) {
    if (eval_pred(q, start)) return "ok";
    if (!eval_pred(p, start)) return "real";
    std::deque<std::pair<std::string, int>> dq;
    std::set<std::pair<std::string, int>> seen;
    dq.push_back({start, 0});
    seen.insert({start, 0});
    bool saw_bound = false;
    while (!dq.empty()) {
        auto [s, d] = dq.front();
        dq.pop_front();
        if (d >= k) {
            saw_bound = true;
            continue;
        }
        auto it = succ.find(s);
        auto nxt = it == succ.end() || it->second.empty() ? std::vector<std::string>{s} : it->second;
        for (auto& sp : nxt) {
            if (eval_pred(q, sp)) continue;
            if (!eval_pred(p, sp)) return "real";
            auto key = std::pair<std::string, int>{sp, d + 1};
            if (!seen.contains(key)) {
                seen.insert(key);
                dq.push_back(key);
            }
        }
    }
    return saw_bound ? "bound" : "ok";
}

Finding check_until_approx(const std::string& p, const std::string& q, int k, const Fsm& fsm,
                           const std::string& formula) {
    auto succ = ltl_succ_map(fsm);
    std::vector<std::string> real, bound;
    for (auto& s : fsm.states) {
        auto hit = until_from(s, p, q, succ, k);
        if (hit == "real") real.push_back(s);
        else if (hit == "bound") bound.push_back(s);
    }
    auto approx = "(" + p + ") U_" + std::to_string(k) + " (" + q + ")";
    std::map<std::string, std::string> extra{
        {"safety_approx", approx},
        {"approx_kind", "UNTIL"},
        {"k", std::to_string(k)},
        {"strix_not_proved", "true"},
        {"strix_note", std::string(kStrixNote)},
        {"approx_note", "safety approximation of unbounded U; not PROVED of the original formula"},
    };
    if (!real.empty())
        return ltl_finding(laws::FAILED, formula,
                           "formula " + formula + " violated: " + p + " U " + q + " fails from " +
                               join_sv(real, ", ") + " (\xC2\xACp \xE2\x88\xA7 \xC2\xACq before q)",
                           extra);
    if (!bound.empty())
        return ltl_finding(laws::FAILED, formula,
                           "safety approximation " + approx + " of " + formula + " violated from " +
                               join_sv(bound, ", "),
                           extra);
    return ltl_finding(laws::BOUNDED, formula,
                       "safety approximation " + approx + " of " + formula +
                           " holds on extracted FSM; not a proof of unbounded U",
                       extra);
}

std::optional<Finding> check_safety(const std::string& formula, const Fsm& fsm) {
    auto cl = classify_ltl(formula);
    try {
        if (cl.kind == LtlKind::Invariant) return check_g(cl.a, fsm, formula);
        if (cl.kind == LtlKind::Next) return check_next(cl.a, cl.b, fsm, formula);
        if (cl.kind == LtlKind::BoundedF) return check_bounded_f(cl.a, cl.b, cl.k, fsm, formula);
        if (cl.kind == LtlKind::FApprox) {
            auto f = check_bounded_f(cl.a, cl.b, cl.k, fsm, formula);
            return approx_finding(f, formula,
                                  "G ((" + cl.a + ") -> F_" + std::to_string(cl.k) + " (" + cl.b + "))",
                                  "F");
        }
        if (cl.kind == LtlKind::GfApprox) {
            auto f = check_bounded_f("true", cl.a, cl.k, fsm, formula);
            return approx_finding(f, formula, "G (F_" + std::to_string(cl.k) + " (" + cl.a + "))", "GF");
        }
        if (cl.kind == LtlKind::FgApprox) return check_fg_approx(cl.a, cl.k, fsm, formula);
        if (cl.kind == LtlKind::UntilApprox) return check_until_approx(cl.a, cl.b, cl.k, fsm, formula);
    } catch (const PredFail&) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::vector<std::string> parse_ltl_file(const fs::path& path) {
    if (!fs::exists(path)) return {};
    std::vector<std::string> out;
    std::istringstream ss(read_text_file(path));
    std::string ln;
    while (std::getline(ss, ln)) {
        ln = strip(ln);
        if (ln.empty() || ln.starts_with("#") || ln.starts_with("//")) continue;
        out.push_back(ln);
    }
    return out;
}

std::vector<std::string> taint_split_args(const std::string& argtext) {
    std::vector<std::string> args;
    int depth = 0;
    std::string cur;
    for (char ch : argtext) {
        if (ch == '(') {
            ++depth;
            cur.push_back(ch);
        } else if (ch == ')') {
            depth = std::max(0, depth - 1);
            cur.push_back(ch);
        } else if (ch == ',' && depth == 0) {
            args.push_back(strip(cur));
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    auto tail = strip(cur);
    if (!tail.empty()) args.push_back(tail);
    return args;
}
std::vector<int> sink_arg_indices(const std::string& name) {
    if (name == "system" || name == "popen" || name == "execl" || name == "execv" || name == "printf")
        return {0};
    if (name == "sprintf" || name == "fprintf") return {1};
    if (name == "strcpy" || name == "memcpy" || name == "strcat" || name == "strncat") return {0, 1};
    return {0};
}
bool arg_is_tainted(const std::string& arg, const std::set<std::string>& tainted) {
    static Regex str_re(R"("([^"\\]|\\.)*")");
    std::string stripped;
    std::size_t i = 0;
    for (auto& m : str_re.finditer(arg)) {
        if (m.spans.empty()) continue;
        auto a = static_cast<std::size_t>(std::max(0, m.spans[0].first));
        auto b = static_cast<std::size_t>(std::max(0, m.spans[0].second));
        if (a < i) continue;
        stripped.append(arg, i, a - i);
        i = b;
    }
    stripped.append(arg, i, std::string::npos);
    static Regex ident("\\b([A-Za-z_]\\w*)\\b");
    for (auto& m : ident.finditer(stripped))
        if (tainted.contains(m.group(1))) return true;
    return false;
}

std::vector<std::pair<int, std::string>> iter_statements(const std::string& body, int base_line) {
    std::vector<std::string> lines;
    {
        std::istringstream ss(body);
        std::string ln;
        while (std::getline(ss, ln)) lines.push_back(ln);
    }
    std::vector<std::pair<int, std::string>> out;
    std::vector<std::string> buf;
    int start = 0;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        if (buf.empty()) start = i;
        buf.push_back(lines[static_cast<std::size_t>(i)]);
        auto raw = lines[static_cast<std::size_t>(i)];
        if (raw.find(';') != std::string::npos || strip(raw).ends_with("}")) {
            auto stmt_s = strip(join_sv(buf, " "));
            if (!stmt_s.empty()) out.emplace_back(base_line + start + 1, stmt_s);
            buf.clear();
        }
    }
    auto tail = strip(join_sv(buf, " "));
    if (!tail.empty()) out.emplace_back(base_line + start + 1, tail);
    return out;
}

void mark_source_taint(const std::string& stmt_s, std::set<std::string>& tainted) {
    static const std::vector<std::string> sources{"getenv", "fgets", "gets", "scanf", "recv", "read", "fread", "recvfrom"};
    static Regex asg("(?P<lhs>[A-Za-z_]\\w*(?:\\s*\\[[^\\]]*\\])?)\\s*=\\s*(?P<rhs>[^;]+)");
    for (auto& m : asg.finditer(stmt_s)) {
        auto lhs = m.named("lhs");
        auto br = lhs.find('[');
        if (br != std::string::npos) lhs = strip(lhs.substr(0, br));
        else lhs = strip(lhs);
        auto rhs = m.named("rhs");
        for (auto& src : sources)
            if (re_search("\\b" + src + "\\s*\\(", rhs)) tainted.insert(lhs);
    }
    for (auto& src : sources) {
        Regex call("\\b" + src + "\\s*\\((?P<args>[^)]*)\\)");
        for (auto& m : call.finditer(stmt_s)) {
            auto args = taint_split_args(m.named("args"));
            if (args.empty()) continue;
            auto take = [](std::string a) {
                a = strip(a);
                while (!a.empty() && (a.front() == '&' || a.front() == '*')) a.erase(a.begin());
                auto br = a.find('[');
                if (br != std::string::npos) a = a.substr(0, br);
                return strip(a);
            };
            if (src == "read" && args.size() >= 2) tainted.insert(take(args[1]));
            else if (src == "recv" && args.size() >= 2) tainted.insert(take(args[1]));
            else if (src == "recvfrom" && args.size() >= 2) tainted.insert(take(args[1]));
            else if (src == "fread") tainted.insert(take(args[0]));
            else if (src == "fgets" || src == "gets") tainted.insert(take(args[0]));
            else if (src == "scanf") {
                for (std::size_t i = 1; i < args.size(); ++i) tainted.insert(take(args[i]));
            }
        }
    }
}
void mark_copy_taint(const std::string& stmt_s, std::set<std::string>& tainted) {
    static Regex re("(?P<dst>[A-Za-z_]\\w*)\\s*=\\s*(?:\\([^)]*\\)\\s*)*(?P<src>[A-Za-z_]\\w*)\\s*(?:;|$)");
    auto m = re.search_match(stmt_s);
    if (!m) return;
    if (tainted.contains(m->named("src"))) tainted.insert(m->named("dst"));
}

std::optional<Finding> analyze_taint(const FunctionInfo& fn) {
    std::set<std::string> tainted;
    for (auto& [t, name] : fn.params)
        if (name == "argv" || name == "envp") tainted.insert(name);
    for (auto& [line_no, stmt_s] : iter_statements(fn.body, fn.line)) {
        mark_source_taint(stmt_s, tainted);
        mark_copy_taint(stmt_s, tainted);
        static Regex sink(
            "\\b(?P<name>system|popen|execl|execv|strcpy|sprintf|memcpy|strcat|strncat)\\s*\\((?P<args>[^)]*)\\)");
        for (auto& m : sink.finditer(stmt_s)) {
            auto name = m.named("name");
            auto args = taint_split_args(m.named("args"));
            for (int idx : sink_arg_indices(name)) {
                if (idx >= static_cast<int>(args.size())) continue;
                if (arg_is_tainted(args[static_cast<std::size_t>(idx)], tainted)) {
                    auto f = make_find("taint", laws::FAILED, fn, "TAINT-SINK",
                                       "tainted data reaches " + name + "()", laws::STRENGTH_FINDS);
                    f.line = line_no;
                    f.evidence = strip(stmt_s);
                    return f;
                }
            }
        }
    }
    return std::nullopt;
}

std::set<std::string> file_globals(const std::string& text) {
    int depth = 0;
    std::set<std::string> g;
    static Regex decl(
        "^(?:\\s*(?:static|extern|const|volatile|unsigned|signed|short|long)\\s+)*"
        "(?:(?:struct|enum|union)\\s+\\w+\\s+)?"
        "(?:\\w+\\s+)+(?P<name>[A-Za-z_]\\w*)\\s*(?:=\\s*[^;]+)?;");
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        auto stripped = strip(line);
        if (stripped.empty() || stripped.starts_with("#")) {
            depth += static_cast<int>(std::count(stripped.begin(), stripped.end(), '{') -
                                      std::count(stripped.begin(), stripped.end(), '}'));
            continue;
        }
        if (depth == 0) {
            if (auto m = match_at(decl, line)) g.insert(m->named("name"));
        }
        depth += static_cast<int>(std::count(stripped.begin(), stripped.end(), '{') -
                                  std::count(stripped.begin(), stripped.end(), '}'));
    }
    return g;
}
bool writes_global(const std::string& body, const std::string& name) {
    return re_search("\\b" + name + "\\s*(?:[\\[\\(]|(?:[+\\-*/%&|^]?=))", body);
}

std::string stem_of(const FunctionInfo& fn) {
    return fs::path(fn.file).stem().string();
}

std::vector<std::pair<FunctionInfo, FunctionInfo>> diff_pairs(const std::vector<FunctionInfo>& functions) {
    std::map<std::string, std::vector<FunctionInfo>> by_name;
    for (auto& fn : functions) by_name[fn.name].push_back(fn);
    std::set<const FunctionInfo*> used;
    std::vector<std::pair<FunctionInfo, FunctionInfo>> pairs;
    auto mark = [&](const FunctionInfo& a, const FunctionInfo& b) {
        used.insert(&a);
        used.insert(&b);
        pairs.emplace_back(a, b);
    };
    std::map<std::string, const FunctionInfo*> names;
    for (auto& fn : functions) names[fn.name] = &fn;
    for (auto& fn : functions) {
        if (used.contains(&fn)) continue;
        if (fn.name.ends_with("_a")) {
            auto other = names.find(fn.name.substr(0, fn.name.size() - 2) + "_b");
            if (other != names.end() && !used.contains(other->second)) mark(fn, *other->second);
        }
    }
    for (auto& fn : functions) {
        if (used.contains(&fn)) continue;
        auto d = parse_comments(fn).diff;
        if (!d) continue;
        auto other = names.find(*d);
        if (other != names.end() && !used.contains(other->second)) mark(fn, *other->second);
    }
    for (auto& [nm, group] : by_name) {
        std::vector<const FunctionInfo*> a_fns, b_fns;
        for (auto& f : group) {
            if (used.contains(&f)) continue;
            if (stem_of(f).ends_with("_a")) a_fns.push_back(&f);
            if (stem_of(f).ends_with("_b")) b_fns.push_back(&f);
        }
        for (std::size_t i = 0; i < a_fns.size() && i < b_fns.size(); ++i) mark(*a_fns[i], *b_fns[i]);
    }
    return pairs;
}

std::string emit_diff_program(const FunctionInfo& a, const FunctionInfo& b) {
    std::string args, call;
    for (std::size_t i = 0; i < a.params.size(); ++i) {
        if (i) {
            args += ", ";
            call += ", ";
        }
        auto t = a.params[i].first;
        auto n = a.params[i].second;
        args += (t.empty() ? "int " : t + " ") + n;
        call += n;
    }
    std::string decls, reads;
    int off = 0;
    for (auto& [typ, name] : a.params) {
        auto key = strip(typ);
        if (key.empty()) key = "int";
        int sz = c_type_nbytes_key(key);
        decls += "    " + key + " " + name + ";\n";
        reads += "    memcpy(&" + name + ", buf + " + std::to_string(off) + ", " + std::to_string(sz) + ");\n";
        off += sz;
    }
    int nbytes = off ? off : 1;
    auto strip_star = [](std::string s) {
        s.erase(std::remove(s.begin(), s.end(), '*'), s.end());
        s = strip(s);
        return s.empty() ? "int" : s;
    };
    auto ret_a = strip_star(a.return_type);
    auto ret_b = strip_star(b.return_type);
    return "#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n\nstatic " + ret_a + " impl_a(" +
           args + ") {\n" + a.body + "\n}\nstatic " + ret_b + " impl_b(" + args + ") {\n" + b.body +
           "\n}\n\nint main(void) {\n    unsigned char buf[" + std::to_string(nbytes) +
           "];\n    if (fread(buf, 1, " + std::to_string(nbytes) + ", stdin) != " + std::to_string(nbytes) +
           ") return 0;\n" + decls + reads + "    " + ret_a + " ra = impl_a(" + call + ");\n    " + ret_b +
           " rb = impl_b(" + call +
           ");\n    if (ra != rb) {\n        fprintf(stderr, \"DIFF %d %d\\n\", (int)ra, (int)rb);\n        "
           "return 2;\n    }\n    return 0;\n}\n";
}

std::vector<std::vector<uint8_t>> diff_inputs(int nbytes) {
    std::vector<int> interesting{0,  1,  -1, 2,         3,          42, 99, 100,
                                 127, 128, 255, 0x7FFFFFFF, static_cast<int>(0x80000000), 123456, -99};
    std::vector<std::vector<uint8_t>> out;
    for (int v : interesting) {
        std::vector<uint8_t> raw;
        if (nbytes >= 4) {
            uint32_t u = static_cast<uint32_t>(v);
            raw = {static_cast<uint8_t>(u), static_cast<uint8_t>(u >> 8), static_cast<uint8_t>(u >> 16),
                   static_cast<uint8_t>(u >> 24)};
        } else {
            raw = {static_cast<uint8_t>(v & 0xFF)};
        }
        if (static_cast<int>(raw.size()) > nbytes) raw.resize(static_cast<std::size_t>(nbytes));
        while (static_cast<int>(raw.size()) < nbytes) raw.push_back(0);
        out.push_back(std::move(raw));
    }
    std::vector<uint8_t> rnd(static_cast<std::size_t>(nbytes));
    std::mt19937 rng{std::random_device{}()};
    for (auto& b : rnd) b = static_cast<uint8_t>(rng());
    out.push_back(std::move(rnd));
    return out;
}

Finding diff_pair(const FunctionInfo& a, const FunctionInfo& b, const fs::path&) {
    auto base = make_find("diff", laws::CLEAN, a, "", "", laws::STRENGTH_FINDS);
    base.function = a.name + "/" + b.name;
    if (a.kind != "SCALAR" || b.kind != "SCALAR") {
        base.status = std::string(laws::NEEDS_HARNESS);
        base.message = "differential testing needs two SCALAR functions, got " + a.kind + "/" + b.kind +
                       "; POINTER/OTHER would invent a buffer or object";
        return base;
    }
    if (a.params != b.params) {
        base.status = std::string(laws::ERROR);
        base.message = "parameter lists differ; same bytes would not mean the same arguments";
        return base;
    }
    if (!sandbox::allowed()) {
        // Law 9: the harness compiles and runs both scanned functions.
        auto f = sandbox::exec_notrun("diff", "diff " + a.name + "/" + b.name);
        f.file = a.file;
        f.function = a.name + "/" + b.name;
        f.line = a.line;
        return f;
    }
    auto cc = Config{}.which({"gcc", "clang"});
    if (!cc) {
        base.status = std::string(laws::NOTRUN);
        base.message = "no gcc/clang on PATH";
        base.extra["install"] = "install gcc or clang";
        return base;
    }
    auto src = emit_diff_program(a, b);
    auto td = fs::temp_directory_path() / ("prism_diff_" + std::to_string(std::random_device{}()));
    fs::create_directories(td);
    auto harness = td / "diff.c";
    auto exe = td / "diff.exe";
    {
        std::ofstream out(harness);
        out << src;
    }
    auto cr = run_argv({cc->string(), "-O0", "-g", "-std=c11", harness.string(), "-o", exe.string()}, {}, 30.0);
    if (cr.timeout || cr.rc != 0) {
        fs::remove_all(td);
        base.status = std::string(laws::ERROR);
        auto err = cr.err.empty() ? cr.out : cr.err;
        if (err.size() > 400) err.resize(400);
        base.message = "diff compile: " + err;
        return base;
    }
    int nbytes = param_nbytes(a.params);
    bool timed_out = false;
    for (auto& data : diff_inputs(nbytes)) {
        std::string in(data.begin(), data.end());
        auto rr = run_argv(sandbox::wrap_argv({exe.string()}, td), in, 1.0, sandbox::limits_for(1.0));
        bool disagree = rr.rc == 2 || rr.err.starts_with("DIFF");
        if (disagree) {
            fs::remove_all(td);
            base.status = std::string(laws::FAILED);
            base.cls = "FUNC-CONTRACT";
            base.message = a.name + " and " + b.name + " disagree";
            base.evidence = rr.err.substr(0, 800);
            std::string hex;
            for (auto b : data) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "%02x", b);
                hex += buf;
            }
            base.counterexample = hex;
            base.extra["a"] = a.file;
            base.extra["b"] = b.file;
            return base;
        }
        if (rr.crashed || rr.rc < 0) {
            fs::remove_all(td);
            base.status = std::string(laws::CRASH);
            base.cls = "FUZZ-CRASH";
            std::string hex;
            for (std::size_t i = 0; i < data.size() && i < 16; ++i) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "%02x", data[i]);
                hex += buf;
            }
            base.message = "diff harness crashed on " + hex;
            base.evidence = rr.err.substr(0, 800);
            std::string full;
            for (auto b : data) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "%02x", b);
                full += buf;
            }
            base.counterexample = full;
            return base;
        }
        if (rr.timeout) timed_out = true;
    }
    fs::remove_all(td);
    if (timed_out) {
        base.status = std::string(laws::TIMEOUT);
        base.message = "diff harness timed out (not agreement, not a proof)";
        base.extra["a"] = a.file;
        base.extra["b"] = b.file;
        return base;
    }
    base.status = std::string(laws::CLEAN);
    base.message = "no disagreement on sampled inputs (not a proof)";
    base.extra["a"] = a.file;
    base.extra["b"] = b.file;
    return base;
}

}  // namespace

std::vector<Finding> run_taint(const std::vector<FunctionInfo>& functions) {
    std::vector<Finding> out;
    for (auto& fn : functions)
        if (auto hit = analyze_taint(fn)) out.push_back(*hit);
    return out;
}

std::vector<Finding> run_thread(const std::vector<FunctionInfo>& functions) {
    if (functions.empty()) return {};
    std::map<std::string, std::vector<FunctionInfo>> by_file;
    for (auto& fn : functions) by_file[fn.file].push_back(fn);
    std::vector<Finding> out;
    static Regex thread_api(
        "\\b(?:pthread_create|std::jthread|std::thread|CreateThread|thrd_create)\\b");
    static Regex mutex_re("\\b(?:mtx_lock|mtx_timedlock|pthread_mutex)\\b");
    for (auto& [rel, fns] : by_file) {
        std::string file_text;
        auto p = locate_source(fns.front());
        if (p) file_text = strip_comments_keep_lines(read_text_file(*p));
        else {
            for (auto& fn : fns) {
                if (!file_text.empty()) file_text += '\n';
                file_text += fn.body;
            }
        }
        bool has_api = thread_api.search(file_text);
        if (!has_api)
            for (auto& fn : fns)
                if (thread_api.search(fn.body)) has_api = true;
        if (!has_api) continue;
        auto globals = file_globals(file_text);
        if (globals.empty()) continue;
        std::map<std::string, std::vector<FunctionInfo>> writers;
        for (auto& fn : fns)
            for (auto& g : globals)
                if (writes_global(fn.body, g)) writers[g].push_back(fn);
        for (auto& [g, who] : writers) {
            std::vector<FunctionInfo> unsync;
            for (auto& fn : who)
                if (!mutex_re.search(fn.body)) unsync.push_back(fn);
            if (unsync.size() < 2) continue;
            auto& anchor = unsync[0];
            auto f = make_find("thread", laws::FAILED, anchor, "RACE-SHARED",
                               "global '" + g + "' written from " + std::to_string(unsync.size()) +
                                   " functions without mtx_lock/pthread_mutex",
                               laws::STRENGTH_FINDS);
            f.file = rel;
            f.evidence = g;
            f.extra["global"] = g;
            nlohmann::json w = nlohmann::json::array();
            for (auto& fn : unsync) w.push_back(fn.name);
            f.extra["writers"] = w.dump();
            out.push_back(std::move(f));
        }
    }
    return out;
}

std::vector<Finding> prove_contracts(const std::vector<FunctionInfo>& functions, int unwind) {
    std::vector<Finding> out;
    static Regex req_atom("^([A-Za-z_]\\w*)\\s*(<|>=|!=)\\s*(0|[1-9]\\d*)$");
    static Regex ens_atom("^result\\s*(==|>=)\\s*(.+)$");
    for (auto& fn : functions) {
        auto spec = parse_comments(fn);
        if (spec.requires_.empty() && spec.ensures.empty() && spec.decreases.empty() && spec.invariant.empty())
            continue;
        std::optional<std::string> requires_, ensures, decreases, invariant;
        if (!spec.requires_.empty()) requires_ = join_sv(spec.requires_, " && ");
        if (!spec.ensures.empty()) ensures = join_sv(spec.ensures, " && ");
        if (!spec.decreases.empty()) decreases = spec.decreases[0];
        if (!spec.invariant.empty()) invariant = join_sv(spec.invariant, " && ");
        bool ptr_body = body_needs_pointer_harness(fn.body);
        if (fn.kind == "POINTER" || ptr_body || fn.kind != "SCALAR") {
            std::string why = (fn.kind == "POINTER" || ptr_body) ? "POINTER" : fn.kind;
            std::string msg = why == "POINTER"
                                  ? "POINTER: contract BMC would invent a buffer; not a proof"
                                  : why + ": contract scalar subset only; not a proof";
            auto f = make_find("contracts", laws::NEEDS_HARNESS, fn, "FUNC-CONTRACT", std::move(msg),
                               laws::STRENGTH_PROVES);
            if (requires_) f.extra["requires"] = *requires_;
            if (ensures) f.extra["ensures"] = *ensures;
            if (decreases) f.extra["decreases"] = *decreases;
            if (invariant) f.extra["invariant"] = *invariant;
            out.push_back(std::move(f));
            continue;
        }
        auto rec = bmc_with_assume(fn, unwind, requires_, ensures, decreases, invariant);
        if (rec.status != laws::ERROR && invariant && has_loop(fn.body)) {
            auto baseline = bmc_with_assume(fn, unwind, requires_, ensures, decreases, std::nullopt);
            bool closed_without = baseline.status == laws::PROVED_UNBOUNDED;
            auto orig = rec.extra.contains("original_status") ? rec.extra["original_status"] : rec.status;
            if (laws::is_proof(orig)) {
                if (orig == laws::PROVED_UNBOUNDED) rec.extra["original_status"] = std::string(laws::PROVED_ASSUMING);
                if (laws::is_proof(rec.status) && rec.status == laws::PROVED_UNBOUNDED) {
                    rec.status = std::string(laws::PROVED_ASSUMING);
                    rec.message = "ensures holds assuming invariant (" + *invariant + "); never PROVED-UNBOUNDED";
                }
            } else if (closed_without && laws::is_proof(rec.status)) {
                rec.status = std::string(laws::PROVED_ASSUMING);
                rec.message = "ensures holds assuming invariant (" + *invariant + "); never PROVED-UNBOUNDED";
            }
        }
        if (rec.status != laws::ERROR && decreases && has_loop(fn.body)) {
            auto baseline = bmc_with_assume(fn, unwind, requires_, ensures, std::nullopt, invariant);
            bool closed_without = baseline.status == laws::PROVED_UNBOUNDED;
            auto orig = rec.extra.contains("original_status") ? rec.extra["original_status"] : rec.status;
            bool assumed = laws::is_proof(orig) && !closed_without;
            rec.extra["decreases_assumed"] = assumed ? "true" : "false";
            if (assumed) {
                if (orig == laws::PROVED_UNBOUNDED) rec.extra["original_status"] = std::string(laws::PROVED_ASSUMING);
                if (laws::is_proof(rec.status) && rec.status == laws::PROVED_UNBOUNDED) {
                    rec.status = std::string(laws::PROVED_ASSUMING);
                    rec.message = "ensures holds under decreases variant; never PROVED-UNBOUNDED";
                }
            }
        }
        bool subset = true;
        for (auto& r : spec.requires_)
            if (!fullmatch(req_atom, strip(r))) subset = false;
        for (auto& e : spec.ensures)
            if (!fullmatch(ens_atom, strip(e))) subset = false;
        rec.extra["subset"] = subset ? "true" : "false";
        out.push_back(std::move(rec));
    }
    return out;
}

namespace {

// Python engine prism/wp.py encode_predicate / _ACSL_UNENC. Regex is the honesty
// gate: unencodable ACSL is ERROR, never PROVED-ASSUMING.
static Regex wp_acsl_unenc(
    "(?i)\\\\(?:"
    "valid(?:_read|_write|_index)?|forall|exists|old|at|"
    "separated|initialized|dangling|null|base_addr|block_length|"
    "offset|allocable|freeable|fresh|from|nothing|let|lambda|"
    "true|false|union|inter|subset|empty|is_finite|is_NaN|"
    "min|max|sum|product|numof|matches|unspecified|allocation"
    ")\\b");
static Regex wp_tok(
    "\\s+|==>|==|!=|<=|>=|&&|\\|\\||<<|>>|->|"
    "[+\\-*/%<>=!&|^~?:()]|"
    "0[xX][0-9A-Fa-f]+|"
    "\\d+|"
    "[A-Za-z_]\\w*|"
    ".");
static Regex wp_ident_re("^[A-Za-z_]\\w*$");
static Regex wp_num_re("^(?:0[xX][0-9A-Fa-f]+|\\d+)$");
static Regex wp_result_re("\\bresult\\b");

const std::unordered_set<std::string> kWpBinop = {
    "+", "-", "*", "/", "%", "<", ">", "<=", ">=", "==", "!=",
    "&&", "||", "<<", ">>", "&", "|", "^", "?", ":",
};
const std::unordered_set<std::string> kWpUnaryOk = {"+", "-", "!", "~"};

bool wp_outer_parens(std::string_view s) {
    if (s.size() < 2 || s.front() != '(' || s.back() != ')') return false;
    int depth = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') {
            --depth;
            if (depth == 0) return i + 1 == s.size();
        }
    }
    return false;
}

std::optional<std::pair<std::string, std::string>> wp_split_top(std::string_view text, std::string_view sep) {
    int depth = 0;
    const std::size_t n = sep.size();
    if (n == 0 || text.size() < n) return std::nullopt;
    for (std::size_t i = 0; i + n <= text.size(); ++i) {
        char ch = text[i];
        if (ch == '(') ++depth;
        else if (ch == ')') {
            --depth;
            if (depth < 0) return std::nullopt;
        } else if (depth == 0 && text.substr(i, n) == sep)
            return std::make_pair(std::string(text.substr(0, i)), std::string(text.substr(i + n)));
    }
    return std::nullopt;
}

std::optional<std::string> wp_rewrite_implies(std::string text) {
    text = strip(std::move(text));
    if (wp_outer_parens(text)) {
        auto inner = wp_rewrite_implies(text.substr(1, text.size() - 2));
        if (!inner) return std::nullopt;
        return "(" + *inner + ")";
    }
    auto split = wp_split_top(text, "==>");
    if (!split) return text;
    auto left = wp_rewrite_implies(split->first);
    auto right = wp_rewrite_implies(split->second);
    if (!left || !right) return std::nullopt;
    return "!(" + *left + ") || (" + *right + ")";
}

bool wp_before_unary(const std::optional<std::string>& prev) {
    if (!prev) return true;
    if (*prev == "(" || *prev == "?" || *prev == ":") return true;
    if (kWpBinop.contains(*prev)) return true;
    return false;
}

std::optional<std::string> wp_encode_scalar(const std::string& text) {
    std::vector<std::string> toks;
    for (auto& m : wp_tok.finditer(text)) {
        auto t = m.text;
        if (t.empty()) continue;
        bool sp = true;
        for (char c : t)
            if (!std::isspace(static_cast<unsigned char>(c))) {
                sp = false;
                break;
            }
        if (sp) continue;
        toks.push_back(std::move(t));
    }
    if (toks.empty()) return std::nullopt;
    std::optional<std::string> prev;
    for (std::size_t i = 0; i < toks.size(); ++i) {
        const auto& t = toks[i];
        if (t == "[" || t == "]" || t == "{" || t == "}" || t == "." || t == "," || t == ";" ||
            t == "@" || t == "\\" || t == "#")
            return std::nullopt;
        if (t == "->") return std::nullopt;
        if (fullmatch(wp_ident_re, t)) {
            const std::string* nxt = (i + 1 < toks.size()) ? &toks[i + 1] : nullptr;
            if (nxt && *nxt == "(") return std::nullopt;
            prev = t;
            continue;
        }
        if (fullmatch(wp_num_re, t)) {
            prev = t;
            continue;
        }
        if (kWpUnaryOk.contains(t) || t == "*" || t == "&") {
            bool unary = wp_before_unary(prev) || (prev && (kWpUnaryOk.contains(*prev) || *prev == "*" || *prev == "&"));
            if ((t == "*" || t == "&") && unary) return std::nullopt;
            if (kWpUnaryOk.contains(t) && unary) {
                prev = t;
                continue;
            }
        }
        if (kWpBinop.contains(t) || t == "(" || t == ")") {
            prev = t;
            continue;
        }
        return std::nullopt;
    }
    return strip(text);
}

std::optional<std::string> wp_encode_predicate(std::string raw) {
    raw = strip(std::move(raw));
    if (raw.empty()) return std::nullopt;
    {
        const std::string from = "\\result";
        std::size_t pos = 0;
        while ((pos = raw.find(from, pos)) != std::string::npos) {
            raw.replace(pos, from.size(), "result");
            pos += 6;
        }
    }
    if (wp_acsl_unenc.search(raw) || raw.find("\\ ") != std::string::npos || raw.starts_with("\\"))
        return std::nullopt;
    if (raw.find("<==>") != std::string::npos || raw.find("^^") != std::string::npos) return std::nullopt;
    auto rewritten = wp_rewrite_implies(raw);
    if (!rewritten) return std::nullopt;
    return wp_encode_scalar(*rewritten);
}

std::string wp_subst_result(const std::string& ensures, const std::string& expr) {
    std::string out;
    std::size_t i = 0;
    const std::string repl = "(" + expr + ")";
    for (auto& m : wp_result_re.finditer(ensures)) {
        if (m.spans.empty()) continue;
        auto a = static_cast<std::size_t>(std::max(0, m.spans[0].first));
        auto b = static_cast<std::size_t>(std::max(0, m.spans[0].second));
        if (a < i) continue;
        out.append(ensures, i, a - i);
        out += repl;
        i = b;
    }
    out.append(ensures, i, std::string::npos);
    return out;
}

std::string wp_norm_expr(std::string s) {
    s = strip(std::move(s));
    while (wp_outer_parens(s)) s = strip(s.substr(1, s.size() - 2));
    std::string o;
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c))) o.push_back(c);
    return o;
}

std::vector<std::string> wp_split_and(const std::string& text) {
    std::vector<std::string> out;
    int depth = 0;
    std::size_t last = 0;
    std::size_t i = 0;
    while (i + 1 < text.size()) {
        char ch = text[i];
        if (ch == '(') ++depth;
        else if (ch == ')') --depth;
        else if (depth == 0 && text[i] == '&' && text[i + 1] == '&') {
            auto p = strip(text.substr(last, i - last));
            if (!p.empty()) out.push_back(std::move(p));
            last = i + 2;
            i += 2;
            continue;
        }
        ++i;
    }
    auto tail = strip(text.substr(last));
    if (!tail.empty()) out.push_back(std::move(tail));
    return out;
}

bool wp_is_tautology(const std::string& pred) {
    auto p = strip(pred);
    if (p == "1" || p == "true" || p == "True") return true;
    auto parts = wp_split_and(p);
    if (parts.size() > 1) {
        for (auto& x : parts)
            if (!wp_is_tautology(x)) return false;
        return true;
    }
    for (const char* op : {"==", ">=", "<="}) {
        auto split = wp_split_top(p, op);
        if (!split) continue;
        auto a = wp_norm_expr(split->first);
        auto b = wp_norm_expr(split->second);
        if (!a.empty() && a == b) return true;
    }
    return false;
}

}  // namespace

std::vector<Finding> run_wp(const std::vector<FunctionInfo>& functions, int unwind) {
    std::vector<Finding> out;
    static Regex ret_re("\\breturn\\s+([^;]+);");
    for (auto& fn : functions) {
        auto spec = parse_comments(fn);
        if (spec.ensures.empty()) continue;
        auto mark = [](Finding& f) {
            f.extra["engine"] = "prism-wp";
            f.extra["wp"] = "return-substitution";
        };
        if (fn.kind == "POINTER" || body_needs_pointer_harness(fn.body)) {
            auto f = make_find("wp", laws::NEEDS_HARNESS, fn, "FUNC-CONTRACT",
                               "POINTER: WP would invent a buffer; Frama-C WP binary is not a proof either",
                               laws::STRENGTH_PROVES);
            mark(f);
            out.push_back(std::move(f));
            continue;
        }
        if (fn.kind != "SCALAR") {
            auto f = make_find("wp", laws::NEEDS_HARNESS, fn, "FUNC-CONTRACT",
                               fn.kind + ": WP scalar subset only; not a proof", laws::STRENGTH_PROVES);
            mark(f);
            out.push_back(std::move(f));
            continue;
        }

        std::vector<std::string> encoded_req;
        std::optional<std::string> bad;
        for (auto& r : spec.requires_) {
            auto e = wp_encode_predicate(r);
            if (!e) {
                bad = r;
                break;
            }
            encoded_req.push_back(*e);
        }
        std::vector<std::string> encoded_ens;
        if (!bad) {
            for (auto& e0 : spec.ensures) {
                auto e = wp_encode_predicate(e0);
                if (!e) {
                    bad = e0;
                    break;
                }
                encoded_ens.push_back(*e);
            }
        }
        if (bad) {
            auto f = make_find("wp", laws::ERROR, fn, "FUNC-CONTRACT",
                               "cannot encode WP predicate (" + *bad + ")", laws::STRENGTH_PROVES);
            mark(f);
            f.extra["wp_unencoded"] = "true";
            if (!spec.requires_.empty()) f.extra["requires"] = join_sv(spec.requires_, " && ");
            f.extra["ensures"] = join_sv(spec.ensures, " && ");
            out.push_back(std::move(f));
            continue;
        }

        std::optional<std::string> requires_;
        if (!encoded_req.empty()) requires_ = join_sv(encoded_req, " && ");
        std::string ensures = join_sv(encoded_ens, " && ");
        std::vector<std::string> returns;
        nlohmann::json rets = nlohmann::json::array();
        for (auto& m : ret_re.finditer(fn.body)) {
            auto g = strip(m.group(1));
            if (g.empty()) continue;
            returns.push_back(g);
            rets.push_back(g);
            if (returns.size() >= 8) break;
        }
        std::vector<std::string> vcs;
        if (returns.empty()) vcs.push_back(ensures);
        else
            for (auto& expr : returns) vcs.push_back(wp_subst_result(ensures, expr));
        std::string wp_vc;
        if (requires_) wp_vc += "(" + *requires_ + ") ==> ";
        for (std::size_t i = 0; i < vcs.size(); ++i) {
            if (i) wp_vc += " && ";
            wp_vc += "(" + vcs[i] + ")";
        }
        auto fill_extra = [&](Finding& rec) {
            mark(rec);
            rec.extra["wp_returns"] = rets.dump();
            rec.extra["wp_vc"] = wp_vc;
            rec.extra["requires"] = requires_ ? *requires_ : "";
            rec.extra["ensures"] = ensures;
        };

        bool qed = !vcs.empty();
        for (auto& v : vcs)
            if (!wp_is_tautology(v)) qed = false;
        if (qed) {
            auto f = make_find("wp", laws::PROVED_ASSUMING, fn, "FUNC-CONTRACT",
                               "WP of ensures (" + ensures + ") holds" +
                                   (requires_ ? " assuming (" + *requires_ + ")" : "") +
                                   "; never an unconditional PROVED",
                               laws::STRENGTH_PROVES);
            fill_extra(f);
            f.extra["wp_qed"] = "true";
            out.push_back(std::move(f));
            continue;
        }

        std::optional<std::string> ens_opt = ensures;
        auto rec = bmc_with_assume(fn, unwind, requires_, ens_opt, std::nullopt, std::nullopt);
        rec.stage = "wp";
        fill_extra(rec);
        if (rec.status == laws::PROVED) {
            rec.status = std::string(laws::PROVED_ASSUMING);
            rec.message = "WP of ensures (" + ensures + ") holds" +
                          (requires_ ? " assuming (" + *requires_ + ")" : "") +
                          "; never an unconditional PROVED";
        } else if (rec.status == laws::PROVED_UNBOUNDED) {
            rec.status = std::string(laws::PROVED_ASSUMING);
            rec.message = "WP of ensures (" + ensures + ") holds for all unrollings" +
                          (requires_ ? " assuming (" + *requires_ + ")" : "") +
                          "; never PROVED-UNBOUNDED from WP";
        } else if (rec.status == laws::PROVED_ASSUMING) {
            auto low = lower_copy(rec.message);
            if (low.find("never") == std::string::npos) {
                rec.message = "WP of ensures (" + ensures + ") holds" +
                              (requires_ ? " assuming (" + *requires_ + ")" : "") +
                              "; never an unconditional PROVED";
            }
        }
        out.push_back(std::move(rec));
    }
    return out;
}

std::vector<Finding> run_harness_bmc(const std::vector<FunctionInfo>& functions, int unwind) {
    std::vector<Finding> out;
    for (auto& fn : functions) {
        if (fn.kind != "POINTER") continue;
        auto harnessed = materialize(fn);
        if (!harnessed) {
            auto f = make_find(
                "harness", laws::NEEDS_HARNESS, fn, "",
                "POINTER: no honest requires; unguarded BMC would invent "
                "a buffer or dress a NULL crash as a finding",
                laws::STRENGTH_SOME);
            f.extra["harness"] = "false";
            f.extra["assumed"] = "false";
            out.push_back(std::move(f));
            continue;
        }
        auto spec = parse_comments(fn);
        std::vector<std::string> reqs;
        for (auto& r : spec.requires_) {
            auto s = strip(r);
            if (!s.empty()) reqs.push_back(s);
        }
        auto recs = run_bmc({*harnessed}, unwind, true);
        auto r = recs.empty() ? bmc_one(*harnessed, unwind) : recs[0];
        r.extra["assumed"] = "true";
        r.extra["requires"] = nlohmann::json(reqs).dump();
        r.extra["original_status"] = r.status;
        r.extra["harness"] = "true";
        r.stage = "harness";
        if (r.status == laws::PROVED || r.status == laws::PROVED_UNBOUNDED) {
            r.status = std::string(laws::PROVED_ASSUMING);
            r.message = "encoded UB properties hold assuming (" + join_sv(reqs, " && ") +
                        "); never an unconditional PROVED";
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<Finding> run_concolic(const std::vector<FunctionInfo>& functions, int budget) {
    std::vector<Finding> out;
    for (auto& fn : functions) out.push_back(concolic_function(fn, budget));
    return out;
}

std::vector<Finding> run_fuse(const std::vector<FunctionInfo>& functions, const std::vector<Finding>& bmc_findings,
                              const fs::path& src_root, double budget, int iters, bool llm) {
    std::vector<Finding> out;
    int rounds = 2;
    double slice_budget = std::max(0.05, budget / rounds);
    int slice_iters = std::max(1, iters / rounds);
    std::optional<LlamaEngine> eng;
    if (llm) eng.emplace(Config{});
    LlamaEngine* ep = eng ? &*eng : nullptr;
    for (auto& fn : functions)
        out.push_back(fuse_one(fn, bmc_findings, src_root, slice_budget, slice_iters, rounds, ep));
    if (ep && !ep->available() && !functions.empty()) {
        Finding f;
        f.stage = "fuse";
        f.status = std::string(laws::NOTRUN);
        f.file = functions[0].file;
        f.function = functions[0].name;
        f.line = functions[0].line;
        f.message = LLM_SKIP_FUSE_MSG;
        f.strength = std::string(laws::STRENGTH_READS);
        f.extra["install"] = LLM_INSTALL;
        f.extra["autoprompt"] = "NOTRUN";
        f.extra["chatfuzz"] = "NOTRUN";
        f.extra["docstring"] = documentation_from_comments(read_fn_source(functions[0]));
        out.push_back(std::move(f));
    } else if (ep && ep->available()) {
        for (auto& fn : functions) {
            auto docs = documentation_from_comments(read_fn_source(fn));
            std::string distilled = docs;
            try {
                auto t = fuzz4all_autoprompt_text(*ep, fn);
                if (!t.empty()) distilled = t;
            } catch (...) {
            }
            Finding f;
            f.stage = "fuse";
            f.status = std::string(laws::HYPOTHESIS);
            f.file = fn.file;
            f.function = fn.name;
            f.line = fn.line;
            f.message = "Fuzz4All distilled prompt (hypothesis, not a proof)";
            f.strength = std::string(laws::STRENGTH_READS);
            if (distilled.size() > 800) distilled.resize(800);
            f.extra["autoprompt"] = distilled;
            auto ds = docs;
            if (ds.size() > 400) ds.resize(400);
            f.extra["docstring"] = ds;
            f.extra["target_api"] = fn.name;
            out.push_back(std::move(f));
        }
    }
    return out;
}

std::vector<Finding> run_diff(const std::vector<FunctionInfo>& functions, const fs::path& root) {
    auto pairs = diff_pairs(functions);
    if (pairs.empty()) return {};
    std::vector<Finding> out;
    for (auto& [a, b] : pairs) out.push_back(diff_pair(a, b, root));
    return out;
}

std::optional<Finding> contract_kind_finding(const FunctionInfo& fn, const std::string& stage) {
    if (fn.kind == "SCALAR") return std::nullopt;
    auto spec = spec_comments_rapid(fn);
    if (spec.ensures.empty()) return std::nullopt;
    auto f = make_find(stage, laws::NEEDS_HARNESS, fn, "",
                       fn.kind + ": RapidCheck would invent a buffer or pass NULL; not a sampled proof",
                       laws::STRENGTH_SOME);
    f.extra["requires"] = nlohmann::json(spec.requires_).dump();
    f.extra["ensures"] = nlohmann::json(spec.ensures).dump();
    return f;
}

std::vector<Finding> run_rapid(const std::vector<FunctionInfo>& functions, int trials) {
    std::vector<Finding> out;
    for (auto& fn : functions) {
        if (auto kind = contract_kind_finding(fn, "rapid")) {
            out.push_back(*kind);
            continue;
        }
        auto plan = plan_trials(fn, trials);
        if (!plan) continue;
        out.push_back(finding_from_plan(fn, *plan, "rapid"));
    }
    return out;
}

std::vector<Finding> run_muttest(const std::vector<FunctionInfo>& functions, int trials) {
    // Killing a mutant is CLEAN (not a proof). Missing gcc/clang is NOTRUN, never CLEAN.
    // Eval errors without a cex are ERROR/NOTRUN, not a killed mutant.
    auto compiler_missing = [](const std::string& err) {
        auto text = lower_copy(err);
        if (text.find("no gcc") != std::string::npos || text.find("gcc/clang") != std::string::npos)
            return true;
        if (text.find("not on path") != std::string::npos &&
            (text.find("gcc") != std::string::npos || text.find("clang") != std::string::npos ||
             text.find("compiler") != std::string::npos))
            return true;
        Config cfg;
        return !cfg.which({"gcc", "clang"}).has_value();
    };
    std::vector<Finding> out;
    for (auto& fn : functions) {
        if (auto kind = contract_kind_finding(fn, "muttest")) {
            out.push_back(*kind);
            continue;
        }
        auto plan = plan_trials(fn, trials);
        if (!plan) continue;
        auto sites = iter_mutations(fn.body);
        if (sites.empty()) continue;
        auto baseline = run_plan(fn, *plan);
        if (!baseline.error.empty() && baseline.counterexample.empty()) {
            bool missing = compiler_missing(baseline.error);
            auto f = make_find("muttest", missing ? laws::NOTRUN : laws::ERROR, fn, "",
                               "cannot score mutants: " + baseline.error, laws::STRENGTH_SOME);
            if (missing) f.extra["install"] = "install gcc or clang";
            out.push_back(std::move(f));
            continue;
        }
        for (auto& [start, end, src, dst] : sites) {
            auto mutated = fn.body;
            mutated.replace(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start), dst);
            auto cloned = fn;
            cloned.body = mutated;
            auto info = run_plan(cloned, *plan);
            std::map<std::string, std::string> extra{
                {"from", src},
                {"to", dst},
                {"mutation", src + " -> " + dst},
                {"offset", std::to_string(start)},
                {"trials", std::to_string(trials)},
                {"engine", info.engine},
                {"requires", nlohmann::json(plan->requires_).dump()},
                {"ensures", nlohmann::json(plan->ensures).dump()},
            };
            if (!info.error.empty() && info.counterexample.empty()) {
                bool missing = compiler_missing(info.error);
                if (missing) extra["install"] = "install gcc or clang";
                auto f = make_find("muttest", missing ? laws::NOTRUN : laws::ERROR, fn, "",
                                   "cannot score mutant " + src + " -> " + dst + ": " + info.error,
                                   laws::STRENGTH_SOME);
                f.extra = extra;
                out.push_back(std::move(f));
                continue;
            }
            bool killed = !info.ok;
            if (killed) {
                auto f = make_find("muttest", laws::CLEAN, fn, "",
                                   "mutant killed: " + src + " -> " + dst + " (" +
                                       (info.counterexample.empty() ? "tests failed on mutant"
                                                                    : info.counterexample) +
                                       "); silence is not a proof",
                                   laws::STRENGTH_SOME);
                f.extra = extra;
                out.push_back(std::move(f));
            } else {
                auto f = make_find("muttest", laws::FAILED, fn, "FUNC-CONTRACT",
                                   "mutant survived: " + src + " -> " + dst + " (all " +
                                       std::to_string(plan->samples.size()) + " trials still hold; not a proof)",
                                   laws::STRENGTH_FINDS);
                f.evidence = mutated.substr(0, 400);
                f.extra = extra;
                out.push_back(std::move(f));
            }
        }
    }
    return out;
}

std::vector<Finding> run_ltl(const std::vector<FunctionInfo>& functions, const std::vector<fs::path>& specs) {
    std::vector<std::string> formulas;
    for (auto& p : specs)
        for (auto& f : parse_ltl_file(p)) formulas.push_back(f);
    if (formulas.empty()) {
        Finding f;
        f.stage = "ltl";
        f.status = std::string(laws::NOTRUN);
        f.cls = "LTL-SAFETY";
        f.message = "no .ltl spec next to the sources";
        f.strength = std::string(laws::STRENGTH_PROVES);
        f.extra["install"] = "add a file with G (...)";
        return {f};
    }
    std::vector<std::pair<FunctionInfo, Fsm>> fsms;
    for (auto& fn : functions)
        if (auto fsm = extract_fsm(fn.body)) fsms.emplace_back(fn, *fsm);
    Config cfg;
    auto strix = cfg.which({"strix"});
    std::vector<Finding> out;
    for (auto& formula : formulas) {
        bool decided = false;
        auto kind = classify_ltl(formula);
        for (auto& [fn, fsm] : fsms) {
            auto f = check_safety(formula, fsm);
            if (!f) f = synthesize_missing(fsm, formula);
            if (f) {
                f->file = fn.file;
                f->function = fn.name;
                f->line = fn.line;
                out.push_back(*f);
                decided = true;
            }
        }
        if (!decided) {
            std::string msg;
            if (is_approx_kind(kind.kind))
                msg = formula + ": known liveness pattern but predicates were not evaluable on this FSM";
            else if (kind.kind != LtlKind::Nonsafety) {
                if (fsms.empty())
                    msg = formula + ": in the safety fragment but no switch(state) FSM extracted";
                else
                    msg = formula + ": in the safety fragment but predicates were not evaluable on this FSM";
            } else if (strix)
                msg = formula +
                      ": not in the safety fragment PRISM decides "
                      "(G p, G (p -> X q), G (req -> F_" +
                      std::to_string(F_BOUND) + " ack), G (F_" + std::to_string(F_BOUND) +
                      " p)); strix is present but PRISM does not treat strix output as PROVED";
            else
                msg = formula + ": not in the safety fragment PRISM decides; missing Strix binary";
            Finding f;
            f.stage = "ltl";
            f.status = std::string(laws::NOTRUN);
            f.cls = "LTL-SAFETY";
            f.message = msg;
            f.strength = std::string(laws::STRENGTH_PROVES);
            f.extra["formula"] = formula;
            f.extra["install"] = "https://github.com/meyerphi/strix";
            f.extra["strix"] = strix ? strix->string() : "";
            f.extra["strix_not_proved"] = "true";
            f.extra["strix_note"] = std::string(kStrixNote);
            out.push_back(std::move(f));
        }
    }
    return out;
}

std::vector<Finding> hypothesize(const std::vector<FunctionInfo>& functions, int budget, const Config& cfg) {
    // Missing GGUF/Ollama is NOTRUN, never CLEAN. Model text is HYPOTHESIS/READS.
    // Empty hypotheses are silence: still HYPOTHESIS, not a proof and not COVERED.
    LlamaEngine engine(cfg);
    if (!engine.available()) {
        Finding f;
        f.stage = "llm";
        f.status = std::string(laws::NOTRUN);
        f.cls = "INTENT";
        f.message = LLM_UNAVAILABLE_MSG;
        f.strength = std::string(laws::STRENGTH_READS);
        f.extra["install"] = LLM_INSTALL;
        f.extra["gguf"] = cfg.gguf.string();
#ifdef PRISM_HAS_LLAMA
        f.extra["prism_has_llama"] = "1";
#else
        f.extra["prism_has_llama"] = "0";
#endif
        return {f};
    }
    std::vector<Finding> out;
    int n = std::min(budget, static_cast<int>(functions.size()));
    for (int i = 0; i < n; ++i) {
        auto& fn = functions[static_cast<std::size_t>(i)];
        auto src = fn.signature + "\n{" + fn.body + "\n}";
        if (src.size() > 6000) src.resize(6000);
        auto r = engine.complete({{"system", SYSTEM_AUDITOR}, {"user", src}});
        if (!r.error.empty()) {
            // Python engine llm_complete_unavailable: HTTP/connection is a missing backend
            // (NOTRUN + LLM_INSTALL), never a code ERROR. Timed-out stays ERROR.
            if (llm_httpish(r.error)) {
                auto f = make_find("llm", laws::NOTRUN, fn, "INTENT", r.error, laws::STRENGTH_READS);
                f.extra["install"] = LLM_INSTALL;
                f.extra["backend"] = r.backend;
                out.push_back(std::move(f));
            } else {
                out.push_back(make_find("llm", laws::ERROR, fn, "INTENT", r.error, laws::STRENGTH_READS));
            }
            continue;
        }
        auto data = extract_json(r.text);
        if (!data.is_object() || !data.contains("hypotheses") || !data["hypotheses"].is_array() ||
            data["hypotheses"].empty()) {
            auto f = make_find("llm", laws::HYPOTHESIS, fn, "INTENT", r.text.substr(0, 400),
                               laws::STRENGTH_READS);
            f.extra["backend"] = r.backend;
            out.push_back(std::move(f));
            continue;
        }
        for (auto& h : data["hypotheses"]) {
            Finding f;
            f.stage = "llm";
            f.status = std::string(laws::HYPOTHESIS);
            f.file = fn.file;
            f.function = h.value("function", fn.name);
            if (h.contains("line") && h["line"].is_number_integer()) f.line = h["line"].get<int>();
            else f.line = fn.line;
            f.cls = h.value("cls", "INTENT");
            f.message = h.value("why", "");
            f.strength = std::string(laws::STRENGTH_READS);
            f.extra["backend"] = r.backend;
            out.push_back(std::move(f));
        }
    }
    return out;
}

ConcreteRec concrete_execute(const FunctionInfo& fn, const std::map<std::string, int>& args) {
    auto rec = execute(fn, args);
    ConcreteRec out;
    out.ub = rec.ub;
    out.rc = rec.value ? static_cast<int>(*rec.value) : 0;
    return out;
}

std::vector<Finding> execute_cex(const std::vector<Finding>& fails, const std::vector<FunctionInfo>& functions,
                                 const Config& cfg) {
    std::vector<Finding> ordered = fails;
    std::sort(ordered.begin(), ordered.end(), [](const Finding& a, const Finding& b) {
        int ka = a.status == laws::CRASH ? 0 : 1;
        int kb = b.status == laws::CRASH ? 0 : 1;
        if (ka != kb) return ka < kb;
        return a.stage < b.stage;
    });
    std::vector<Finding> out;
    int n = 0;
    for (auto& f0 : ordered) {
        if (n >= 16) break;
        const FunctionInfo* fn = nullptr;
        for (auto& x : functions) {
            if (!f0.function) break;
            if (x.name == *f0.function &&
                (x.file == f0.file || fs::path(x.file).filename() == fs::path(f0.file).filename())) {
                fn = &x;
                break;
            }
        }
        if (!fn && f0.function) {
            for (auto& x : functions)
                if (x.name == *f0.function) {
                    fn = &x;
                    break;
                }
        }
        if (!fn) continue;
        if (fn->kind == "POINTER" || fn->kind == "OTHER") {
            auto f = make_find("execute", laws::NEEDS_HARNESS, *fn, "",
                               fn->kind + ": cex replay would invent a buffer or object",
                               laws::STRENGTH_FINDS);
            f.extra["oracle"] = "concrete-replay";
            out.push_back(std::move(f));
            continue;
        }
        Args args;
        auto blob = f0.counterexample;
        for (char& c : blob)
            if (c == ';') c = ',';
        std::istringstream ss(blob);
        std::string part;
        while (std::getline(ss, part, ',')) {
            auto eq = part.find('=');
            if (eq == std::string::npos) continue;
            auto k = strip(part.substr(0, eq));
            auto sp = k.find_last_of(" \t");
            if (sp != std::string::npos) k = k.substr(sp + 1);
            auto v = strip(part.substr(eq + 1));
            auto sp2 = v.find(' ');
            if (sp2 != std::string::npos) v = v.substr(0, sp2);
            try {
                args[k] = std::stoi(v, nullptr, 0);
            } catch (...) {
            }
        }
        if (args.empty()) continue;
        ++n;
        auto rec = execute(*fn, args);
        Finding f;
        f.stage = "execute";
        f.file = fn->file;
        f.function = fn->name;
        f.line = fn->line;
        f.strength = std::string(laws::STRENGTH_FINDS);
        f.extra["oracle"] = "concrete-replay";
        if (!rec.ub.empty()) {
            f.status = std::string(laws::CRASH);
            f.cls = rec.ub;
            f.message = "cex replay trapped " + rec.ub;
            f.counterexample = f0.counterexample.substr(0, 200);
        } else {
            f.status = std::string(laws::CLEAN);
            f.message = "cex did not trap in concrete replay (not a proof)";
        }
        out.push_back(std::move(f));
    }
    if (cfg.llm && !fails.empty()) {
        LlamaEngine engine(cfg);
        // Python engine execute_cex: LLM half down is NOTRUN with extra.half=llm, never CLEAN.
        if (!engine.available()) {
            auto f = nr("execute", LLM_UNAVAILABLE_MSG);
            f.extra["install"] = LLM_INSTALL;
            f.extra["half"] = "llm";
            out.push_back(std::move(f));
        } else {
            auto& f0 = fails[0];
            std::string prompt = "Write a C main() that demonstrates this finding is real or not.\n" + f0.file +
                                 ":" + (f0.line ? std::to_string(*f0.line) : "0") + " " +
                                 (f0.function ? *f0.function : "") + " " + f0.cls + ": " + f0.message +
                                 "\ncounterexample: " + f0.counterexample;
            auto extra = interpreter_loop(engine, prompt, 2, cfg.allow_exec);
            out.insert(out.end(), extra.begin(), extra.end());
        }
    }
    if (out.empty()) return {nr("execute", "no FAILED/CRASH cex to replay")};
    return out;
}

std::vector<Finding> rlef_repair(const Finding& fail, const Config& cfg) {
    LlamaEngine engine(cfg);
    if (!engine.available()) {
        auto f = nr("repair", LLM_UNAVAILABLE_MSG);
        f.extra["install"] = LLM_INSTALL;
        return {f};
    }
    if (!which_cc()) {
        auto f = nr("repair", "gcc/clang not on PATH", std::string(laws::STRENGTH_FINDS));
        f.extra["install"] = "install gcc or clang";
        return {f};
    }
    if (!cfg.allow_exec) {
        auto f = sandbox::exec_notrun("repair", "repair (LLM-written candidates)");
        f.strength = std::string(laws::STRENGTH_READS);
        return {f};
    }
    fs::path src_path = fail.file;
    if (!fs::exists(src_path)) src_path = cfg.root / fail.file;
    std::string source;
    try {
        source = read_text_file(src_path);
    } catch (const std::exception& ex) {
        Finding f;
        f.stage = "repair";
        f.status = std::string(laws::ERROR);
        f.file = src_path.string();
        f.message = ex.what();
        f.strength = std::string(laws::STRENGTH_READS);
        return {f};
    }
    if (source.empty() && !fs::exists(src_path)) {
        Finding f;
        f.stage = "repair";
        f.status = std::string(laws::ERROR);
        f.file = src_path.string();
        f.message = "cannot read source";
        f.strength = std::string(laws::STRENGTH_READS);
        return {f};
    }
    std::string feedback = fail.stage + " " + fail.status + " " + fail.cls + " " + fail.message + " " +
                           fail.counterexample;
    int rounds = cfg.repair_rounds;
    int best_score = -1;
    std::string best_src = source;
    std::vector<std::pair<std::string, std::string>> messages{
        {"system", SYSTEM_REPAIR},
        {"user", "SOURCE:\n" + source + "\n\nFEEDBACK:\n" + feedback},
    };
    nlohmann::json history = nlohmann::json::array();
    for (int i = 0; i < rounds; ++i) {
        auto r = engine.complete(messages, 120);
        if (!r.error.empty()) {
            if (llm_httpish(r.error) && best_score < 0) {
                auto f = nr("repair", r.error);
                f.extra["install"] = LLM_INSTALL;
                f.extra["backend"] = r.backend;
                f.extra["history"] = history.dump();
                return {f};
            }
            history.push_back(r.error);
            break;
        }
        auto src = c_from_llm(r.text);
        if (src.empty()) {
            history.push_back({{"round", i + 1}, {"score", nlohmann::json(nullptr)}, {"error", "silent"}});
            messages.push_back({"assistant", r.text});
            messages.push_back({"user", "Reply with a complete corrected C file only."});
            continue;
        }
        auto result = sandbox_run(src, 8.0, /*allow_exec=*/true);
        auto err = sandbox_err(result);
        if (err == "no compiler") {
            auto f = nr("repair", "gcc/clang not on PATH", std::string(laws::STRENGTH_FINDS));
            f.extra["install"] = "install gcc or clang";
            f.extra["history"] = history.dump();
            return {f};
        }
        std::string bmc_st;
        if (err != "compile" && err != "compile-timeout") bmc_st = bmc_status_of_source(src, 2);
        auto scored = rlef_reward(result, bmc_st);
        history.push_back({{"round", i + 1},
                           {"score", scored.score},
                           {"error", err.empty() ? nlohmann::json(nullptr) : nlohmann::json(err)},
                           {"bmc", bmc_st.empty() ? nlohmann::json(nullptr) : nlohmann::json(bmc_st)}});
        if (scored.score > best_score) {
            best_score = scored.score;
            best_src = src;
        }
        if (!scored.proved.empty()) {
            Finding f;
            f.stage = "repair";
            f.status = scored.proved;
            f.message = "RLEF BMC " + scored.proved + " on round " + std::to_string(i + 1) +
                        " (terminal success)";
            f.strength = std::string(laws::STRENGTH_PROVES);
            f.extra["history"] = history.dump();
            f.extra["best"] = best_src.substr(0, 1000);
            f.extra["bmc"] = scored.proved;
            return {f};
        }
        if (result.value("ok", false) && bmc_st != laws::FAILED) break;
        messages.push_back({"assistant", r.text});
        messages.push_back({"user", result.dump().substr(0, 1500)});
    }
    Finding f;
    f.stage = "repair";
    f.status = best_score >= 3 ? std::string(laws::CLEAN) : std::string(laws::HYPOTHESIS);
    f.message = "RLEF best score " + std::to_string(best_score) + " over " + std::to_string(history.size()) +
                " rounds (not a proof)";
    f.strength = std::string(laws::STRENGTH_READS);
    f.extra["history"] = history.dump();
    f.extra["best"] = best_src.substr(0, 1000);
    return {f};
}

}  // namespace prism
