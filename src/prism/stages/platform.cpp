// OS plumbing of the stages: run_argv (CreateProcess / fork+execvp with
// sandbox rlimits) and the plain-HTTP client (WinHTTP / sockets) that the LLM
// engine and src/prism/ai use (ai::http_request_raw).
#include "common.hpp"
#include "../ai/ai_internal.hpp"

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

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
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

}  // namespace

namespace stages_detail {
// limits: rlimits applied in the forked child (Law 9 sandbox for built
// binaries; the caller wraps argv with sandbox::wrap_argv). Default: none.
ProcRun run_argv(const std::vector<std::string>& args, const std::string& input, double timeout_s,
                 const sandbox::Limits& limits) {
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

bool http_ok(const std::string& url, int timeout_ms) {
    return http_request("GET", url, {}, timeout_ms).has_value();
}

}  // namespace stages_detail

namespace ai {
std::optional<std::string> http_request_raw(const std::string& method, const std::string& url,
                                            const std::string& body, int timeout_ms) {
    return http_request(method, url, body, timeout_ms);
}
}  // namespace ai

}  // namespace prism
