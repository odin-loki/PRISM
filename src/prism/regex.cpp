#define PCRE2_CODE_UNIT_WIDTH 8
#include "prism/regex.hpp"

#include <pcre2.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace prism {

// What Regex::code_ points to (private to this file; the header only tests
// it for null). `code` is the interpreted pattern every match can fall back
// to. A pattern matched often gets a JIT-compiled copy (`jit`), made once
// by the thread that crosses kJitAfter and published with release order;
// the original is never modified, so matching threads never race with the
// JIT compile. A JIT match that ends in an error (JIT stack, match limit) is
// re-run on the interpreter, so an error result is the interpreter's.
namespace {

constexpr std::uint32_t kJitAfter = 64;

struct Compiled {
    std::atomic<std::uint32_t> refs{1};  // Regex objects + the compile cache holding it
    pcre2_code* code = nullptr;
    std::uint32_t groups = 1;  // capture count + 1: the ovector pairs a match fills
    std::atomic<std::uint32_t> uses{0};
    std::atomic<pcre2_code*> jit{nullptr};
    std::once_flag jit_once;

    ~Compiled() {
        if (auto* j = jit.load(std::memory_order_acquire)) pcre2_code_free(j);
        pcre2_code_free(code);
    }
};

Compiled* compiled(void* p) { return static_cast<Compiled*>(p); }

void release(Compiled* c) {
    if (c && c->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete c;
}

void free_code(void* p) { release(compiled(p)); }

// Checkers build patterns per variable (`\b` + v + `\s*->`) on every call;
// each thread keeps the compiled code of the patterns it built, shared by
// reference with the Regex objects made from them (code is read-only once
// compiled). A failed compile is cached too (nullptr). A thread that sees
// many distinct patterns starts over.
struct CompileCache {
    std::unordered_map<std::string, Compiled*> map;
    void clear() {
        for (auto& [k, c] : map) release(c);
        map.clear();
    }
    ~CompileCache() { clear(); }
};

constexpr std::size_t kCompileCacheMax = 1024;

Compiled* compile_pattern(const std::string& pattern, std::uint32_t opts) {
    int err = 0;
    PCRE2_SIZE erroff = 0;
    pcre2_code* code =
        pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.c_str()), pattern.size(), opts, &err, &erroff, nullptr);
    if (!code) return nullptr;
    auto* c = new Compiled;
    c->code = code;
    std::uint32_t caps = 0;
    pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &caps);
    c->groups = caps + 1;
    return c;
}

// The code to match with: the JIT copy once there is one.
pcre2_code* code_for(Compiled& c) {
    if (auto* j = c.jit.load(std::memory_order_acquire)) return j;
    if (c.uses.fetch_add(1, std::memory_order_relaxed) + 1 < kJitAfter) return c.code;
    std::call_once(c.jit_once, [&c] {
        pcre2_code* copy = pcre2_code_copy(c.code);
        if (!copy) return;
        if (pcre2_jit_compile(copy, PCRE2_JIT_COMPLETE) != 0) {
            pcre2_code_free(copy);  // no JIT here (unsupported, no exec memory): interpreter only
            return;
        }
        c.jit.store(copy, std::memory_order_release);
    });
    if (auto* j = c.jit.load(std::memory_order_acquire)) return j;
    return c.code;
}

// One match data per thread, grown to the largest pattern seen. A match
// fills `groups` pairs and pcre2_match sets the later pairs of a larger
// ovector to unset, so reading `groups` pairs gives what a match data
// sized from the pattern gives. Reusing it also keeps the interpreter's
// heap frames between calls.
pcre2_match_data* match_data(std::uint32_t groups) {
    struct Holder {
        pcre2_match_data* md = nullptr;
        std::uint32_t pairs = 0;
        ~Holder() { pcre2_match_data_free(md); }
    };
    thread_local Holder h;
    if (h.pairs < groups) {
        auto pairs = std::max<std::uint32_t>(groups, 32);
        auto* md = pcre2_match_data_create(pairs, nullptr);
        if (!md) return nullptr;
        pcre2_match_data_free(h.md);
        h.md = md;
        h.pairs = pairs;
    }
    return h.md;
}

// pcre2_match on the JIT copy when there is one; an error there is re-run
// on the interpreter. The match is left in `md`.
// True when every byte is below 0x80. ASCII text is valid UTF-8, so the
// match may skip PCRE2's own UTF-8 check (which reads the subject a byte at
// a time on every call: the syntax gates run a few hundred patterns over
// each function body). Any other subject keeps the check.
bool all_ascii(std::string_view s) {
    const char* p = s.data();
    std::size_t n = s.size(), i = 0;
    constexpr std::uint64_t hi = 0x8080808080808080ull;
    for (; i + 32 <= n; i += 32) {
        std::uint64_t a, b, c, d;
        std::memcpy(&a, p + i, 8);
        std::memcpy(&b, p + i + 8, 8);
        std::memcpy(&c, p + i + 16, 8);
        std::memcpy(&d, p + i + 24, 8);
        if ((a | b | c | d) & hi) return false;
    }
    for (; i + 8 <= n; i += 8) {
        std::uint64_t a;
        std::memcpy(&a, p + i, 8);
        if (a & hi) return false;
    }
    for (; i < n; ++i)
        if (static_cast<unsigned char>(p[i]) & 0x80) return false;
    return true;
}

int run_match(Compiled& c, std::string_view s, std::size_t offset, std::uint32_t options,
              pcre2_match_data* md) {
    // From offset 0 only: past it PCRE2 checks just the part a match can
    // reach (finditer's later calls), cheaper than a scan of the whole subject.
    if (offset == 0 && all_ascii(s)) options |= PCRE2_NO_UTF_CHECK;
    auto* code = code_for(c);
    int rc = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(s.data()), s.size(), offset, options, md, nullptr);
    if (rc < PCRE2_ERROR_NOMATCH && code != c.code)
        rc = pcre2_match(c.code, reinterpret_cast<PCRE2_SPTR>(s.data()), s.size(), offset, options, md,
                         nullptr);
    return rc;
}

}  // namespace

Regex::Regex(std::string pattern, bool multiline, bool dotall) : pattern_(std::move(pattern)) {
    std::uint32_t opts = PCRE2_UTF | PCRE2_UCP;
    if (multiline) opts |= PCRE2_MULTILINE;
    if (dotall) opts |= PCRE2_DOTALL;
    thread_local CompileCache cache;
    std::string key = pattern_;
    key.push_back(static_cast<char>('0' + (multiline ? 1 : 0) + (dotall ? 2 : 0)));
    Compiled* c = nullptr;
    if (auto it = cache.map.find(key); it != cache.map.end()) {
        c = it->second;
    } else {
        if (cache.map.size() >= kCompileCacheMax) cache.clear();
        c = compile_pattern(pattern_, opts);
        cache.map.emplace(std::move(key), c);  // the cache's reference
    }
    if (!c) return;
    c->refs.fetch_add(1, std::memory_order_relaxed);
    code_ = c;
}

Regex::Regex(Regex&& o) noexcept : pattern_(std::move(o.pattern_)), code_(o.code_) {
    o.code_ = nullptr;
}

Regex& Regex::operator=(Regex&& o) noexcept {
    if (this != &o) {
        free_code(code_);
        pattern_ = std::move(o.pattern_);
        code_ = o.code_;
        o.code_ = nullptr;
    }
    return *this;
}

Regex::~Regex() { free_code(code_); }

static void fill_named(Match& m, pcre2_code* code, pcre2_match_data* md) {
    uint32_t namecount = 0;
    pcre2_pattern_info(code, PCRE2_INFO_NAMECOUNT, &namecount);
    if (!namecount) return;
    uint32_t entry_size = 0;
    pcre2_pattern_info(code, PCRE2_INFO_NAMEENTRYSIZE, &entry_size);
    PCRE2_SPTR table = nullptr;
    pcre2_pattern_info(code, PCRE2_INFO_NAMETABLE, &table);
    for (uint32_t i = 0; i < namecount; ++i) {
        auto* entry = table + i * entry_size;
        int n = (entry[0] << 8) | entry[1];
        const char* name = reinterpret_cast<const char*>(entry + 2);
        PCRE2_UCHAR* buf = nullptr;
        PCRE2_SIZE len = 0;
        if (pcre2_substring_get_bynumber(md, static_cast<uint32_t>(n), &buf, &len) == 0) {
            m.named_groups[name] = std::string(reinterpret_cast<char*>(buf), len);
            pcre2_substring_free(buf);
        }
    }
}

// The first `n` ovector pairs of `md` (n = capture count + 1).
static Match from_ovector(std::string_view s, pcre2_match_data* md, pcre2_code* code, uint32_t n) {
    Match m;
    auto* ovec = pcre2_get_ovector_pointer(md);
    m.groups.resize(n);
    m.spans.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        auto a = ovec[2 * i];
        auto b = ovec[2 * i + 1];
        m.spans[i] = {static_cast<int>(a == PCRE2_UNSET ? -1 : a),
                      static_cast<int>(b == PCRE2_UNSET ? -1 : b)};
        if (a == PCRE2_UNSET) {
            m.groups[i] = std::nullopt;
        } else {
            m.groups[i] = std::string(s.substr(static_cast<std::size_t>(a),
                                               static_cast<std::size_t>(b - a)));
        }
    }
    if (!m.groups.empty() && m.groups[0]) m.text = *m.groups[0];
    fill_named(m, code, md);
    return m;
}

std::string Match::named(std::string_view name) const {
    auto it = named_groups.find(std::string(name));
    return it == named_groups.end() ? std::string{} : it->second;
}

std::string Match::group(std::size_t i) const {
    if (i >= groups.size() || !groups[i]) return {};
    return *groups[i];
}

// search and match_line answer from the ovector without building a Match.
bool Regex::search(std::string_view s) const {
    if (!code_) return false;
    auto& c = *compiled(code_);
    auto* md = match_data(c.groups);
    if (!md) return false;
    return run_match(c, s, 0, 0, md) >= 0;
}

std::optional<Match> Regex::search_match(std::string_view s, std::size_t offset) const {
    if (!code_ || offset > s.size()) return std::nullopt;
    auto& c = *compiled(code_);
    auto* md = match_data(c.groups);
    if (!md) return std::nullopt;
    std::optional<Match> out;
    if (run_match(c, s, offset, 0, md) >= 0) out = from_ovector(s, md, c.code, c.groups);
    return out;
}

std::optional<Match> Regex::match_prefix(std::string_view s) const {
    if (!code_) return std::nullopt;
    auto& c = *compiled(code_);
    auto* md = match_data(c.groups);
    if (!md) return std::nullopt;
    std::optional<Match> out;
    // PCRE2_ANCHORED at match time is not a JIT option: pcre2_match uses the
    // interpreter for it either way.
    if (run_match(c, s, 0, PCRE2_ANCHORED, md) >= 0) out = from_ovector(s, md, c.code, c.groups);
    return out;
}

std::vector<Match> Regex::finditer(std::string_view s) const {
    std::vector<Match> out;
    if (!code_) return out;
    std::size_t off = 0;
    while (off <= s.size()) {
        auto m = search_match(s, off);
        if (!m) break;
        out.push_back(*m);
        auto end = static_cast<std::size_t>(m->spans.empty() ? off + 1 : std::max(0, m->spans[0].second));
        if (end <= off) end = off + 1;
        off = end;
        if (off > s.size()) break;
    }
    return out;
}

bool Regex::match_line(std::string_view s) const {
    // The leftmost match starts at 0.
    if (!code_) return false;
    auto& c = *compiled(code_);
    auto* md = match_data(c.groups);
    if (!md) return false;
    if (run_match(c, s, 0, 0, md) < 0) return false;
    return pcre2_get_ovector_pointer(md)[0] == 0;
}

// The compiled pattern for re_search / re_search_match. Callers pass the
// same few hundred literal patterns over and over (the unencoded-syntax
// gates run per function per stage), so each thread keeps its compiled
// copies; a thread that sees many distinct patterns starts over.
static const Regex& cached_regex(std::string_view pattern, bool multiline) {
    thread_local std::unordered_map<std::string, std::unique_ptr<Regex>> cache;
    std::string key(pattern);
    key.push_back(multiline ? '\1' : '\0');
    if (auto it = cache.find(key); it != cache.end()) return *it->second;
    if (cache.size() >= 4096) cache.clear();
    auto re = std::make_unique<Regex>(std::string(pattern), multiline);
    return *cache.emplace(std::move(key), std::move(re)).first->second;
}

bool re_search(std::string_view pattern, std::string_view s, bool multiline) {
    return cached_regex(pattern, multiline).search(s);
}

std::optional<Match> re_search_match(std::string_view pattern, std::string_view s) {
    return cached_regex(pattern, false).search_match(s);
}

}  // namespace prism
