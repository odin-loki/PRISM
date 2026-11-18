// prism::sim canonical form and structural hashes (include/prism/sim/term.hpp).

#include "prism/sim/term.hpp"

#include <algorithm>
#include <unordered_map>

namespace prism::sim {

std::string_view op_name(op o) {
    static const char* const names[] = {
#define X(n) #n,
        PRISM_SIM_OPS(X)
#undef X
    };
    const auto i = static_cast<std::size_t>(o);
    return i < static_cast<std::size_t>(op::count_) ? names[i] : "?";
}

std::uint64_t mix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
std::uint64_t hash_combine(std::uint64_t h, std::uint64_t x) {
    return mix64(h ^ (mix64(x) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)));
}

namespace {

bool is_flat(op o) {
    switch (o) {
        case op::and_: case op::or_: case op::xor_: case op::bvadd: case op::bvmul: case op::bvand:
        case op::bvor: case op::bvxor: case op::iadd: case op::imul: return true;
        default: return false;
    }
}
bool is_ordered(op o) {
    if (is_flat(o)) return true;
    switch (o) {
        case op::eq: case op::distinct: case op::bvnand: case op::bvnor: case op::bvxnor: case op::bvcomp: return true;
        default: return false;
    }
}

bool pow2_or_zero(word v) { return v == 0 || (v & (v - 1)) == 0; }

std::uint64_t sort_hash(const sort_t& s) {
    return hash_combine(hash_combine(static_cast<std::uint64_t>(s.kind) + 1, s.width), hash_combine(s.idx_width, s.depth));
}
std::uint64_t word_hash(word v) {
    std::uint64_t h = hash_combine(0x1234, static_cast<std::uint64_t>(v));
#if PRISM_HAS_INT128
    h = hash_combine(h, static_cast<std::uint64_t>(v >> 64));
#endif
    return h;
}

}  // namespace

std::uint64_t node_label_hash(const node& n) {
    std::uint64_t h = hash_combine(static_cast<std::uint64_t>(n.o) + 7, sort_hash(n.s));
    switch (n.o) {
        case op::lit: case op::ilit: h = hash_combine(h, keeps_literal(n) ? word_hash(n.val) : 0xABCDEFULL); break;
        case op::var: break;
        case op::opaque: h = hash_combine(h, n.tag); break;
        case op::extract: h = hash_combine(h, (static_cast<std::uint64_t>(n.aux) << 32) | n.aux2); break;
        case op::zext: case op::sext: case op::repeat: case op::rotl: case op::rotr: h = hash_combine(h, n.aux); break;
        default: break;
    }
    return h;
}

namespace {

struct builder {
    std::vector<node> nodes;
    std::vector<std::uint64_t> sh;
    std::unordered_map<std::string, std::uint32_t> memo;

    std::uint64_t shape_of(const node& n) const {
        std::uint64_t h = node_label_hash(n);
        std::vector<std::uint64_t> ks;
        ks.reserve(n.kids.size());
        for (auto k : n.kids) ks.push_back(sh[k]);
        if (is_ordered(n.o)) std::sort(ks.begin(), ks.end());
        for (auto k : ks) h = hash_combine(h, k);
        return h;
    }
    std::uint32_t mk(node n) {
        std::string key;
        key.reserve(64 + n.kids.size() * 4);
        auto put = [&](const void* p, std::size_t len) { key.append(static_cast<const char*>(p), len); };
        const auto o = static_cast<std::uint8_t>(n.o);
        const auto kind = static_cast<std::uint8_t>(n.s.kind);
        put(&o, 1);
        put(&kind, 1);
        put(&n.s.width, 4);
        put(&n.s.idx_width, 4);
        put(&n.s.depth, 1);
        put(&n.aux, 4);
        put(&n.aux2, 4);
        put(&n.val, sizeof n.val);
        put(&n.tag, 8);
        if (!n.kids.empty()) put(n.kids.data(), n.kids.size() * 4);
        auto [it, fresh] = memo.try_emplace(std::move(key), static_cast<std::uint32_t>(nodes.size()));
        if (!fresh) return it->second;
        sh.push_back(shape_of(n));
        nodes.push_back(std::move(n));
        return it->second;
    }
};

}  // namespace

bool keeps_literal(const node& n) {
    if (n.o == op::tru || n.o == op::fls) return true;
    if (n.o == op::lit) {
        const word v = n.val;
        if (n.s.width == 0 || n.s.width > kMaxWidth) return false;
        const word all1 = n.s.width == kMaxWidth ? ~word(0) : ((word(1) << n.s.width) - 1);
        return pow2_or_zero(v) || v == all1 || v == (all1 >> 1);  // 0, 1, 2^k (incl. MIN), -1, MAX
    }
    if (n.o == op::ilit) {
        const sword v = static_cast<sword>(n.val);
        return v == 0 || v == -1 || (v > 0 && pow2_or_zero(static_cast<word>(v)));
    }
    return false;
}

std::vector<std::uint64_t> shape_hashes(const term& t) {
    builder b;
    b.sh.reserve(t.nodes.size());
    for (const auto& n : t.nodes) b.sh.push_back(b.shape_of(n));  // variables hash by sort only
    return b.sh;
}

term canonicalise(const term& in) {
    term out;
    if (in.nodes.empty()) return out;
    // ---- phase 1: unwrap trivial and/or, flatten, order; variables keep their original id in aux ----
    builder b1;
    std::vector<std::uint32_t> map(in.nodes.size());
    for (std::size_t i = 0; i < in.nodes.size(); ++i) {
        node n = in.nodes[i];
        for (auto& k : n.kids) k = map[k];
        if ((n.o == op::and_ || n.o == op::or_) && n.kids.size() == 1) {
            map[i] = n.kids[0];
            continue;
        }
        if ((n.o == op::and_ || n.o == op::or_) && n.kids.empty()) {
            node c;
            c.o = n.o == op::and_ ? op::tru : op::fls;
            c.s = n.s;
            map[i] = b1.mk(std::move(c));
            continue;
        }
        if (is_flat(n.o)) {
            std::vector<std::uint32_t> flat;
            for (auto k : n.kids) {
                const node& kn = b1.nodes[k];
                if (kn.o == n.o && kn.s == n.s) flat.insert(flat.end(), kn.kids.begin(), kn.kids.end());
                else flat.push_back(k);
            }
            n.kids = std::move(flat);
        }
        if (is_ordered(n.o))
            std::stable_sort(n.kids.begin(), n.kids.end(), [&](std::uint32_t x, std::uint32_t y) { return b1.sh[x] < b1.sh[y]; });
        map[i] = b1.mk(std::move(n));
    }
    const std::uint32_t root1 = map[in.root];

    // ---- phase 2: alpha-rename by first use in a depth-first walk; rebuild with sharing ----
    builder b2;
    std::unordered_map<std::uint32_t, std::uint32_t> ord;  // phase-1 var node id -> ordinal
    std::vector<std::int64_t> done(b1.nodes.size(), -1);
    struct frame {
        std::uint32_t id;
        std::size_t next;
        std::vector<std::uint32_t> kids;
    };
    std::vector<frame> st;
    auto leaf = [&](std::uint32_t id) -> std::uint32_t {
        const node& n = b1.nodes[id];
        auto it = ord.find(id);
        if (it == ord.end()) it = ord.emplace(id, static_cast<std::uint32_t>(ord.size())).first;
        node v;
        v.o = op::var;
        v.s = n.s;
        v.aux = it->second;
        return b2.mk(std::move(v));
    };
    std::uint32_t result = 0;
    if (b1.nodes[root1].o == op::var) {
        result = leaf(root1);
    } else {
        st.push_back({root1, 0, {}});
        while (!st.empty()) {
            const std::uint32_t id = st.back().id;
            const node& n = b1.nodes[id];
            if (st.back().next < n.kids.size()) {
                const auto k = n.kids[st.back().next++];
                if (done[k] >= 0) {
                    st.back().kids.push_back(static_cast<std::uint32_t>(done[k]));
                } else if (b1.nodes[k].o == op::var) {
                    done[k] = leaf(k);
                    st.back().kids.push_back(static_cast<std::uint32_t>(done[k]));
                } else {
                    st.push_back({k, 0, {}});
                }
                continue;
            }
            node c = n;
            c.kids = std::move(st.back().kids);
            result = b2.mk(std::move(c));
            done[id] = result;
            st.pop_back();
            if (!st.empty()) st.back().kids.push_back(result);
        }
    }
    out.nodes = std::move(b2.nodes);
    out.root = result;
    out.n_vars = static_cast<std::uint32_t>(ord.size());
    out.canonical = true;
    // Nodes are created in postorder, so the root (created last) is the last node,
    // except when the root is a variable seen first: then it is the only node.
    return out;
}

}  // namespace prism::sim
