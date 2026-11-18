// leandb text vectors and the global-statistics BM25 function.

#include <cctype>
#include <cmath>

#include "internal.hpp"
#include "prism/sim/term.hpp"  // mix64

namespace prism::leandb::detail {

const char* tbl_name(int t) {
    switch (t) {
        case 0: return "decls";
        case 1: return "problems";
        default: return "trace_theorems";
    }
}
int tbl_code(std::string_view n) { return n == "decls" ? 0 : n == "problems" ? 1 : 2; }
int use_class(std::string_view u) { return u == "library" ? 0 : u == "data" ? 1 : 2; }

std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
    };
    for (unsigned char ch : text) {
        if (std::isalnum(ch)) cur += static_cast<char>(std::tolower(ch));
        else if (ch >= 0x80) cur += static_cast<char>(ch);  // keep non-ASCII symbols inside a word
        else flush();
    }
    flush();
    return out;
}

std::array<std::uint64_t, 4> simhash(std::string_view text) {
    int acc[256] = {0};
    std::uint64_t h0 = 0;
    for (const auto& w : words(text)) {
        std::uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : w) h = (h ^ c) * 0x100000001b3ULL;
        for (int part = 0; part < 4; ++part) {
            const std::uint64_t x = prism::sim::mix64(h + static_cast<std::uint64_t>(part) * 0x9e3779b97f4a7c15ULL);
            for (int b = 0; b < 64; ++b) acc[part * 64 + b] += ((x >> b) & 1) ? 1 : -1;
        }
        h0 ^= h;
    }
    (void)h0;
    std::array<std::uint64_t, 4> out{};
    for (int i = 0; i < 256; ++i)
        if (acc[i] > 0) out[static_cast<std::size_t>(i / 64)] |= std::uint64_t{1} << (i % 64);
    return out;
}

namespace {
thread_local const bm25_stats* g_stats = nullptr;

// FTS5's bm25(), line for line, with N, avgdl and each phrase's document frequency taken from g_stats.
void bm25_fn(const Fts5ExtensionApi* api, Fts5Context* ctx, sqlite3_context* out, int nVal, sqlite3_value** apVal) {
    const double k1 = 1.2, b = 0.75;
    const bm25_stats* st = g_stats;
    if (!st) { sqlite3_result_error(out, "prism_bm25: no statistics set", -1); return; }
    const int nPhrase = api->xPhraseCount(ctx);
    const int nCol = api->xColumnCount(ctx);
    std::vector<double> w(static_cast<std::size_t>(nCol), 1.0), tf(static_cast<std::size_t>(nPhrase), 0.0);
    for (int i = 0; i < nCol && i < nVal; ++i) w[static_cast<std::size_t>(i)] = sqlite3_value_double(apVal[i]);
    int nInst = 0;
    api->xInstCount(ctx, &nInst);
    for (int i = 0; i < nInst; ++i) {
        int ip = 0, ic = 0, io = 0;
        api->xInst(ctx, i, &ip, &ic, &io);
        tf[static_cast<std::size_t>(ip)] += w[static_cast<std::size_t>(ic)];
    }
    int D = 0;
    api->xColumnSize(ctx, -1, &D);
    double score = 0;
    for (int i = 0; i < nPhrase; ++i) {
        const double nHit = static_cast<std::size_t>(i) < st->df.size() ? st->df[static_cast<std::size_t>(i)] : 0.0;
        double idf = std::log((st->n_docs - nHit + 0.5) / (nHit + 0.5));
        if (idf <= 0.0) idf = 1e-6;
        score += idf * ((tf[static_cast<std::size_t>(i)] * (k1 + 1.0)) /
                        (tf[static_cast<std::size_t>(i)] + k1 * (1.0 - b + b * D / st->avg_len)));
    }
    sqlite3_result_double(out, -1.0 * score);
}
}  // namespace

void set_bm25_stats(const bm25_stats* s) { g_stats = s; }

void register_bm25(forge::db::connection& c) {
    fts5_api* api = nullptr;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(c.handle(), "SELECT fts5(?1)", -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_pointer(st, 1, &api, "fts5_api_ptr", nullptr);
    sqlite3_step(st);
    sqlite3_finalize(st);
    if (api) api->xCreateFunction(api, "prism_bm25", nullptr, &bm25_fn, nullptr);
}

}  // namespace prism::leandb::detail
