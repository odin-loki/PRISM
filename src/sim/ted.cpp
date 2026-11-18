// prism::sim bounded tree edit distance (Zhang-Shasha, ordered trees, unit cost).

#include <algorithm>

#include "prism/sim/term.hpp"

namespace prism::sim {
namespace {

struct tree {
    std::vector<std::uint64_t> label;     // postorder
    std::vector<std::size_t> lml;         // leftmost leaf descendant (postorder index)
    std::vector<std::size_t> keyroots;
};

bool expand(const term& t, std::size_t max_nodes, tree& out) {
    // postorder of the tree unfolding of the DAG, iteratively
    struct fr { std::uint32_t id; std::size_t next; std::size_t first_leaf; bool have; };
    std::vector<fr> st;
    st.push_back({t.root, 0, 0, false});
    while (!st.empty()) {
        fr& f = st.back();
        const node& n = t.nodes[f.id];
        if (f.next < n.kids.size()) {
            const auto k = n.kids[f.next++];
            st.push_back({k, 0, 0, false});
            continue;
        }
        if (out.label.size() >= max_nodes) return false;
        const std::size_t idx = out.label.size();
        out.label.push_back(node_label_hash(n));
        out.lml.push_back(f.have ? f.first_leaf : idx);
        const std::size_t lm = out.lml.back();
        st.pop_back();
        if (!st.empty() && !st.back().have) {
            st.back().have = true;
            st.back().first_leaf = lm;
        }
    }
    // keyroots: nodes that have no left sibling sharing their lml, i.e. the last
    // node with a given leftmost leaf
    std::vector<bool> seen(out.label.size(), false);
    for (std::size_t i = out.label.size(); i-- > 0;) {
        if (!seen[out.lml[i]]) {
            seen[out.lml[i]] = true;
            out.keyroots.push_back(i);
        }
    }
    std::sort(out.keyroots.begin(), out.keyroots.end());
    return true;
}

}  // namespace

std::optional<double> tree_edit_distance(const term& a, const term& b, std::size_t max_nodes) {
    if (a.nodes.empty() || b.nodes.empty()) return std::nullopt;
    tree x, y;
    if (!expand(a, max_nodes, x) || !expand(b, max_nodes, y)) return std::nullopt;
    const std::size_t n = x.label.size(), m = y.label.size();
    std::vector<std::vector<int>> td(n, std::vector<int>(m, 0));
    std::vector<std::vector<int>> fd(n + 2, std::vector<int>(m + 2, 0));
    for (auto i : x.keyroots) {
        for (auto j : y.keyroots) {
            const std::size_t li = x.lml[i], lj = y.lml[j];
            // forest distance over [li..i] x [lj..j]; fd index offset by li / lj
            fd[0][0] = 0;
            for (std::size_t di = li; di <= i; ++di) fd[di - li + 1][0] = fd[di - li][0] + 1;
            for (std::size_t dj = lj; dj <= j; ++dj) fd[0][dj - lj + 1] = fd[0][dj - lj] + 1;
            for (std::size_t di = li; di <= i; ++di) {
                for (std::size_t dj = lj; dj <= j; ++dj) {
                    const std::size_t r = di - li + 1, c = dj - lj + 1;
                    const int del = fd[r - 1][c] + 1, ins = fd[r][c - 1] + 1;
                    if (x.lml[di] == li && y.lml[dj] == lj) {
                        const int sub = fd[r - 1][c - 1] + (x.label[di] == y.label[dj] ? 0 : 1);
                        fd[r][c] = std::min({del, ins, sub});
                        td[di][dj] = fd[r][c];
                    } else {
                        const std::size_t pr = x.lml[di] - li, pc = y.lml[dj] - lj;
                        fd[r][c] = std::min({del, ins, fd[pr][pc] + td[di][dj]});
                    }
                }
            }
        }
    }
    return std::min(1.0, static_cast<double>(td[n - 1][m - 1]) / static_cast<double>(std::max(n, m)));
}

}  // namespace prism::sim
