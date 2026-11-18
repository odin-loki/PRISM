// prism::sim: z3::expr -> term IR, SMT-LIB2 front end through Z3's own parser.

#include "prism/sim/term.hpp"

#include <fstream>
#include <sstream>
#include <unordered_map>

namespace prism::sim {

#ifdef PRISM_HAS_Z3
namespace {

sort_t sort_of(const z3::sort& s) {
    sort_t r;
    if (s.is_bool()) {
        r.kind = sk::boolean;
    } else if (s.is_bv()) {
        r.kind = sk::bv;
        r.width = s.bv_size();
    } else if (s.is_int()) {
        r.kind = sk::integer;
    } else if (s.is_array()) {
        r.kind = sk::array;
        const auto elem = sort_of(s.array_range());
        const auto idx = sort_of(s.array_domain());
        r.width = elem.kind == sk::bv ? elem.width : elem.width;
        r.idx_width = idx.kind == sk::bv ? idx.width : 0;
        r.depth = static_cast<std::uint8_t>((elem.kind == sk::array ? elem.depth : 0) + 1);
    }
    return r;
}

std::uint64_t str_hash(const std::string& s) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ULL;
    return mix64(h);
}

bool parse_decimal(const std::string& s, word& out) {
    bool neg = false;
    std::size_t i = 0;
    if (!s.empty() && s[0] == '-') { neg = true; i = 1; }
    word v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + static_cast<word>(s[i] - '0');
    }
    out = neg ? static_cast<word>(0) - v : v;
    return true;
}

struct converter {
    z3::context& ctx;
    term t;
    std::unordered_map<unsigned, std::uint32_t> memo;
    std::unordered_map<unsigned, std::uint32_t> var_ids;
    std::string why;
    unsigned max_depth;

    std::uint32_t add(node n) {
        t.nodes.push_back(std::move(n));
        return static_cast<std::uint32_t>(t.nodes.size() - 1);
    }

    std::optional<op> map_op(Z3_decl_kind k) {
        switch (k) {
            case Z3_OP_NOT: return op::not_;
            case Z3_OP_AND: return op::and_;
            case Z3_OP_OR: return op::or_;
            case Z3_OP_XOR: return op::xor_;
            case Z3_OP_IMPLIES: return op::implies;
            case Z3_OP_ITE: return op::ite;
            case Z3_OP_EQ: case Z3_OP_IFF: return op::eq;
            case Z3_OP_DISTINCT: return op::distinct;
            case Z3_OP_BNOT: return op::bvnot;
            case Z3_OP_BNEG: return op::bvneg;
            case Z3_OP_BAND: return op::bvand;
            case Z3_OP_BOR: return op::bvor;
            case Z3_OP_BXOR: return op::bvxor;
            case Z3_OP_BNAND: return op::bvnand;
            case Z3_OP_BNOR: return op::bvnor;
            case Z3_OP_BXNOR: return op::bvxnor;
            case Z3_OP_BADD: return op::bvadd;
            case Z3_OP_BSUB: return op::bvsub;
            case Z3_OP_BMUL: return op::bvmul;
            case Z3_OP_BUDIV: case Z3_OP_BUDIV_I: return op::bvudiv;
            case Z3_OP_BUREM: case Z3_OP_BUREM_I: return op::bvurem;
            case Z3_OP_BSDIV: case Z3_OP_BSDIV_I: return op::bvsdiv;
            case Z3_OP_BSREM: case Z3_OP_BSREM_I: return op::bvsrem;
            case Z3_OP_BSMOD: case Z3_OP_BSMOD_I: return op::bvsmod;
            case Z3_OP_BSHL: return op::bvshl;
            case Z3_OP_BLSHR: return op::bvlshr;
            case Z3_OP_BASHR: return op::bvashr;
            case Z3_OP_CONCAT: return op::concat;
            case Z3_OP_EXTRACT: return op::extract;
            case Z3_OP_ZERO_EXT: return op::zext;
            case Z3_OP_SIGN_EXT: return op::sext;
            case Z3_OP_REPEAT: return op::repeat;
            case Z3_OP_ROTATE_LEFT: return op::rotl;
            case Z3_OP_ROTATE_RIGHT: return op::rotr;
            case Z3_OP_ULT: return op::bvult;
            case Z3_OP_ULEQ: return op::bvule;
            case Z3_OP_UGT: return op::bvugt;
            case Z3_OP_UGEQ: return op::bvuge;
            case Z3_OP_SLT: return op::bvslt;
            case Z3_OP_SLEQ: return op::bvsle;
            case Z3_OP_SGT: return op::bvsgt;
            case Z3_OP_SGEQ: return op::bvsge;
            case Z3_OP_BCOMP: return op::bvcomp;
            case Z3_OP_ADD: return op::iadd;
            case Z3_OP_SUB: return op::isub;
            case Z3_OP_MUL: return op::imul;
            case Z3_OP_UMINUS: return op::ineg;
            case Z3_OP_LE: return op::ile;
            case Z3_OP_LT: return op::ilt;
            case Z3_OP_GE: return op::ige;
            case Z3_OP_GT: return op::igt;
            case Z3_OP_IDIV: return op::idiv;
            case Z3_OP_MOD: return op::imod;
            case Z3_OP_SELECT: return op::select;
            case Z3_OP_STORE: return op::store;
            case Z3_OP_CONST_ARRAY: return op::constarr;
            default: return std::nullopt;
        }
    }

    // Postorder, iterative: kernel VCs have let-chains thousands deep.
    std::optional<std::uint32_t> convert(const z3::expr& root) {
        struct frame { z3::expr e; unsigned next; std::vector<std::uint32_t> kids; };
        std::vector<frame> st;
        std::uint32_t result = 0;
        auto finish = [&](frame& f) {
            const z3::expr& e = f.e;
            node n;
            n.s = sort_of(e.get_sort());
            n.kids = std::move(f.kids);
            if (e.is_app()) {
                const auto d = e.decl();
                const auto k = d.decl_kind();
                const Z3_context c = ctx;
                if (k == Z3_OP_TRUE) { n.o = op::tru; }
                else if (k == Z3_OP_FALSE) { n.o = op::fls; }
                else if (k == Z3_OP_BNUM || k == Z3_OP_ANUM) {
                    const std::string txt = Z3_get_numeral_string(c, e);
                    if (k == Z3_OP_BNUM && n.s.width <= kMaxWidth) { n.o = op::lit; parse_decimal(txt, n.val); }
                    else if (k == Z3_OP_ANUM && n.s.kind == sk::integer) { n.o = op::ilit; parse_decimal(txt, n.val); }
                    else { n.o = op::opaque; n.tag = str_hash("num:" + txt); }
                } else if (k == Z3_OP_UNINTERPRETED && e.num_args() == 0) {
                    n.o = op::var;
                    auto it = var_ids.find(e.id());
                    if (it == var_ids.end()) it = var_ids.emplace(e.id(), static_cast<unsigned>(var_ids.size())).first;
                    n.aux = it->second;
                    n.tag = str_hash(d.name().str());  // the variable's name (position-sensitive signature only)
                } else if (auto o = map_op(k)) {
                    n.o = *o;
                    if (*o == op::extract) { n.aux = static_cast<std::uint32_t>(Z3_get_decl_int_parameter(c, d, 0)); n.aux2 = static_cast<std::uint32_t>(Z3_get_decl_int_parameter(c, d, 1)); }
                    else if (*o == op::zext || *o == op::sext || *o == op::repeat || *o == op::rotl || *o == op::rotr)
                        n.aux = static_cast<std::uint32_t>(Z3_get_decl_int_parameter(c, d, 0));
                    // an operator over a sort the evaluator cannot hold stays structural but evaluates opaquely
                } else {
                    n.o = op::opaque;
                    n.tag = str_hash(d.name().str());
                }
            } else {
                n.o = op::opaque;  // quantifier / de Bruijn variable
                n.tag = str_hash("quantifier");
            }
            result = add(std::move(n));
            memo[f.e.id()] = result;
        };
        st.push_back({root, 0, {}});
        while (!st.empty()) {
            auto& f = st.back();
            if (st.size() > max_depth) { why = "term deeper than the limit"; return std::nullopt; }
            const unsigned na = f.e.is_app() ? f.e.num_args() : 0;
            if (f.next < na) {
                z3::expr a = f.e.arg(f.next++);
                auto it = memo.find(a.id());
                if (it != memo.end()) f.kids.push_back(it->second);
                else st.push_back({a, 0, {}});
                continue;
            }
            finish(f);
            st.pop_back();
            if (!st.empty()) st.back().kids.push_back(result);
        }
        return result;
    }
};

}  // namespace

std::optional<term> from_z3(const z3::expr& e, std::string* why, unsigned max_depth) {
    converter cv{e.ctx(), {}, {}, {}, {}, max_depth};
    auto r = cv.convert(e);
    if (!r) {
        if (why) *why = cv.why;
        return std::nullopt;
    }
    cv.t.root = *r;
    cv.t.n_vars = static_cast<std::uint32_t>(cv.var_ids.size());
    return std::move(cv.t);
}

std::optional<term> from_smt2(std::string_view text, std::string* why) {
    try {
        z3::context c;
        const std::string s(text);
        auto v = c.parse_string(s.c_str());
        if (v.size() == 0) {
            if (why) *why = "no assertions";
            return std::nullopt;
        }
        z3::expr f = v.size() == 1 ? v[0] : z3::mk_and(v);
        return from_z3(f, why);
    } catch (const z3::exception& e) {
        if (why) *why = std::string("smt2: ") + e.msg();
        return std::nullopt;
    }
}
#else
std::optional<term> from_smt2(std::string_view, std::string* why) {
    if (why) *why = "built without Z3";
    return std::nullopt;
}
#endif

std::optional<term> from_smt2_file(const std::string& path, std::string* why) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (why) *why = "cannot read " + path;
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return from_smt2(ss.str(), why);
}

}  // namespace prism::sim
