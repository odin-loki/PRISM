// prism::sim structural vector, distances, embedding and byte codec.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "prism/sim/symbolic_metric.hpp"

namespace prism::sim {
namespace {

enum : std::uint64_t { F_SHAPE = 1, F_OP = 2, F_WIDTH = 3, F_ARR = 4, F_SIZE = 5, F_VARS = 6 };

std::uint32_t fid(std::uint64_t kind, std::uint64_t a, std::uint64_t b = 0) {
    return static_cast<std::uint32_t>(mix64((kind << 56) ^ hash_combine(a, b)) >> 16);
}

}  // namespace

std::vector<std::pair<std::uint32_t, float>> structural_features(const term& t, const sim_options& opt) {
    std::vector<std::pair<std::uint32_t, float>> raw;
    if (t.nodes.empty()) return raw;
    const unsigned D = std::max(1u, opt.depth);
    // trunc[k][i]: hash of the subtree of node i cut at depth k (k = 1: the node's own label).
    std::vector<std::vector<std::uint64_t>> tr(D + 1, std::vector<std::uint64_t>(t.nodes.size()));
    for (std::size_t i = 0; i < t.nodes.size(); ++i) tr[1][i] = node_label_hash(t.nodes[i]);
    for (unsigned k = 2; k <= D; ++k) {
        for (std::size_t i = 0; i < t.nodes.size(); ++i) {
            const node& n = t.nodes[i];
            std::vector<std::uint64_t> ks;
            ks.reserve(n.kids.size());
            for (auto c : n.kids) ks.push_back(tr[k - 1][c]);
            const bool ordered = n.o == op::and_ || n.o == op::or_ || n.o == op::xor_ || n.o == op::bvadd || n.o == op::bvmul ||
                                 n.o == op::bvand || n.o == op::bvor || n.o == op::bvxor || n.o == op::eq || n.o == op::distinct ||
                                 n.o == op::iadd || n.o == op::imul || n.o == op::bvnand || n.o == op::bvnor ||
                                 n.o == op::bvxnor || n.o == op::bvcomp;
            if (ordered) std::sort(ks.begin(), ks.end());
            std::uint64_t h = tr[1][i];
            for (auto x : ks) h = hash_combine(h, x);
            tr[k][i] = h;
        }
    }
    std::vector<unsigned> opc(static_cast<std::size_t>(op::count_), 0);
    std::vector<std::pair<std::uint32_t, unsigned>> widths;
    unsigned arr_depth = 0;
    for (std::size_t i = 0; i < t.nodes.size() && i <= t.root; ++i) {
        const node& n = t.nodes[i];
        for (unsigned k = 1; k <= D; ++k) raw.emplace_back(fid(F_SHAPE, k, tr[k][i]), 1.0f / static_cast<float>(k));
        ++opc[static_cast<std::size_t>(n.o)];
        if (n.s.kind == sk::bv) raw.emplace_back(fid(F_WIDTH, n.s.width), 0.5f);
        arr_depth = std::max<unsigned>(arr_depth, n.s.depth);
    }
    for (std::size_t o = 0; o < opc.size(); ++o)
        if (opc[o]) raw.emplace_back(fid(F_OP, o), std::log2(1.0f + static_cast<float>(opc[o])));
    if (arr_depth) raw.emplace_back(fid(F_ARR, arr_depth), 1.0f);
    raw.emplace_back(fid(F_SIZE, static_cast<std::uint64_t>(std::log2(1.0 + static_cast<double>(t.nodes.size())) * 2)), 1.0f);
    raw.emplace_back(fid(F_VARS, std::min<std::uint32_t>(t.n_vars, 16)), 1.0f);
    // sum weights of equal ids, sort by id
    std::sort(raw.begin(), raw.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::pair<std::uint32_t, float>> out;
    for (const auto& [id, w] : raw) {
        if (!out.empty() && out.back().first == id) out.back().second += w;
        else out.emplace_back(id, w);
    }
    return out;
}

sym_vec embed(const term& t, const sim_options& opt) {
    const term c = t.canonical ? t : canonicalise(t);
    sym_vec v;
    v.nodes = static_cast<std::uint32_t>(c.nodes.size());
    v.vars = c.n_vars;
    v.feat = structural_features(c, opt);
    auto pr = semantic_profile(c, opt);
    v.sem = std::move(pr.root);
    v.sem_constant = pr.constant;
    v.root_bool = pr.root_is_bool;
    v.sub = std::move(pr.sub);
    v.named = std::move(pr.named);
    return v;
}

std::optional<sym_vec> embed_smt2(std::string_view text, std::string* why, const sim_options& opt) {
    auto t = from_smt2(text, why);
    if (!t) return std::nullopt;
    return embed(*t, opt);
}
std::optional<sym_vec> embed_smt2_file(const std::string& path, std::string* why, const sim_options& opt) {
    auto t = from_smt2_file(path, why);
    if (!t) return std::nullopt;
    return embed(*t, opt);
}

double structural_distance(const sym_vec& a, const sym_vec& b) {
    double mn = 0, mx = 0;
    std::size_t i = 0, j = 0;
    const auto &x = a.feat, &y = b.feat;
    while (i < x.size() || j < y.size()) {
        if (j == y.size() || (i < x.size() && x[i].first < y[j].first)) { mx += x[i].second; ++i; }
        else if (i == x.size() || y[j].first < x[i].first) { mx += y[j].second; ++j; }
        else {
            mn += std::min(x[i].second, y[j].second);
            mx += std::max(x[i].second, y[j].second);
            ++i;
            ++j;
        }
    }
    return mx <= 0 ? 0.0 : std::clamp(1.0 - mn / mx, 0.0, 1.0);
}

namespace {
double hamming_bits(const std::vector<std::uint64_t>& a, const std::vector<std::uint64_t>& b) {
    const std::size_t n = std::max(a.size(), b.size());
    if (n == 0) return 0.0;
    std::size_t diff = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        diff += static_cast<std::size_t>(std::popcount(x ^ y));
    }
    return std::min(1.0, static_cast<double>(diff) / static_cast<double>(64 * n));
}
}  // namespace

double semantic_distance(const sym_vec& a, const sym_vec& b) {
    if (!a.sem_constant || !b.sem_constant || !a.root_bool || !b.root_bool) return hamming_bits(a.sem, b.sem);
    if (a.sub.empty() && b.sub.empty()) return 0.0;
    std::size_t i = 0, j = 0, common = 0;
    while (i < a.sub.size() && j < b.sub.size()) {
        if (a.sub[i] == b.sub[j]) { ++common; ++i; ++j; }
        else if (a.sub[i] < b.sub[j]) ++i;
        else ++j;
    }
    const std::size_t uni = a.sub.size() + b.sub.size() - common;
    return uni == 0 ? 0.0 : 1.0 - static_cast<double>(common) / static_cast<double>(uni);
}

double named_distance(const sym_vec& a, const sym_vec& b) { return hamming_bits(a.named, b.named); }

namespace {
template <class T> void put(std::vector<std::byte>& out, const T& v) {
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    out.insert(out.end(), p, p + sizeof v);
}
template <class T> bool get(const std::byte*& p, const std::byte* end, T& v) {
    if (static_cast<std::size_t>(end - p) < sizeof v) return false;
    std::memcpy(&v, p, sizeof v);
    p += sizeof v;
    return true;
}
}  // namespace

void encode_sym_vec(const sym_vec& v, std::vector<std::byte>& out) {
    put<std::uint32_t>(out, 0x32564d53u);  // "SMV2"
    put(out, v.nodes);
    put(out, v.vars);
    put<std::uint8_t>(out, (v.sem_constant ? 1 : 0) | (v.root_bool ? 2 : 0));
    put<std::uint32_t>(out, static_cast<std::uint32_t>(v.feat.size()));
    for (const auto& [id, w] : v.feat) { put(out, id); put(out, w); }
    put<std::uint32_t>(out, static_cast<std::uint32_t>(v.sem.size()));
    for (auto w : v.sem) put(out, w);
    put<std::uint32_t>(out, static_cast<std::uint32_t>(v.sub.size()));
    for (auto w : v.sub) put(out, w);
    put<std::uint32_t>(out, static_cast<std::uint32_t>(v.named.size()));
    for (auto w : v.named) put(out, w);
}

sym_vec decode_sym_vec(const std::byte* data, std::size_t size) {
    sym_vec v;
    const std::byte* p = data;
    const std::byte* end = data + size;
    std::uint32_t magic = 0, nf = 0, ns = 0;
    std::uint8_t c = 0;
    if (!get(p, end, magic) || magic != 0x32564d53u) return {};
    if (!get(p, end, v.nodes) || !get(p, end, v.vars) || !get(p, end, c) || !get(p, end, nf)) return {};
    v.sem_constant = (c & 1) != 0;
    v.root_bool = (c & 2) != 0;
    if (static_cast<std::size_t>(end - p) < static_cast<std::size_t>(nf) * 8) return {};
    v.feat.resize(nf);
    for (auto& [id, w] : v.feat) { get(p, end, id); get(p, end, w); }
    if (!get(p, end, ns) || static_cast<std::size_t>(end - p) < static_cast<std::size_t>(ns) * 8) return {};
    v.sem.resize(ns);
    for (auto& w : v.sem) get(p, end, w);
    for (auto* vec : {&v.sub, &v.named}) {
        std::uint32_t n = 0;
        if (!get(p, end, n) || static_cast<std::size_t>(end - p) < static_cast<std::size_t>(n) * 8) return {};
        vec->resize(n);
        for (auto& w : *vec) get(p, end, w);
    }
    return v;
}

}  // namespace prism::sim
