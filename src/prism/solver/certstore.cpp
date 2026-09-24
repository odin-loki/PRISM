// The certificate store of the solver cache: packed LRAT proofs, a size cap
// with least-recently-used pruning, and a sweep of work directories left by
// killed solver processes. See include/prism/solver_certs.hpp.
//
// Soundness does not rest on this file: a stored proof is only ever handed
// to the checkers (cake_lpr, Lean's checker) against a CNF bit-blasted again
// from the formula, so a corrupt, truncated or foreign proof can only be
// rejected. The packing is lossless for every well-formed text LRAT file.

#include "prism/solver_certs.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace prism::solver::certs {

namespace {

constexpr char kMagic[] = "PRISMLRATZ2\n";
constexpr std::size_t kMagicLen = sizeof(kMagic) - 1;
constexpr const char* kExt = ".lratz";

std::mutex g_mu;
std::map<std::string, int> g_reading;  // query hash -> fetches in progress

struct File {
    std::FILE* f = nullptr;
    explicit File(const fs::path& p, const char* mode) : f(std::fopen(p.string().c_str(), mode)) {}
    ~File() {
        if (f) std::fclose(f);
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    bool close() {
        const bool ok = f && std::fclose(f) == 0;
        f = nullptr;
        return ok;
    }
};

// Buffered byte reader.
class In {
public:
    explicit In(std::FILE* f) : f_(f), buf_(1u << 20) {}
    int get() {
        if (pos_ == len_) {
            len_ = f_ ? std::fread(buf_.data(), 1, buf_.size(), f_) : 0;
            pos_ = 0;
            if (len_ == 0) return -1;
        }
        return static_cast<unsigned char>(buf_[pos_++]);
    }
    bool error() const { return f_ == nullptr || std::ferror(f_) != 0; }

private:
    std::FILE* f_;
    std::vector<char> buf_;
    std::size_t pos_ = 0, len_ = 0;
};

class Out {
public:
    explicit Out(std::FILE* f) : f_(f) { buf_.reserve(1u << 20); }
    void put(char c) {
        buf_.push_back(c);
        if (buf_.size() >= (1u << 20)) flush();
    }
    void put(std::string_view s) {
        buf_.append(s);
        if (buf_.size() >= (1u << 20)) flush();
    }
    void uvarint(std::uint64_t v) {
        while (v >= 0x80) {
            put(static_cast<char>((v & 0x7f) | 0x80));
            v >>= 7;
        }
        put(static_cast<char>(v));
    }
    void integer(std::int64_t v) {
        char tmp[24];
        char* e = tmp + sizeof tmp;
        char* p = e;
        std::uint64_t u = v < 0 ? 0 - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
        do {
            *--p = static_cast<char>('0' + u % 10);
            u /= 10;
        } while (u);
        if (v < 0) *--p = '-';
        put(std::string_view(p, static_cast<std::size_t>(e - p)));
    }
    bool flush() {
        if (!f_) return false;
        if (!buf_.empty() && std::fwrite(buf_.data(), 1, buf_.size(), f_) != buf_.size()) ok_ = false;
        buf_.clear();
        return ok_;
    }
    bool ok() const { return ok_; }

private:
    std::FILE* f_;
    std::string buf_;
    bool ok_ = true;
};

std::uint64_t zig(std::int64_t v) {
    return (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
}
std::int64_t unzig(std::uint64_t u) { return static_cast<std::int64_t>(u >> 1) ^ -static_cast<std::int64_t>(u & 1); }

bool read_uvarint(In& in, std::uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        const int c = in.get();
        if (c < 0) return false;
        v |= static_cast<std::uint64_t>(c & 0x7f) << shift;
        if (!(c & 0x80)) return true;
    }
    return false;
}

// Text tokenizer for LRAT: integers, the 'd' keyword, newlines, comments.
struct Tok {
    enum Kind { Int, Del, Eol, Eof, Bad } kind = Eof;
    std::int64_t v = 0;
};

class Lexer {
public:
    explicit Lexer(In& in) : in_(in) {}
    Tok next() {
        int c = peek_ ? std::exchange(peek_, 0) : in_.get();
        while (c == ' ' || c == '\t' || c == '\r') c = in_.get();
        Tok t;
        if (c < 0) { t.kind = Tok::Eof; return t; }
        if (c == '\n') { t.kind = Tok::Eol; return t; }
        if (c == 'd') { t.kind = Tok::Del; return t; }
        if (c == 'c') {  // comment line
            while (c >= 0 && c != '\n') c = in_.get();
            t.kind = c < 0 ? Tok::Eof : Tok::Eol;
            return t;
        }
        bool neg = false;
        if (c == '-') { neg = true; c = in_.get(); }
        if (c < '0' || c > '9') { t.kind = Tok::Bad; return t; }
        std::uint64_t u = 0;
        while (c >= '0' && c <= '9') {
            if (u > (std::uint64_t(1) << 62)) { t.kind = Tok::Bad; return t; }
            u = u * 10 + static_cast<std::uint64_t>(c - '0');
            c = in_.get();
        }
        peek_ = c < 0 ? 0 : c;
        t.kind = Tok::Int;
        t.v = neg ? -static_cast<std::int64_t>(u) : static_cast<std::int64_t>(u);
        return t;
    }

private:
    In& in_;
    int peek_ = 0;
};

void set(std::string* why, std::string s) {
    if (why) *why = std::move(s);
}

std::string temp_name(const fs::path& target) {
    static std::atomic<unsigned long long> n{0};
#ifndef _WIN32
    const long long pid = static_cast<long long>(getpid());
#else
    const long long pid = 0;
#endif
    return target.string() + ".tmp." + std::to_string(pid) + "." + std::to_string(n.fetch_add(1));
}

std::string key_of(const fs::path& p) {
    auto name = p.filename().string();
    return name.substr(0, name.find('.'));
}

}  // namespace

bool parse_size(const std::string& s, std::uint64_t& out) {
    if (s.empty()) return false;
    std::size_t i = 0;
    std::uint64_t v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        if (v > (std::uint64_t(1) << 50)) return false;
        v = v * 10 + static_cast<std::uint64_t>(s[i] - '0');
        ++i;
    }
    if (i == 0) return false;
    std::string unit = s.substr(i);
    for (auto& ch : unit) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (!unit.empty() && unit.back() == 'B') unit.pop_back();
    if (!unit.empty() && unit.back() == 'I') unit.pop_back();  // GiB, MiB
    int shift = 0;
    if (unit.empty()) shift = 0;
    else if (unit == "K") shift = 10;
    else if (unit == "M") shift = 20;
    else if (unit == "G") shift = 30;
    else if (unit == "T") shift = 40;
    else return false;
    out = v << shift;
    return true;
}

std::uint64_t cap_bytes(std::int64_t opt_bytes, std::string* why) {
    if (opt_bytes >= 0) {
        set(why, "option");
        return static_cast<std::uint64_t>(opt_bytes);
    }
    if (const char* e = std::getenv("PRISM_CERT_CACHE_MAX"); e && *e) {
        std::uint64_t v = 0;
        if (parse_size(e, v)) {
            set(why, "PRISM_CERT_CACHE_MAX");
            return v;
        }
        set(why, std::string("default (PRISM_CERT_CACHE_MAX=") + e + " is not a size)");
        return kDefaultCapBytes;
    }
    set(why, "default");
    return kDefaultCapBytes;
}

bool pack_lrat(const fs::path& text_in, const fs::path& packed_out, std::string* why) {
    File fi(text_in, "rb");
    if (!fi.f) { set(why, "cannot read " + text_in.string()); return false; }
    File fo(packed_out, "wb");
    if (!fo.f) { set(why, "cannot write " + packed_out.string()); return false; }
    In in(fi.f);
    Out out(fo.f);
    out.put(std::string_view(kMagic, kMagicLen));
    Lexer lx(in);
    std::int64_t prev = 0;
    std::uint64_t line = 0;
    auto bad = [&](const char* what) {
        set(why, "LRAT line " + std::to_string(line + 1) + ": " + what);
        return false;
    };
    for (;;) {
        Tok t = lx.next();
        if (t.kind == Tok::Eof) break;
        if (t.kind == Tok::Eol) { ++line; continue; }
        if (t.kind != Tok::Int || t.v <= 0) return bad("expected a clause id");
        const std::int64_t id = t.v;
        Tok t2 = lx.next();
        if (t2.kind == Tok::Del) {
            out.put('d');
            out.uvarint(zig(id - prev));
            std::int64_t last = id;
            for (;;) {
                Tok x = lx.next();
                if (x.kind != Tok::Int) return bad("unterminated deletion");
                if (x.v == 0) break;
                out.uvarint(zig(x.v - last) + 1);
                last = x.v;
            }
            out.uvarint(0);
        } else if (t2.kind == Tok::Int) {
            out.put('a');
            out.uvarint(zig(id - prev));
            Tok x = t2;
            for (;;) {  // literals
                if (x.kind != Tok::Int) return bad("unterminated clause");
                if (x.v == 0) break;
                out.uvarint(zig(x.v));
                x = lx.next();
            }
            out.uvarint(0);
            std::int64_t last = id;
            for (;;) {  // hints
                x = lx.next();
                if (x.kind != Tok::Int) return bad("unterminated hints");
                if (x.v == 0) break;
                out.uvarint(zig(x.v - last) + 1);
                last = x.v;
            }
            out.uvarint(0);
        } else {
            return bad("expected literals or 'd'");
        }
        prev = id;
        Tok e = lx.next();
        if (e.kind == Tok::Eof) break;
        if (e.kind != Tok::Eol) return bad("trailing tokens");
        ++line;
    }
    if (in.error()) { set(why, "read error on " + text_in.string()); return false; }
    if (!out.flush() || !fo.close()) { set(why, "write error on " + packed_out.string()); return false; }
    return true;
}

bool unpack_lrat(const fs::path& packed_in, const fs::path& text_out, std::string* why) {
    File fi(packed_in, "rb");
    if (!fi.f) { set(why, "cannot read " + packed_in.string()); return false; }
    In in(fi.f);
    for (std::size_t i = 0; i < kMagicLen; ++i)
        if (in.get() != static_cast<unsigned char>(kMagic[i])) {
            set(why, packed_in.string() + " is not a packed LRAT proof");
            return false;
        }
    File fo(text_out, "wb");
    if (!fo.f) { set(why, "cannot write " + text_out.string()); return false; }
    Out out(fo.f);
    std::int64_t prev = 0;
    auto trunc = [&] {
        set(why, packed_in.string() + " is truncated or corrupt");
        return false;
    };
    for (;;) {
        const int tag = in.get();
        if (tag < 0) break;
        std::uint64_t u = 0;
        if ((tag != 'a' && tag != 'd') || !read_uvarint(in, u)) return trunc();
        const std::int64_t id = prev + unzig(u);
        out.integer(id);
        if (tag == 'd') {
            out.put(" d");
            std::int64_t last = id;
            for (;;) {
                if (!read_uvarint(in, u)) return trunc();
                if (u == 0) break;
                last += unzig(u - 1);
                out.put(' ');
                out.integer(last);
            }
            out.put(" 0\n");
        } else {
            for (;;) {
                if (!read_uvarint(in, u)) return trunc();
                if (u == 0) break;
                out.put(' ');
                out.integer(unzig(u));
            }
            out.put(" 0");
            std::int64_t last = id;
            for (;;) {
                if (!read_uvarint(in, u)) return trunc();
                if (u == 0) break;
                last += unzig(u - 1);
                out.put(' ');
                out.integer(last);
            }
            out.put(" 0\n");
        }
        prev = id;
    }
    if (in.error()) { set(why, "read error on " + packed_in.string()); return false; }
    if (!out.flush() || !fo.close()) { set(why, "write error on " + text_out.string()); return false; }
    return true;
}

fs::path proof_path(const fs::path& cache_root, const std::string& query_hash) {
    return cache_root / "certs" / (query_hash + kExt);
}

bool store(const fs::path& cache_root, const std::string& query_hash, const fs::path& lrat, std::uint64_t cap,
           std::string* why) {
    if (cap == 0) { set(why, "the certificate cache cap is 0"); return false; }
    std::error_code ec;
    fs::create_directories(cache_root / "certs", ec);
    const fs::path target = proof_path(cache_root, query_hash);
    const fs::path tmp = temp_name(target);
    if (!pack_lrat(lrat, tmp, why)) {
        fs::remove(tmp, ec);
        return false;
    }
    const auto sz = fs::file_size(tmp, ec);
    if (ec || sz > cap) {
        fs::remove(tmp, ec);
        set(why, "packed proof (" + std::to_string(sz >> 20) + " MiB) is larger than the certificate cache cap (" +
                     std::to_string(cap >> 20) + " MiB)");
        return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        set(why, "cannot store " + target.string());
        return false;
    }
    prune(cache_root, cap, {query_hash});
    return true;
}

bool fetch(const fs::path& cache_root, const std::string& query_hash, const fs::path& text_out, std::string* why) {
    {
        std::lock_guard<std::mutex> g(g_mu);
        ++g_reading[query_hash];
    }
    struct Release {
        std::string h;
        ~Release() {
            std::lock_guard<std::mutex> g(g_mu);
            if (--g_reading[h] <= 0) g_reading.erase(h);
        }
    } release{query_hash};
    const fs::path p = proof_path(cache_root, query_hash);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        set(why, "the stored proof is gone (pruned by the certificate cache cap)");
        return false;
    }
    // Least recently used = oldest mtime: mark it used before unpacking.
    fs::last_write_time(p, fs::file_time_type::clock::now(), ec);
    // Unpacking reads an open file: a concurrent prune in another process
    // unlinks the name only, and the checkers read the private copy.
    return unpack_lrat(p, text_out, why);
}

PruneStats prune(const fs::path& cache_root, std::uint64_t cap, const std::set<std::string>& keep) {
    PruneStats st;
    std::error_code ec;
    const fs::path dir = cache_root / "certs";
    if (!fs::is_directory(dir, ec)) return st;
    struct Item {
        fs::path p;
        std::uint64_t size;
        fs::file_time_type mtime;
        bool stale_tmp;
    };
    std::vector<Item> items;
    const auto now = fs::file_time_type::clock::now();
    std::set<std::string> busy = keep;
    {
        std::lock_guard<std::mutex> g(g_mu);
        for (auto& [h, n] : g_reading) busy.insert(h);
    }
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const auto sz = it->file_size(e2);
        if (e2) continue;
        const auto mt = it->last_write_time(e2);
        if (e2) continue;
        st.bytes_before += sz;
        const auto name = it->path().filename().string();
        const bool tmp = name.find(".tmp.") != std::string::npos;
        if (tmp) {
            // A write in progress (maybe in another process) is left alone;
            // one older than an hour was interrupted.
            if (now - mt < std::chrono::hours(1)) continue;
            items.push_back({it->path(), sz, mt, true});
            continue;
        }
        if (busy.count(key_of(it->path()))) continue;
        items.push_back({it->path(), sz, mt, false});
    }
    st.bytes_after = st.bytes_before;
    const bool over = st.bytes_before > cap;
    // Stale temp files first, then the least recently used.
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.stale_tmp != b.stale_tmp) return a.stale_tmp;
        return a.mtime < b.mtime;
    });
    const std::uint64_t target = cap - cap / 5;  // prune to 80% of the cap
    for (auto& i : items) {
        if (!i.stale_tmp && (!over || st.bytes_after <= target)) break;
        std::error_code e3;
        if (fs::remove(i.p, e3)) {
            st.bytes_after -= std::min(st.bytes_after, i.size);
            ++st.removed;
        }
    }
    return st;
}

std::size_t sweep_orphan_work_dirs() {
#ifdef _WIN32
    return 0;
#else
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) return 0;
    std::size_t n = 0;
    const long long self = static_cast<long long>(getpid());
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.rfind("prism-solve-", 0) != 0) continue;
        // prism-solve-<hash12>-<pid>-<n>
        const auto last = name.rfind('-');
        if (last == std::string::npos || last == 0) continue;
        const auto prev = name.rfind('-', last - 1);
        if (prev == std::string::npos || prev < 12) continue;
        const std::string pid_s = name.substr(prev + 1, last - prev - 1);
        if (pid_s.empty() || pid_s.find_first_not_of("0123456789") != std::string::npos || pid_s.size() > 9) continue;
        const long long pid = std::stoll(pid_s);
        if (pid <= 1 || pid == self) continue;
        // Only a process that certainly does not exist (ESRCH); EPERM means
        // it exists under another user.
        if (kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH) continue;
        std::error_code e2;
        std::error_code e3;
        if (!it->is_directory(e3) || it->is_symlink(e3)) continue;
        if (fs::remove_all(it->path(), e2) > 0 && !e2) ++n;
    }
    return n;
#endif
}

}  // namespace prism::solver::certs
