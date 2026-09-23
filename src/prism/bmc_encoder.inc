// Typed bitvector encoder for the scalar C subset. Included from bmc.cpp
// (inside its anonymous namespace, under PRISM_HAS_Z3). The Python engine
// mirrors this file in prism/bmc.py; both must give identical verdicts.
//
// Semantics (C11, LP64 x86-64; docs/CONFORMANCE.md S1-S7, F1):
//  * every value carries its C type (width, signedness): TV;
//  * integer promotions (6.3.1.1) and the usual arithmetic conversions
//    (6.3.1.8) are applied before every arithmetic operator and comparison;
//  * signed + - * overflow is checked in both directions, / and % check
//    zero and MIN / -1, << checks the count, a negative left operand and an
//    unrepresentable result, >> takes its type from the promoted left operand;
//  * &&, || and ?: evaluate their right operands only under their condition;
//  * break / continue / early loop exits merge their states (no path is
//    silently dropped); loops are unrolled (bounded) or, for the unbounded
//    proof, havocked (every variable the loop assigns is made arbitrary);
//  * anything not modelled (unknown identifier, unexpanded macro, call to a
//    function that is neither inlined nor modelled) is never a proof: it is
//    UNENCODED (NEEDS-HARNESS), and call arguments are still checked.

struct Prop {
    std::string name;
    std::string cls;
    z3::expr cond;
    int loc;
};

struct CType {
    int w = WIDTH;
    bool u = false;
};

// C type of a type name (parameter, declaration, cast). nullopt: not a
// scalar integer type the encoder models (pointer, typedef, enum, float...).
std::optional<CType> ctype_parse(std::string_view typ_in) {
    std::string t = to_lower_copy(std::string(typ_in));
    if (t.find('*') != std::string::npos || t.find('[') != std::string::npos ||
        t.find('&') != std::string::npos || t.find('(') != std::string::npos)
        return std::nullopt;
    t = rx_sub("\\b(?:const|volatile|register|auto|static|extern|inline|restrict|__restrict|__restrict__)\\b", " ", t);
    std::vector<std::string> words;
    {
        std::stringstream ss(t);
        std::string wd;
        while (ss >> wd) words.push_back(wd);
    }
    if (words.empty()) return std::nullopt;
    static const std::map<std::string, CType> NAMED = {
        {"_bool", {1, true}}, {"bool", {1, true}},
        {"int8_t", {8, false}}, {"uint8_t", {8, true}},
        {"int16_t", {16, false}}, {"uint16_t", {16, true}},
        {"int32_t", {32, false}}, {"uint32_t", {32, true}},
        {"int64_t", {64, false}}, {"uint64_t", {64, true}},
        {"size_t", {64, true}}, {"ssize_t", {64, false}},
        {"ptrdiff_t", {64, false}}, {"intptr_t", {64, false}},
        {"uintptr_t", {64, true}}, {"intmax_t", {64, false}},
        {"uintmax_t", {64, true}},
        {"u_char", {8, true}}, {"u_short", {16, true}},
        {"u_int", {32, true}}, {"u_long", {64, true}},
        {"wchar_t", {32, false}}, {"char8_t", {8, true}},
        {"char16_t", {16, true}}, {"char32_t", {32, true}},
    };
    if (words.size() == 1) {
        auto it = NAMED.find(words[0]);
        if (it != NAMED.end()) return it->second;
    }
    bool is_u = false, is_s = false;
    int n_long = 0, n_short = 0, n_char = 0, n_int = 0;
    for (auto& wd : words) {
        if (wd == "unsigned") is_u = true;
        else if (wd == "signed") is_s = true;
        else if (wd == "long") ++n_long;
        else if (wd == "short") ++n_short;
        else if (wd == "char") ++n_char;
        else if (wd == "int") ++n_int;
        else return std::nullopt;
    }
    if ((is_u && is_s) || n_long > 2 || n_short > 1 || n_char > 1 || n_int > 1) return std::nullopt;
    if (n_char && (n_long || n_short || n_int)) return std::nullopt;
    if (n_short && n_long) return std::nullopt;
    if (n_char) return CType{8, is_u};
    if (n_short) return CType{16, is_u};
    if (n_long) return CType{64, is_u};
    return CType{32, is_u};
}

CType ctype_of(std::string_view typ) {
    if (auto t = ctype_parse(typ)) return *t;
    return CType{WIDTH, type_is_unsigned(typ)};
}

// A typed value: a bitvector of width w holding a value of C type (w, u).
struct TV {
    z3::expr v;
    int w = WIDTH;
    bool u = false;
};

struct Arr {
    z3::expr a;
    int n;
    int w = WIDTH;
    bool u = false;
};

struct State {
    z3::expr path;
    std::map<std::string, z3::expr> vars;
    std::map<std::string, Arr> arrays;
    std::map<std::string, z3::expr> uninit;
};

// Pending jumps of one loop (break/continue) or switch (break).
struct Jump {
    bool loop = true;
    std::vector<State> breaks;
    std::vector<State> conts;
};

struct Enc {
    int unwind = 8;
    z3::context ctx;
    z3::solver s;
    std::map<std::string, z3::expr> vars;
    std::map<std::string, Arr> arrays;
    std::map<std::string, std::string> alias;  // pointer -> array it aliases
    // name -> Bool, true = maybe uninit. "@a" -> Array(BV, Bool): the
    // per-element shadow of local array a (true = element maybe unwritten).
    std::map<std::string, z3::expr> uninit;
    std::vector<std::string> value_arrays;  // arrays used as values (escape to calls)
    std::vector<Prop> props;
    int pc = 0;
    int fresh = 0;
    bool unwind_ok = true;
    z3::expr path_true;
    std::unordered_set<std::string> unsigned_names;
    std::map<std::string, int> bits;  // declared (visible) scalars -> width
    std::vector<std::string> unmodelled;  // calls whose callee is not modelled
    std::vector<z3::expr> call_vars;      // their unconstrained results
    std::vector<Jump> jumps;
    std::vector<std::vector<std::string>> scopes;
    bool havoc = false;  // loops are havocked (unbounded step) instead of unrolled

    explicit Enc(int u)
        : unwind(u), s(ctx), path_true(ctx.bool_val(true)) {
        s.set("timeout", 8000u);
    }

    void retag_unsigned() {}

    CType type_of(const std::string& n) const {
        auto it = bits.find(n);
        return CType{it == bits.end() ? WIDTH : it->second, unsigned_names.count(n) != 0};
    }
    std::string canonical(const std::string& n) const {
        auto it = alias.find(n);
        return it == alias.end() ? n : it->second;
    }
    bool is_array(const std::string& n) const { return arrays.count(canonical(n)) != 0; }
    bool declared(const std::string& n) const {
        return bits.count(n) || arrays.count(n) || alias.count(n);
    }
    void note_scope(const std::string& n) {
        if (!scopes.empty()) scopes.back().push_back(n);
    }
    void declare(const std::string& n, CType t) {
        bits[n] = t.w;
        if (t.u) unsigned_names.insert(n);
        else unsigned_names.erase(n);
        note_scope(n);
    }
    void declare_array(const std::string& n, Arr a) {
        arrays.insert_or_assign(n, std::move(a));
        note_scope(n);
    }
    void declare_alias(const std::string& n, const std::string& target) {
        alias[n] = canonical(target);
        note_scope(n);
    }
    void push_scope() { scopes.emplace_back(); }
    void pop_scope() {
        if (scopes.empty()) return;
        for (auto& n : scopes.back()) {
            bits.erase(n);
            unsigned_names.erase(n);
            vars.erase(n);
            uninit.erase(n);
            uninit.erase("@" + n);
            arrays.erase(n);
            alias.erase(n);
        }
        scopes.pop_back();
    }

    z3::expr bv(std::string name = {}, int width = 0) {
        fresh += 1;
        int w = width ? width : WIDTH;
        if (name.empty()) name = "t" + std::to_string(fresh);
        return ctx.bv_const(name.c_str(), static_cast<unsigned>(w));
    }

    TV get(const std::string& name) {
        auto t = type_of(name);
        if (!vars.count(name)) vars.emplace(name, bv(name, t.w));
        return TV{vars.at(name), t.w, t.u};
    }

    TV conv(const TV& x, CType t) {
        if (t.w == 1) {
            if (x.w == 1) return TV{x.v, 1, true};
            return TV{z3::ite(x.v != bv_zero(x.w), ctx.bv_val(1, 1), ctx.bv_val(0, 1)), 1, true};
        }
        z3::expr v = x.v;
        if (x.w < t.w)
            v = x.u ? z3::zext(v, static_cast<unsigned>(t.w - x.w))
                    : z3::sext(v, static_cast<unsigned>(t.w - x.w));
        else if (x.w > t.w)
            v = v.extract(static_cast<unsigned>(t.w - 1), 0);
        return TV{v, t.w, t.u};
    }

    // Integer promotions (C11 6.3.1.1p2): every type narrower than int
    // (including unsigned char/short and _Bool) becomes int.
    TV promote(const TV& x) {
        if (x.w < 32) return conv(x, CType{32, false});
        return x;
    }

    // Usual arithmetic conversions (C11 6.3.1.8) after promotion.
    CType common(const TV& a, const TV& b) const {
        int wa = std::max(a.w, 32), wb = std::max(b.w, 32);
        bool ua = a.w >= 32 && a.u, ub = b.w >= 32 && b.u;
        if (ua == ub) return CType{std::max(wa, wb), ua};
        int uw = ua ? wa : wb, sw = ua ? wb : wa;
        if (uw >= sw) return CType{uw, true};
        return CType{sw, false};
    }

    void set(const std::string& name, const TV& val) {
        vars.insert_or_assign(name, conv(val, type_of(name)).v);
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

    State snap() const { return State{path_true, vars, arrays, uninit}; }
    void load(const State& st) {
        path_true = st.path;
        vars = st.vars;
        arrays = st.arrays;
        uninit = st.uninit;
    }

    z3::expr bv_min(int w) {
        if (w >= 64) return ctx.bv_val(static_cast<uint64_t>(1) << 63, static_cast<unsigned>(w));
        int64_t v = -(int64_t{1} << (w - 1));
        return ctx.bv_val(v, static_cast<unsigned>(w));
    }
    z3::expr bv_zero(int w) { return ctx.bv_val(0, static_cast<unsigned>(w)); }
    z3::expr bv_val_w(int64_t v, int w = WIDTH) {
        return ctx.bv_val(v, static_cast<unsigned>(w));
    }
    TV int_val(int64_t v) { return TV{ctx.bv_val(v, 32), 32, false}; }
};

z3::expr as_bool(const TV& x) {
    return x.v != x.v.ctx().bv_val(0, static_cast<unsigned>(x.w));
}

TV bool_tv(Enc& e, const z3::expr& b) {
    return TV{z3::ite(b, e.ctx.bv_val(1, 32), e.ctx.bv_val(0, 32)), 32, false};
}

// Merge disjoint states (each selected by its own path condition) into one.
// Only names visible in `base` (the state before the construct) survive:
// anything declared inside the construct is out of scope afterwards.
State merge_states(Enc& e, const std::vector<State>& states, const State& base) {
    std::vector<const State*> live;
    for (auto& st : states)
        if (!st.path.is_false()) live.push_back(&st);
    State out = base;
    if (live.empty()) {
        out.path = e.ctx.bool_val(false);
        return out;
    }
    if (live.size() == 1) {
        out.path = live[0]->path;
    } else {
        z3::expr_vector ps(e.ctx);
        for (auto* st : live) ps.push_back(st->path);
        out.path = z3::mk_or(ps);
    }
    for (auto& [k, v0] : base.vars) {
        auto val_in = [&](const State* st) -> z3::expr {
            auto it = st->vars.find(k);
            return it == st->vars.end() ? v0 : it->second;
        };
        z3::expr acc = val_in(live.back());
        for (size_t i = live.size() - 1; i-- > 0;) {
            auto v = val_in(live[i]);
            if (!z3::eq(v, acc)) acc = z3::ite(live[i]->path, v, acc);
        }
        out.vars.insert_or_assign(k, acc);
    }
    for (auto& [k, a0] : base.arrays) {
        auto val_in = [&](const State* st) -> Arr {
            auto it = st->arrays.find(k);
            return it == st->arrays.end() ? a0 : it->second;
        };
        Arr acc = val_in(live.back());
        for (size_t i = live.size() - 1; i-- > 0;) {
            auto v = val_in(live[i]);
            if (!z3::eq(v.a, acc.a)) acc.a = z3::ite(live[i]->path, v.a, acc.a);
        }
        out.arrays.insert_or_assign(k, acc);
    }
    for (auto& [k, u0] : base.uninit) {
        auto val_in = [&](const State* st) -> z3::expr {
            auto it = st->uninit.find(k);
            return it == st->uninit.end() ? u0 : it->second;
        };
        z3::expr acc = val_in(live.back());
        for (size_t i = live.size() - 1; i-- > 0;) {
            auto v = val_in(live[i]);
            if (!z3::eq(v, acc)) acc = z3::ite(live[i]->path, v, acc);
        }
        out.uninit.insert_or_assign(k, acc);
    }
    return out;
}

z3::expr oob(Enc& e, const TV& i0, int n) {
    TV i = e.promote(i0);
    auto bound = e.ctx.bv_val(n, static_cast<unsigned>(i.w));
    if (i.u) return z3::uge(i.v, bound);
    return (i.v < e.bv_zero(i.w)) || (i.v >= bound);
}

z3::expr index32(Enc& e, const TV& i) {
    return e.conv(e.promote(i), CType{WIDTH, false}).v;
}

// Per-element initialisation of local arrays (prism/bmc.py: _Enc.shadow_init
// ... escape_array). The shadow of array a is uninit["@a"].
void shadow_init(Enc& e, const std::string& name, bool init) {
    e.uninit.insert_or_assign("@" + name,
        z3::const_array(e.ctx.bv_sort(WIDTH), e.ctx.bool_val(!init)));
}

void mark_elem_init(Enc& e, const std::string& name, const z3::expr& idx) {
    auto it = e.uninit.find("@" + name);
    if (it != e.uninit.end()) it->second = z3::store(it->second, idx, e.ctx.bool_val(false));
}

// Reading an element never written is an indeterminate value (C11
// 6.3.2.1p2, 6.7.9p10): UNINIT-READ. Out-of-bounds indices are the OOB
// property's, not this one's.
void check_elem(Enc& e, const std::string& name, const TV& i, int n) {
    auto it = e.uninit.find("@" + name);
    if (it == e.uninit.end()) return;
    auto viol = !oob(e, i, n) && z3::select(it->second, index32(e, i));
    try {
        if (viol.simplify().is_false()) return;
    } catch (...) {}
    e.add_prop("uninit", "UNINIT-READ", viol, e.pc);
}

// A havocked loop may write any element but never unwrites one.
void havoc_shadow(Enc& e, const std::string& name, const std::string& tag) {
    auto it = e.uninit.find("@" + name);
    if (it == e.uninit.end()) return;
    e.fresh += 1;
    auto h = e.ctx.constant((tag + std::to_string(e.fresh) + "_u").c_str(),
                            e.ctx.array_sort(e.ctx.bv_sort(WIDTH), e.ctx.bool_sort()));
    auto x = e.ctx.bv_const((tag + std::to_string(e.fresh) + "_x").c_str(), WIDTH);
    it->second = z3::lambda(x, z3::select(it->second, x) && z3::select(h, x));
}

// Array passed to an unmodelled call: the callee may write any element. Its
// contents become unknown (quantified like a call result) and its elements
// count as written; the unmodelled call already rules out a proof (S6), so
// this only avoids false alarms.
void escape_array(Enc& e, const std::string& name) {
    auto it = e.arrays.find(name);
    if (it == e.arrays.end()) return;
    e.fresh += 1;
    auto arr = it->second;
    arr.a = e.ctx.constant((name + "_esc" + std::to_string(e.fresh)).c_str(), arr.a.get_sort());
    e.call_vars.push_back(arr.a);
    e.arrays.insert_or_assign(name, arr);
    if (e.uninit.count("@" + name)) shadow_init(e, name, true);
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
    int brace = 0;  // inside a `= { ... }` aggregate initialiser
    for (size_t i = 0; i < text.size(); ++i) {
        if (brace) {
            if (text[i] == '{') ++brace;
            else if (text[i] == '}') --brace;
            continue;
        }
        if (text[i] == '(') ++depth;
        else if (text[i] == ')') --depth;
        else if (text[i] == '{' && depth == 0) {
            auto before = i == 0 ? std::string::npos : text.find_last_not_of(" \t\r\n", i - 1);
            if (before != std::string::npos && text[before] == '=') {
                brace = 1;
                continue;
            }
            break;
        }
        else if (text[i] == ';' && depth == 0)
            return {text.substr(0, i + 1), text.substr(i + 1)};
    }
    throw ParseFail("no semicolon in " + text.substr(0, std::min<size_t>(80, text.size())));
}

std::pair<std::string, std::string> block_or_stmt(std::string text) {
    text = lstrip(text);
    if (text.starts_with("{")) return brace(text);
    // A nested if/loop/switch body is one statement including its own
    // else (dangling else binds to the innermost if).
    for (auto kw : {"if", "switch", "while", "for", "do"})
        if (starts_kw(text, kw)) return consume_stmt_src(text);
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
    std::vector<std::optional<TV>> labels;
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


TV apply_binop(Enc& e, TV a, const std::string& op, TV b);

struct Parser;

TV parse_expr(Enc& e, std::string src, Parser& parser);

// Declaration type prefix the encoder models (a superset of DECL_TYPE).
const char* CDECL_TYPE =
    "(?:(?:unsigned|signed)\\s+(?:long\\s+long|long|short|char|int)(?:\\s+int)?|"
    "long\\s+long(?:\\s+int)?|long(?:\\s+int)?|short(?:\\s+int)?|"
    "unsigned|signed|int|char|_Bool|bool|u?int(?:8|16|32|64)_t|"
    "size_t|ssize_t|ptrdiff_t|u?intptr_t|u?intmax_t)";

const std::vector<std::string> CDECL_KWS = {
    "int", "unsigned", "signed", "long", "short", "char", "_Bool", "bool",
    "int8_t", "uint8_t", "int16_t", "uint16_t", "uint32_t", "int32_t",
    "uint64_t", "int64_t", "size_t", "ssize_t", "ptrdiff_t", "intptr_t",
    "uintptr_t", "intmax_t", "uintmax_t",
};

bool looks_like_cdecl(std::string_view stmt) {
    auto s = lstrip(std::string(stmt));
    for (auto& kw : CDECL_KWS)
        if (starts_kw(s, kw)) return true;
    return false;
}

// Names a loop may assign (over-approximation is sound: more havoc).
std::set<std::string> assigned_names(const std::string& text) {
    std::set<std::string> out;
    static Regex asg("([A-Za-z_]\\w*)\\s*(?:\\[[^\\]]*\\]\\s*)?(?:<<|>>|[-+*/%&|^])?=(?!=)");
    static Regex pre("(?:\\+\\+|--)\\s*([A-Za-z_]\\w*)");
    static Regex post("([A-Za-z_]\\w*)\\s*(?:\\+\\+|--)");
    for (auto* rx : {&asg, &pre, &post})
        for (auto& m : rx->finditer(text)) out.insert(m.group(1));
    static Regex ident("[A-Za-z_]\\w*");
    for (auto& m : ident.finditer(text)) out.insert("@" + m.text);  // every mention (arrays)
    return out;
}

bool const_false_cond(const std::string& src) {
    auto s = strip(src);
    while (s.size() >= 2 && s.front() == '(' && s.back() == ')') s = strip(s.substr(1, s.size() - 2));
    return s == "0" || s == "false";
}

struct Parser {
    std::string body;
    std::vector<std::pair<std::string, std::string>> params;
    int unwind = 8;
    std::map<std::string, int> enums;
    std::map<std::string, std::string> macros;
    std::string err;
    bool havoc = false;
    // AI hook statements (__prism_assume/assert/havoc/step) are only parsed
    // for programs built by prism::ai::check_program, never for user code.
    bool ai_hooks = false;

    Parser(std::string b, std::vector<std::pair<std::string, std::string>> p, int u,
           std::map<std::string, int> en, std::map<std::string, std::string> mac = {},
           bool hv = false)
        : body(std::move(b)), params(std::move(p)), unwind(u), enums(std::move(en)),
          macros(std::move(mac)), havoc(hv) {}

    std::unique_ptr<Enc> run() {
        auto e = std::make_unique<Enc>(unwind);
        e->havoc = havoc;
        try {
            for (auto& [typ, name] : params) {
                if (name.empty()) continue;
                auto t = ctype_parse(typ);
                if (!t) throw ParseFail("UNENCODED: parameter type '" + strip(typ) + "' of " + name);
                e->declare(name, *t);
                e->get(name);
            }
            e->push_scope();
            stmts(*e, prep(body));
            e->pop_scope();
        } catch (const ParseFail& ex) {
            err = ex.what();
            return nullptr;
        } catch (const z3::exception& ex) {
            err = std::string("z3: ") + ex.msg();
            return nullptr;
        }
        return e;
    }

    static std::string prep(std::string body) {
        return rx_sub("#.*", " ", body);
    }

    void stmts(Enc& e, std::string text);
    void block(Enc& e, const std::string& text) {
        e.push_scope();
        stmts(e, text);
        e.pop_scope();
    }
    void decl(Enc& e, std::string stmt);
    void decl_one(Enc& e, const std::string& stmt);
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
    void loop(Enc& e, const std::string& kind, const std::string& cond_src,
              const std::string& body, const std::string& incr);
    void loop_havoc(Enc& e, const std::string& kind, const std::string& cond_src,
                    const std::string& body, const std::string& incr);
    void run_body(Enc& e, const std::string& body);
    std::string ai_hook_stmt(Enc& e, std::string text);
    TV expr(Enc& e, std::string src) { return parse_expr(e, strip(src), *this); }
    TV binop(Enc& e, const TV& a, const std::string& op, const TV& b, const std::string&) {
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
            block(e, inner);
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
            if (e.jumps.empty()) throw ParseFail("break outside loop/switch");
            e.jumps.back().breaks.push_back(e.snap());
            e.path_true = e.ctx.bool_val(false);
            text = rest;
            continue;
        }
        if (starts_kw(text, "continue")) {
            auto [_, rest] = stmt_split(text);
            Jump* j = nullptr;
            for (auto it = e.jumps.rbegin(); it != e.jumps.rend(); ++it)
                if (it->loop) { j = &*it; break; }
            if (!j) throw ParseFail("continue outside loop");
            j->conts.push_back(e.snap());
            e.path_true = e.ctx.bool_val(false);
            text = rest;
            continue;
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
        if (looks_like_cdecl(stmt)) decl(e, stmt);
        else assign_or_expr(e, stmt);
    }
}

void Parser::decl(Enc& e, std::string stmt) {
    while (!stmt.empty() && stmt.back() == ';') stmt.pop_back();
    stmt = strip(stmt);
    auto parts = split_comma(stmt);
    if (parts.size() <= 1) {
        decl_one(e, stmt);
        return;
    }
    // int x = 1, y, z = 2;  ->  one declaration per declarator.
    static Regex head(std::string("(") + CDECL_TYPE + ")\\s+[A-Za-z_*]");
    auto m = head.search_match(parts[0]);
    if (!(m && m->spans[0].first == 0)) throw ParseFail("unparsed decl: " + stmt.substr(0, 80));
    auto prefix = m->group(1);
    decl_one(e, strip(parts[0]));
    for (size_t i = 1; i < parts.size(); ++i) decl_one(e, prefix + " " + strip(parts[i]));
}

void Parser::decl_one(Enc& e, const std::string& stmt) {
    auto shadow = [&](const std::string& name) {
        if (e.declared(name))
            throw ParseFail("UNENCODED: shadowed declaration of " + name);
    };
    {
        static Regex mptr(std::string(CDECL_TYPE) + "\\s*\\*+\\s*([A-Za-z_]\\w*)(?:\\s*=\\s*(.*))?$");
        if (auto m = mptr.search_match(stmt); m && m->spans.size() > 1 && m->spans[0].first == 0 &&
            static_cast<size_t>(m->spans[0].second) == stmt.size()) {
            auto name = m->group(1);
            if (!m->groups[2] || !m->groups[2]->size())
                throw ParseFail("uninitialised pointer decl: " + stmt.substr(0, 80));
            auto src = strip(*m->groups[2]);
            if (is_ident(src) && e.is_array(src)) {
                shadow(name);
                e.declare_alias(name, src);
                return;
            }
            throw ParseFail("pointer decl must alias an array: " + stmt.substr(0, 80));
        }
    }
    {
        static Regex marr(std::string("(") + CDECL_TYPE + ")\\s+([A-Za-z_]\\w*)\\s*\\[([^\\]]+)\\](?:\\s*=\\s*(.*))?$");
        if (auto m = marr.search_match(stmt); m && m->spans.size() > 1 && m->spans[0].first == 0 &&
            static_cast<size_t>(m->spans[0].second) == stmt.size()) {
            auto typ = m->group(1);
            auto name = m->group(2);
            auto dim = strip(m->group(3));
            auto init = m->groups.size() > 4 && m->groups[4] ? *m->groups[4] : std::string{};
            if (!init.empty() && rx_search("\\{\\s*(?:\\[[^\\]]+\\]|\\.[A-Za-z_]\\w*)\\s*=", init))
                throw ParseFail("designated init unencoded");
            if (!rx_fullmatch("\\d+", dim)) throw ParseFail("VLA unencoded");
            auto ct = ctype_parse(typ);
            if (!ct) throw ParseFail("UNENCODED: element type of " + name);
            shadow(name);
            int n = std::stoi(dim);
            init = strip(init);
            if (init.size() >= 2 && init.front() == '{' && init.back() == '}') {
                // Aggregate initialiser (C11 6.7.9p21): the listed elements in
                // order, every other element zero. The items are evaluated
                // (their UB is checked) before the name is in scope.
                std::vector<std::string> items;
                for (auto& x : split_comma(init.substr(1, init.size() - 2))) items.push_back(strip(x));
                if (!items.empty() && items.back().empty()) items.pop_back();  // trailing comma, or {}
                for (auto& x : items)
                    if (x.empty() || x.front() == '{')
                        throw ParseFail("UNENCODED: nested initialiser of " + name);
                if (static_cast<int>(items.size()) > n)
                    throw ParseFail("UNENCODED: excess initialisers of " + name);
                std::vector<TV> vals;
                for (auto& x : items) vals.push_back(expr(e, x));
                auto arr = z3::const_array(e.ctx.bv_sort(WIDTH), e.bv_zero(ct->w));
                for (size_t k = 0; k < vals.size(); ++k)
                    arr = z3::store(arr, e.ctx.bv_val(static_cast<int>(k), WIDTH), e.conv(vals[k], *ct).v);
                e.declare_array(name, Arr{arr, n, ct->w, ct->u});
                shadow_init(e, name, true);
                return;
            }
            if (!init.empty() && !(ct->w == 8 && rx_fullmatch(R"BMC("(?:[^"\\]|\\.)*")BMC", init)))
                throw ParseFail("UNENCODED: initialiser of " + name);
            // No initialiser: contents arbitrary, every element unwritten. A
            // string literal writes every element (6.7.9p14, p21); its
            // characters are not modelled (arbitrary contents: a sound
            // over-approximation, never a proof about the values).
            auto arr = e.ctx.constant((name + "_arr" + std::to_string(++e.fresh)).c_str(),
                e.ctx.array_sort(e.ctx.bv_sort(WIDTH), e.ctx.bv_sort(static_cast<unsigned>(ct->w))));
            e.declare_array(name, Arr{arr, n, ct->w, ct->u});
            shadow_init(e, name, !init.empty());
            return;
        }
    }
    static Regex md(std::string("(") + CDECL_TYPE + ")\\s+([A-Za-z_]\\w*)(?:\\s*=\\s*(.*))?$");
    auto m = md.search_match(stmt);
    if (!(m && m->spans[0].first == 0 && static_cast<size_t>(m->spans[0].second) == stmt.size())) {
        if (rx_search(":\\s*[A-Za-z_]", stmt)) throw ParseFail("range-for unencoded");
        throw ParseFail("unparsed decl: " + stmt.substr(0, 80));
    }
    auto typ = m->group(1);
    auto name = m->group(2);
    bool has_init = m->groups.size() > 3 && m->groups[3].has_value();
    auto ct = ctype_parse(typ);
    if (!ct) throw ParseFail("UNENCODED: type of " + name);
    shadow(name);
    if (has_init) {
        // The initialiser is evaluated before the name is in scope.
        auto val = expr(e, *m->groups[3]);
        e.declare(name, *ct);
        e.set(name, val);
        e.mark_init(name);
    } else {
        e.declare(name, *ct);
        auto v = e.bv(name + "_uninit", ct->w);
        e.set(name, TV{v, ct->w, ct->u});
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
        static_cast<size_t>(m->spans[0].second) == stmt.size() &&
        !rx_match("=", m->group(2))) {
        astore(e, m->group(1), "0", m->group(2));
        return;
    }
    static Regex aidx("([A-Za-z_]\\w*)\\s*\\[(.+)\\]\\s*=\\s*(.+)$");
    if (auto m = aidx.search_match(stmt); m && m->spans[0].first == 0 &&
        static_cast<size_t>(m->spans[0].second) == stmt.size() &&
        !rx_match("=", m->group(3))) {
        astore(e, m->group(1), m->group(2), m->group(3));
        return;
    }
    static Regex asg("([A-Za-z_]\\w*)\\s*((?:<<|>>|[-+*/%|&^])?=)\\s*(.+)$");
    if (auto m = asg.search_match(stmt); m && m->spans[0].first == 0 &&
        static_cast<size_t>(m->spans[0].second) == stmt.size() &&
        !rx_match("=", m->group(3))) {
        auto name = m->group(1);
        auto op = m->group(2);
        auto rhs = m->group(3);
        if (!e.bits.count(name)) {
            if (e.declared(name)) throw ParseFail("UNENCODED: assignment to array/pointer " + name);
            throw ParseFail("UNENCODED: assignment to undeclared " + name);
        }
        auto val = expr(e, rhs);
        if (op == "=") e.set(name, val);
        else {
            e.check_read(name);
            auto cur = e.get(name);
            e.set(name, binop(e, cur, op.substr(0, op.size() - 1), val, stmt));
        }
        e.mark_init(name);
        return;
    }
    expr(e, stmt);
}

void Parser::astore(Enc& e, const std::string& name0, const std::string& idx, const std::string& rhs) {
    auto name = e.canonical(name0);
    if (!e.arrays.count(name)) throw ParseFail("UNENCODED: store to unknown array " + name0);
    auto i = expr(e, idx);
    auto v = expr(e, rhs);
    auto arr = e.arrays.at(name);
    e.add_prop("oob-write", "MEM-OOB-WRITE", oob(e, i, arr.n), e.pc);
    arr.a = z3::store(arr.a, index32(e, i), e.conv(v, CType{arr.w, arr.u}).v);
    e.arrays.insert_or_assign(name, arr);
    mark_elem_init(e, name, index32(e, i));
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

void Parser::return_stmt(Enc& e, std::string stmt) {
    auto rest = strip(stmt);
    if (rest.starts_with("return")) {
        auto exprs = strip(rest.substr(6));
        while (!exprs.empty() && exprs.back() == ';') exprs.pop_back();
        exprs = strip(exprs);
        if (!exprs.empty()) {
            auto val = expr(e, exprs);
            e.vars.insert_or_assign("__ret", val.v);
        }
    }
    e.path_true = e.ctx.bool_val(false);
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
        if (e.is_array(name)) {
            auto an = e.canonical(name);
            auto arr = e.arrays.at(an);
            arr.a = e.ctx.constant(fresh_name.c_str(), arr.a.get_sort());
            e.arrays.insert_or_assign(an, arr);
            havoc_shadow(e, an, fresh_name + "_u");
        } else {
            if (!e.bits.count(name)) throw ParseFail("UNENCODED: ai havoc of undeclared " + name);
            auto t = e.type_of(name);
            e.vars.insert_or_assign(name, e.bv(fresh_name, t.w));
            auto it = e.uninit.find(name);
            if (it != e.uninit.end()) {
                bool surely_init = false;
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
        auto base = e.snap();
        e.path_true = base.path && cond;
        // A break/continue has no enclosing loop here: ParseFail, never dropped.
        block(e, body);
        e.load(base);
        e.path_true = base.path && !cond;
        return r2;
    }
    throw ParseFail("unknown ai hook");
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
    if (starts_kw(rest2, "else")) {
        auto [es, r3] = block_or_stmt(rest2.substr(4));
        else_src = es;
        rest = r3;
    }
    auto cond = as_bool(expr(e, cond_src));
    auto base = e.snap();
    e.path_true = base.path && cond;
    block(e, then_src);
    auto then_st = e.snap();
    e.load(base);
    e.path_true = base.path && !cond;
    if (else_src) block(e, *else_src);
    auto else_st = e.snap();
    e.load(merge_states(e, {then_st, else_st}, base));
    return rest;
}

// One execution of a loop body; `continue` states rejoin at its end.
void Parser::run_body(Enc& e, const std::string& body) {
    auto base = e.snap();
    e.jumps.back().conts.clear();
    block(e, body);
    auto& conts = e.jumps.back().conts;
    if (!conts.empty()) {
        std::vector<State> states = conts;
        states.push_back(e.snap());
        conts.clear();
        e.load(merge_states(e, states, base));
    }
}

// Bounded unrolling. Every iteration's exit (condition false) and every
// break is kept and merged after the loop; a path still looping after
// `unwind` iterations is cut and makes the result BOUNDED (unwind_ok=false).
void Parser::loop(Enc& e, const std::string& kind, const std::string& cond_src,
                  const std::string& body, const std::string& incr) {
    if (e.havoc) {
        loop_havoc(e, kind, cond_src, body, incr);
        return;
    }
    auto base = e.snap();
    e.jumps.push_back(Jump{true, {}, {}});
    std::vector<State> exits;
    int iters = e.unwind;
    if (kind == "do") {
        run_body(e, body);
        iters = std::max(e.unwind - 1, 0);
    }
    bool stopped = false;
    for (int i = 0; i < iters; ++i) {
        auto cond = as_bool(expr(e, cond_src));
        auto ex = e.snap();
        ex.path = e.path_true && !cond;
        exits.push_back(ex);
        e.path_true = e.path_true && cond;
        if (check_sat(e, e.ctx.bool_val(true)) == z3::unsat) {
            stopped = true;
            break;
        }
        run_body(e, body);
        if (!incr.empty()) assign_or_expr(e, incr);
    }
    if (!stopped) {
        auto cond = as_bool(expr(e, cond_src));
        if (check_sat(e, cond) != z3::unsat) e.unwind_ok = false;
        auto ex = e.snap();
        ex.path = e.path_true && !cond;
        exits.push_back(ex);
    }
    auto j = std::move(e.jumps.back());
    e.jumps.pop_back();
    for (auto& b : j.breaks) exits.push_back(b);
    e.load(merge_states(e, exits, base));
}

// Unbounded step: every variable (and array) the loop may assign is made
// arbitrary, the condition and one body execution are checked from that
// state, and the loop is left with the condition false (or by break).
// Every concrete iteration starts in a havocked state, so no UB inside the
// loop is missed and every post-loop state is covered.
void Parser::loop_havoc(Enc& e, const std::string& kind, const std::string& cond_src,
                        const std::string& body, const std::string& incr) {
    if (const_false_cond(cond_src)) {
        if (kind != "do") return;
        auto base = e.snap();
        e.jumps.push_back(Jump{true, {}, {}});
        run_body(e, body);
        auto j = std::move(e.jumps.back());
        e.jumps.pop_back();
        std::vector<State> exits = j.breaks;
        exits.push_back(e.snap());
        e.load(merge_states(e, exits, base));
        return;
    }
    auto names = assigned_names(cond_src + ";\n" + body + ";\n" + incr);
    for (auto& n : names) {
        if (n.starts_with("@")) {
            auto an = e.canonical(n.substr(1));
            auto it = e.arrays.find(an);
            if (it != e.arrays.end()) {
                auto arr = it->second;
                arr.a = e.ctx.constant((an + "_hv" + std::to_string(++e.fresh)).c_str(), arr.a.get_sort());
                e.arrays.insert_or_assign(an, arr);
                havoc_shadow(e, an, an + "_hvu");
            }
            continue;
        }
        if (!e.bits.count(n)) continue;
        auto t = e.type_of(n);
        e.vars.insert_or_assign(n, e.bv(n + "_hv" + std::to_string(e.fresh + 1), t.w));
        auto u = e.uninit.find(n);
        if (u != e.uninit.end())
            u->second = u->second && e.ctx.bool_const((n + "_hvu" + std::to_string(++e.fresh)).c_str());
    }
    auto base = e.snap();
    e.jumps.push_back(Jump{true, {}, {}});
    std::vector<State> exits;
    if (kind == "do") {
        run_body(e, body);
        auto cond = as_bool(expr(e, cond_src));
        auto ex = e.snap();
        ex.path = e.path_true && !cond;
        exits.push_back(ex);
    } else {
        auto cond = as_bool(expr(e, cond_src));
        auto ex = e.snap();
        ex.path = e.path_true && !cond;
        exits.push_back(ex);
        e.path_true = e.path_true && cond;
        run_body(e, body);
        if (!incr.empty()) assign_or_expr(e, incr);
    }
    auto j = std::move(e.jumps.back());
    e.jumps.pop_back();
    for (auto& b : j.breaks) exits.push_back(b);
    e.load(merge_states(e, exits, base));
}

std::string Parser::while_stmt(Enc& e, std::string text) {
    auto rest = lstrip(text.substr(5));
    auto [cond_src, r1] = paren(rest);
    auto [body, r2] = block_or_stmt(r1);
    loop(e, "while", cond_src, body, "");
    return r2;
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
    loop(e, "do", cond_src, body, "");
    return rest;
}

std::string Parser::for_stmt(Enc& e, std::string text) {
    auto rest = lstrip(text.substr(3));
    auto [head, r1] = paren(rest);
    auto parts = split_semi(head);
    while (parts.size() < 3) parts.push_back("");
    if (parts.size() > 3) throw ParseFail("unparsed for header");
    auto init = strip(parts[0]);
    auto cond_src = strip(parts[1]);
    auto incr = strip(parts[2]);
    auto [body, r2] = block_or_stmt(r1);
    e.push_scope();
    if (!init.empty()) {
        auto init_stmt = init.ends_with(";") ? init : init + ";";
        if (auto miss = unencoded_layout_stmt(init_stmt)) throw ParseFail(*miss);
        if (looks_like_cdecl(init_stmt)) decl(e, init_stmt);
        else assign_or_expr(e, init_stmt);
    }
    auto incr_stmt = incr.empty() ? std::string{} : (incr.ends_with(";") ? incr : incr + ";");
    loop(e, "for", cond_src.empty() ? "1" : cond_src, body, incr_stmt);
    e.pop_scope();
    return r2;
}

std::string Parser::switch_stmt(Enc& e, std::string text) {
    auto rest = lstrip(text.substr(6));
    auto [cond_src, r1] = paren(rest);
    auto [body, r2] = block_or_stmt(r1);
    rest = r2;
    auto scrut = e.promote(expr(e, cond_src));
    auto arms = parse_switch_arms(e, body);
    if (arms.empty()) return rest;
    CType st{scrut.w, scrut.u};

    z3::expr_vector case_eqs(e.ctx);
    bool has_default = false;
    for (auto& arm : arms) {
        for (auto& lab : arm.labels) {
            if (!lab) has_default = true;
            else case_eqs.push_back(scrut.v == e.conv(*lab, st).v);
        }
    }
    z3::expr any_case = case_eqs.empty() ? e.ctx.bool_val(false) : z3::mk_or(case_eqs);

    auto base = e.snap();
    e.jumps.push_back(Jump{false, {}, {}});
    std::vector<State> taken;
    for (size_t i = 0; i < arms.size(); ++i) {
        auto& arm = arms[i];
        if (arm.labels.empty()) continue;
        z3::expr_vector parts(e.ctx);
        for (auto& lab : arm.labels) {
            if (!lab) parts.push_back(!any_case);
            else parts.push_back(scrut.v == e.conv(*lab, st).v);
        }
        z3::expr cond = parts.size() == 1 ? parts[0] : z3::mk_or(parts);
        e.load(base);
        e.path_true = base.path && cond;
        block(e, arm_code(arms, i));
        taken.push_back(e.snap());
    }
    if (!has_default) {
        auto skip = base;
        skip.path = base.path && !any_case;
        taken.push_back(skip);
    }
    auto j = std::move(e.jumps.back());
    e.jumps.pop_back();
    for (auto& b : j.breaks) taken.push_back(b);
    e.load(merge_states(e, taken, base));
    return rest;
}

std::vector<SwitchArm> Parser::parse_switch_arms(Enc& e, std::string body) {
    std::vector<SwitchArm> arms;
    std::vector<std::optional<TV>> labels;
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

// Signed result of `wide` (computed exactly in more bits) does not fit in w bits.
z3::expr signed_ovf(const z3::expr& wide, int w) {
    auto narrow = wide.extract(static_cast<unsigned>(w - 1), 0);
    int ww = static_cast<int>(wide.get_sort().bv_size());
    return z3::sext(narrow, static_cast<unsigned>(ww - w)) != wide;
}

TV apply_binop(Enc& e, TV a, const std::string& op, TV b) {
    if (op == "<<" || op == ">>") {
        // C11 6.5.7: promotions on each operand separately; the result has
        // the type of the promoted left operand.
        a = e.promote(a);
        b = e.promote(b);
        int w = a.w;
        z3::expr bad_count = b.u ? z3::uge(b.v, e.ctx.bv_val(w, static_cast<unsigned>(b.w)))
                                 : ((b.v < e.bv_zero(b.w)) ||
                                    (b.v >= e.ctx.bv_val(w, static_cast<unsigned>(b.w))));
        e.add_prop("shift", "INT-SHIFT-UB", bad_count, e.pc);
        z3::expr cnt = e.conv(b, CType{w, true}).v;
        if (op == "<<") {
            if (!a.u) {
                // Negative left operand, or a * 2^b not representable (6.5.7p4).
                e.add_prop("shift-neg", "INT-SHIFT-UB", a.v < e.bv_zero(w), e.pc);
                auto top = z3::lshr(a.v, e.ctx.bv_val(w - 1, static_cast<unsigned>(w)) - cnt);
                e.add_prop("shift31", "INT-SHIFT-UB",
                           !bad_count && (a.v >= e.bv_zero(w)) && (top != e.bv_zero(w)), e.pc);
            }
            return TV{z3::shl(a.v, cnt), w, a.u};
        }
        return TV{a.u ? z3::lshr(a.v, cnt) : z3::ashr(a.v, cnt), w, a.u};
    }
    auto ct = e.common(a, b);
    a = e.conv(a, ct);
    b = e.conv(b, ct);
    int w = ct.w;
    bool u = ct.u;
    if (op == "+" || op == "-" || op == "*") {
        z3::expr r = op == "+" ? a.v + b.v : op == "-" ? a.v - b.v : a.v * b.v;
        if (!u) {
            // Both directions (S1). + and - are computed exactly one bit
            // wider; * uses Z3's overflow/underflow predicates, which are
            // exact and far cheaper than a 2w-bit product.
            z3::expr viol = e.ctx.bool_val(false);
            if (op == "*") {
                viol = !z3::bvmul_no_overflow(a.v, b.v, true) || !z3::bvmul_no_underflow(a.v, b.v);
            } else {
                auto wa = z3::sext(a.v, 1), wb = z3::sext(b.v, 1);
                viol = signed_ovf(op == "+" ? wa + wb : wa - wb, w);
            }
            e.add_prop(std::string("ovf") + op, "INT-SIGNED-OVF", viol, e.pc);
        }
        return TV{r, w, u};
    }
    if (op == "/" || op == "%") {
        bool div = op == "/";
        e.add_prop(div ? "div0" : "mod0", "INT-DIV-ZERO", b.v == e.bv_zero(w), e.pc);
        auto z = e.bv_zero(w);
        if (u) {
            auto r = div ? z3::udiv(a.v, b.v) : z3::urem(a.v, b.v);
            return TV{z3::ite(b.v == z, z, r), w, true};
        }
        // INT_MIN / -1 and INT_MIN % -1 are both undefined (6.5.5p6).
        e.add_prop(div ? "divovf" : "modovf", "INT-SIGNED-OVF",
                   (a.v == e.bv_min(w)) && (b.v == e.ctx.bv_val(-1, static_cast<unsigned>(w))), e.pc);
        auto r = div ? a.v / b.v : z3::srem(a.v, b.v);
        return TV{z3::ite(b.v == z, z, r), w, false};
    }
    if (op == "&") return TV{a.v & b.v, w, u};
    if (op == "|") return TV{a.v | b.v, w, u};
    if (op == "^") return TV{a.v ^ b.v, w, u};
    if (op == "==") return bool_tv(e, a.v == b.v);
    if (op == "!=") return bool_tv(e, a.v != b.v);
    if (op == "<") return bool_tv(e, u ? z3::ult(a.v, b.v) : (a.v < b.v));
    if (op == ">") return bool_tv(e, u ? z3::ugt(a.v, b.v) : (a.v > b.v));
    if (op == "<=") return bool_tv(e, u ? z3::ule(a.v, b.v) : (a.v <= b.v));
    if (op == ">=") return bool_tv(e, u ? z3::uge(a.v, b.v) : (a.v >= b.v));
    throw ParseFail("op " + op);
}

// Evaluate fn() only on the paths where cond holds (&&, ||, ?:), then merge
// its side effects back: state = cond ? after : before.
template <class F>
TV guarded(Enc& e, const z3::expr& cond, F fn) {
    auto base = e.snap();
    e.path_true = base.path && cond;
    TV r = fn();
    auto taken = e.snap();
    auto skipped = base;
    skipped.path = base.path && !cond;
    e.load(merge_states(e, {taken, skipped}, base));
    return r;
}

int char_lit_value(const std::string& tok) {
    auto inner = tok.substr(1, tok.size() - 2);
    if (inner.empty()) throw ParseFail("empty character literal");
    int v = 0;
    if (inner[0] == '\\' && inner.size() >= 2) {
        char esc = inner[1];
        auto all_of = [](const std::string& d, const char* set) {
            return !d.empty() && d.find_first_not_of(set) == std::string::npos;
        };
        if (esc == 'x') {
            auto d = inner.substr(2);
            if (!all_of(d, "0123456789abcdefABCDEF") || d.size() > 8)
                throw ParseFail("UNENCODED: character literal " + tok);
            v = static_cast<int>(std::stoll(d, nullptr, 16));
        } else if (esc >= '0' && esc <= '7') {
            auto d = inner.substr(1);
            if (!all_of(d, "01234567") || d.size() > 8)
                throw ParseFail("UNENCODED: character literal " + tok);
            v = static_cast<int>(std::stoll(d, nullptr, 8));
        } else {
            if (inner.size() != 2) throw ParseFail("UNENCODED: character literal " + tok);
            switch (esc) {
            case 'n': v = 10; break;
            case 't': v = 9; break;
            case 'r': v = 13; break;
            case 'a': v = 7; break;
            case 'b': v = 8; break;
            case 'f': v = 12; break;
            case 'v': v = 11; break;
            case '\\': v = 92; break;
            case '\'': v = 39; break;
            case '"': v = 34; break;
            case '?': v = 63; break;
            default: throw ParseFail("UNENCODED: character literal " + tok);
            }
        }
    } else {
        if (inner.size() != 1 || static_cast<unsigned char>(inner[0]) > 127)
            throw ParseFail("UNENCODED: character literal " + tok);
        v = static_cast<unsigned char>(inner[0]);
    }
    if (v > 255) throw ParseFail("UNENCODED: character literal " + tok);
    // A character constant has type int and the value of the (signed, on
    // x86-64) char it names: '\xff' == -1.
    return v > 127 ? v - 256 : v;
}

// Integer constant with its C type (C11 6.4.4.1p5, LP64).
TV int_literal(Enc& e, const std::string& tok) {
    std::string digits = tok;
    std::string suf;
    while (!digits.empty() && std::strchr("uUlL", digits.back())) {
        suf.insert(suf.begin(), digits.back());
        digits.pop_back();
    }
    bool has_u = suf.find_first_of("uU") != std::string::npos;
    int nl = 0;
    for (char c : suf)
        if (c == 'l' || c == 'L') ++nl;
    if (suf.size() - (has_u ? 1 : 0) != static_cast<size_t>(nl) || nl > 2 ||
        std::count_if(suf.begin(), suf.end(), [](char c) { return c == 'u' || c == 'U'; }) > 1)
        throw ParseFail("UNENCODED: integer literal " + tok);
    uint64_t val = 0;
    bool decimal = true;
    try {
        size_t used = 0;
        if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B')) {
            decimal = false;
            val = std::stoull(digits.substr(2), &used, 2);
            used += 2;
        } else {
            decimal = !(digits.size() > 1 && digits[0] == '0');
            val = std::stoull(digits, &used, 0);
        }
        if (used != digits.size()) throw ParseFail("UNENCODED: integer literal " + tok);
    } catch (const ParseFail&) {
        throw;
    } catch (...) {
        throw ParseFail("UNENCODED: integer literal " + tok);
    }
    std::vector<CType> cands;
    if (has_u) {
        if (nl == 0) cands.push_back({32, true});
        cands.push_back({64, true});
    } else if (decimal) {
        if (nl == 0) cands.push_back({32, false});
        cands.push_back({64, false});
    } else {
        if (nl == 0) {
            cands.push_back({32, false});
            cands.push_back({32, true});
        }
        cands.push_back({64, false});
        cands.push_back({64, true});
    }
    for (auto& c : cands) {
        uint64_t maxv = c.w == 64 ? (c.u ? ~uint64_t{0} : (uint64_t{1} << 63) - 1)
                                  : (c.u ? 0xFFFFFFFFull : 0x7FFFFFFFull);
        if (val <= maxv) return TV{e.ctx.bv_val(val, static_cast<unsigned>(c.w)), c.w, c.u};
    }
    throw ParseFail("UNENCODED: integer literal " + tok + " has no type");
}

const std::unordered_set<std::string> CAST_TYPE_WORDS = {
    "char", "short", "int", "long", "unsigned", "signed",
    "const", "volatile", "void", "_Bool", "bool",
    "int8_t", "uint8_t", "int16_t", "uint16_t",
    "uint32_t", "int32_t", "uint64_t", "int64_t", "size_t", "ssize_t",
    "ptrdiff_t", "intptr_t", "uintptr_t", "intmax_t", "uintmax_t",
};

// Object-like macros the encoder knows without a preprocessor: <limits.h>,
// <stdint.h>, <stdlib.h>, <stdbool.h> (LP64 glibc values). A #define in the
// file itself takes precedence (Parser::macros).
const std::map<std::string, std::string>& builtin_macros() {
    static const std::map<std::string, std::string> M = {
        {"CHAR_BIT", "8"},
        {"SCHAR_MIN", "(-128)"}, {"SCHAR_MAX", "127"}, {"UCHAR_MAX", "255"},
        {"CHAR_MIN", "(-128)"}, {"CHAR_MAX", "127"},
        {"SHRT_MIN", "(-32768)"}, {"SHRT_MAX", "32767"}, {"USHRT_MAX", "65535"},
        {"INT_MIN", "(-2147483647 - 1)"}, {"INT_MAX", "2147483647"},
        {"UINT_MAX", "4294967295U"},
        {"LONG_MIN", "(-9223372036854775807L - 1L)"}, {"LONG_MAX", "9223372036854775807L"},
        {"ULONG_MAX", "18446744073709551615UL"},
        {"LLONG_MIN", "(-9223372036854775807LL - 1LL)"}, {"LLONG_MAX", "9223372036854775807LL"},
        {"ULLONG_MAX", "18446744073709551615ULL"},
        {"INT8_MIN", "(-128)"}, {"INT8_MAX", "127"}, {"UINT8_MAX", "255"},
        {"INT16_MIN", "(-32768)"}, {"INT16_MAX", "32767"}, {"UINT16_MAX", "65535"},
        {"INT32_MIN", "(-2147483647 - 1)"}, {"INT32_MAX", "2147483647"},
        {"UINT32_MAX", "4294967295U"},
        {"INT64_MIN", "(-9223372036854775807L - 1L)"}, {"INT64_MAX", "9223372036854775807L"},
        {"UINT64_MAX", "18446744073709551615UL"},
        {"SIZE_MAX", "18446744073709551615UL"},
        {"RAND_MAX", "2147483647"},
        {"EXIT_SUCCESS", "0"}, {"EXIT_FAILURE", "1"},
        {"true", "1"}, {"false", "0"}, {"NULL", "0"},
    };
    return M;
}

std::vector<std::string> ctok(const std::string& src) {
    static Regex rx(
        "(0[xX][0-9a-fA-F]+[uUlL]*)|(0[bB][01]+[uUlL]*)|(\\d+[uUlL]*)|"
        "('(?:\\\\.|[^\\\\'])+')|(\"(?:\\\\.|[^\\\\\"])*\")|"
        "([A-Za-z_]\\w*)|(::|->|&&|\\|\\||==|!=|<=|>=|<<|>>|\\+\\+|--)|"
        "([+\\-*/%<>=!&|^~()[\\],?:.;{}])|(\\S)");
    std::vector<std::string> out;
    for (auto& m : rx.finditer(src)) out.push_back(m.text);
    return out;
}

// Expand object-like macros (file #defines first, then the builtin table)
// that do not name a visible variable.
std::vector<std::string> expand_macros(const std::vector<std::string>& toks, Enc& e,
                                       const std::map<std::string, std::string>& file_macros,
                                       int depth = 0) {
    if (depth > 8) throw ParseFail("UNENCODED: macro expansion too deep");
    std::vector<std::string> out;
    for (size_t i = 0; i < toks.size(); ++i) {
        auto& t = toks[i];
        const std::string* rep = nullptr;
        if (is_ident(t) && !e.declared(t)) {
            auto it = file_macros.find(t);
            if (it != file_macros.end()) rep = &it->second;
            else {
                auto jt = builtin_macros().find(t);
                if (jt != builtin_macros().end()) rep = &jt->second;
            }
        }
        if (!rep) {
            out.push_back(t);
            continue;
        }
        auto sub = expand_macros(ctok(*rep), e, file_macros, depth + 1);
        out.push_back("(");
        out.insert(out.end(), sub.begin(), sub.end());
        out.push_back(")");
    }
    return out;
}

// Calls whose semantics are modelled. Everything else: arguments are
// checked, the result is unconstrained, and the verdict can never be a proof.
TV model_call(Enc& e, const std::string& t, std::vector<TV>& args) {
    auto nargs = [&](size_t n) {
        if (args.size() != n) throw ParseFail("UNENCODED: call to " + t + " with " + std::to_string(args.size()) + " arguments");
    };
    if (t == "abs" || t == "labs" || t == "llabs") {
        nargs(1);
        CType ct = t == "abs" ? CType{32, false} : CType{64, false};
        auto a = e.conv(e.promote(args[0]), ct);
        // abs(INT_MIN) is undefined (C11 7.22.6.1p2).
        e.add_prop("abs", "INT-SIGNED-OVF", a.v == e.bv_min(ct.w), e.pc);
        return TV{z3::ite(a.v < e.bv_zero(ct.w), e.bv_zero(ct.w) - a.v, a.v), ct.w, false};
    }
    if (t == "__builtin_expect") {
        nargs(2);
        return e.conv(args[0], CType{64, false});
    }
    static const std::map<std::string, CType> NONDET = {
        {"__VERIFIER_nondet_int", {32, false}}, {"__VERIFIER_nondet_uint", {32, true}},
        {"__VERIFIER_nondet_unsigned", {32, true}},
        {"__VERIFIER_nondet_long", {64, false}}, {"__VERIFIER_nondet_ulong", {64, true}},
        {"__VERIFIER_nondet_longlong", {64, false}}, {"__VERIFIER_nondet_ulonglong", {64, true}},
        {"__VERIFIER_nondet_short", {16, false}}, {"__VERIFIER_nondet_ushort", {16, true}},
        {"__VERIFIER_nondet_char", {8, false}}, {"__VERIFIER_nondet_uchar", {8, true}},
        {"__VERIFIER_nondet_bool", {1, true}}, {"__VERIFIER_nondet__Bool", {1, true}},
        {"__VERIFIER_nondet_size_t", {64, true}},
    };
    if (auto it = NONDET.find(t); it != NONDET.end()) {
        nargs(0);
        auto ct = it->second;
        return TV{e.bv("nondet_" + std::to_string(e.fresh + 1), ct.w), ct.w, ct.u};
    }
    if (t == "rand") {
        nargs(0);
        auto v = e.bv("rand_" + std::to_string(e.fresh + 1), 32);
        e.assume(v >= e.bv_zero(32));
        return TV{v, 32, false};
    }
    if (t == "__VERIFIER_assume" || t == "assume_abort_if_not") {
        // SV-COMP harness convention: execution continues only if cond != 0.
        nargs(1);
        e.assume(as_bool(args[0]));
        return e.int_val(0);
    }
    if (t == "abort" || t == "exit" || t == "_Exit" || t == "quick_exit" ||
        t == "reach_error" || t == "__assert_fail" || t == "__VERIFIER_error") {
        // Does not return: nothing after it runs on this path.
        e.path_true = e.ctx.bool_val(false);
        return e.int_val(0);
    }
    // Juliet support-library output helpers (io.c): print one scalar or a
    // string literal; no undefined behaviour for any argument value.
    static const std::unordered_set<std::string> PRINT = {
        "printLine", "printWLine", "printIntLine", "printShortLine", "printLongLine",
        "printLongLongLine", "printSizeTLine", "printHexCharLine", "printUnsignedLine",
        "printHexUnsignedCharLine",
    };
    if (PRINT.count(t)) {
        nargs(1);
        return e.int_val(0);
    }
    if (std::find(e.unmodelled.begin(), e.unmodelled.end(), t) == e.unmodelled.end())
        e.unmodelled.push_back(t);
    auto r = e.bv("call_" + t + "_" + std::to_string(e.fresh + 1), 32);
    e.call_vars.push_back(r);
    return TV{r, 32, false};
}

int sizeof_type(const std::vector<std::string>& inner, Enc& e) {
    for (auto& t : inner)
        if (t == "*") return 8;
    std::string joined;
    for (size_t i = 0; i < inner.size(); ++i) {
        if (i) joined += " ";
        joined += inner[i];
    }
    if (inner.size() == 1 && is_ident(inner[0]) && e.declared(inner[0])) {
        auto name = inner[0];
        if (e.alias.count(name)) return 8;
        if (e.arrays.count(name)) {
            auto& a = e.arrays.at(name);
            return a.n * std::max(1, a.w / 8);
        }
        return std::max(1, e.type_of(name).w / 8);
    }
    if (auto ct = ctype_parse(joined)) return std::max(1, ct->w / 8);
    if (inner.size() == 4 && e.is_array(inner[0]) && inner[1] == "[" && inner[3] == "]") {
        auto& a = e.arrays.at(e.canonical(inner[0]));
        return std::max(1, a.w / 8);
    }
    throw ParseFail("UNENCODED: sizeof " + joined);
}

TV parse_expr(Enc& e, std::string src, Parser& parser) {
    src = strip(src);
    auto tokens = expand_macros(ctok(src), e, parser.macros);
    size_t pos = 0;
    auto peek = [&](size_t k = 0) -> std::string {
        return pos + k < tokens.size() ? tokens[pos + k] : std::string{};
    };
    auto eat = [&](std::string t = {}) -> std::string {
        if (pos >= tokens.size()) throw ParseFail("unexpected end of expression");
        auto got = tokens[pos];
        if (!t.empty() && got != t) throw ParseFail("expected " + t + " got " + got);
        ++pos;
        return got;
    };
    std::function<TV(int)> parse;
    std::function<TV()> nud;
    static const std::map<std::string, int> PREC = {
        {"||", 10}, {"&&", 20},
        {"|", 30}, {"^", 40}, {"&", 50},
        {"==", 60}, {"!=", 60},
        {"<", 70}, {">", 70}, {"<=", 70}, {">=", 70},
        {"<<", 80}, {">>", 80},
        {"+", 90}, {"-", 90},
        {"*", 100}, {"/", 100}, {"%", 100},
    };
    auto incdec = [&](const std::string& name, const std::string& op, bool prefix) -> TV {
        if (!e.bits.count(name)) {
            if (e.declared(name)) throw ParseFail("UNENCODED: " + op + " on array/pointer " + name);
            throw ParseFail("UNENCODED: identifier " + name);
        }
        e.check_read(name);
        auto cur = e.get(name);
        auto nw = apply_binop(e, cur, op == "++" ? "+" : "-", e.int_val(1));
        e.set(name, nw);
        e.mark_init(name);
        return prefix ? e.get(name) : cur;
    };
    nud = [&]() -> TV {
        auto t = eat();
        if (t.size() >= 3 && t.front() == '\'' && t.back() == '\'')
            return e.int_val(char_lit_value(t));
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
            return e.int_val(1);  // a string literal is a non-null address
        if (t == "alignof" || t == "_Alignof") throw ParseFail("alignof unencoded");
        if (t == "sizeof") {
            std::vector<std::string> inner;
            if (peek() == "(") {
                eat("(");
                int depth = 1;
                while (depth) {
                    auto ntok = eat();
                    if (ntok == "(") { ++depth; inner.push_back(ntok); }
                    else if (ntok == ")") {
                        --depth;
                        if (depth) inner.push_back(ntok);
                    } else inner.push_back(ntok);
                }
            } else {
                inner.push_back(eat());
            }
            return TV{e.ctx.bv_val(sizeof_type(inner, e), 64), 64, true};
        }
        if (t == "(") {
            if (peek() == "{") throw ParseFail("statement-expr unencoded");
            if (CAST_TYPE_WORDS.count(peek())) {
                std::vector<std::string> words;
                while (!peek().empty() && peek() != ")") {
                    if (!CAST_TYPE_WORDS.count(peek()) && peek() != "*") break;
                    words.push_back(eat());
                }
                eat(")");
                auto v = parse(110);
                std::string joined;
                for (size_t i = 0; i < words.size(); ++i) {
                    if (i) joined += " ";
                    joined += words[i];
                }
                bool is_void = true;
                for (auto& wd : words)
                    if (wd != "void" && wd != "const" && wd != "volatile") is_void = false;
                if (is_void) return e.int_val(0);  // (void)x: evaluated, value unused
                auto ct = ctype_parse(joined);
                if (!ct) throw ParseFail("UNENCODED: cast to " + joined);
                return e.conv(v, *ct);
            }
            auto v = parse(0);
            eat(")");
            return v;
        }
        if (t == "++" || t == "--") {
            auto name = eat();
            if (!is_ident(name)) throw ParseFail("prefix " + t + " needs an identifier");
            return incdec(name, t, true);
        }
        if (t == "*") {
            auto name = peek();
            if (!is_ident(name) || !e.is_array(name))
                throw ParseFail("deref of '" + name + "'");
            eat();
            auto arr = e.arrays.at(e.canonical(name));
            auto idx = e.int_val(0);
            e.add_prop("oob-read", "MEM-OOB-READ", oob(e, idx, arr.n), e.pc);
            check_elem(e, e.canonical(name), idx, arr.n);
            return TV{z3::select(arr.a, index32(e, idx)), arr.w, arr.u};
        }
        if (t == "-") {
            auto v = e.promote(parse(110));
            if (!v.u) e.add_prop("neg", "INT-SIGNED-OVF", v.v == e.bv_min(v.w), e.pc);
            return TV{e.bv_zero(v.w) - v.v, v.w, v.u};
        }
        if (t == "+") return e.promote(parse(110));
        if (t == "!") return bool_tv(e, !as_bool(parse(110)));
        if (t == "~") {
            auto v = e.promote(parse(110));
            return TV{~v.v, v.w, v.u};
        }
        if (t == "&") throw ParseFail("UNENCODED: address-of");
        if (std::isdigit(static_cast<unsigned char>(t[0]))) return int_literal(e, t);
        if (is_ident(t)) {
            if (peek() == "::") throw ParseFail("UNENCODED: qualified name " + t + "::" + peek(1));
            if (t == "static_cast" && peek() == "<") {
                // static_cast<T>(e) between integer types is the C cast (T)e.
                eat("<");
                std::string joined;
                while (!peek().empty() && peek() != ">") joined += (joined.empty() ? "" : " ") + eat();
                eat(">");
                auto ct = ctype_parse(joined);
                if (!ct) throw ParseFail("UNENCODED: static_cast to " + joined);
                eat("(");
                auto v = parse(0);
                eat(")");
                return e.conv(v, *ct);
            }
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
                if (!e.is_array(t)) throw ParseFail("UNENCODED: index into unknown array " + t);
                auto arr = e.arrays.at(e.canonical(t));
                e.add_prop("oob-read", "MEM-OOB-READ", oob(e, idx, arr.n), e.pc);
                check_elem(e, e.canonical(t), idx, arr.n);
                return TV{z3::select(arr.a, index32(e, idx)), arr.w, arr.u};
            }
            if (peek() == "(" && !e.declared(t)) {
                eat("(");
                std::vector<TV> args;
                size_t n_values = e.value_arrays.size();
                if (peek() == ")") eat(")");
                else {
                    while (true) {
                        args.push_back(parse(2));
                        if (peek() == ",") { eat(","); continue; }
                        eat(")");
                        break;
                    }
                }
                size_t n_calls = e.call_vars.size();
                auto res = model_call(e, t, args);
                if (e.call_vars.size() > n_calls) {
                    // unmodelled callee: every array its arguments mention escapes
                    std::vector<std::string> seen;
                    for (size_t k = n_values; k < e.value_arrays.size(); ++k) {
                        auto an = e.value_arrays[k];
                        if (std::find(seen.begin(), seen.end(), an) != seen.end()) continue;
                        seen.push_back(an);
                        escape_array(e, an);
                    }
                }
                e.value_arrays.resize(n_values);
                return res;
            }
            if (peek() == "++" || peek() == "--") {
                auto op = eat();
                return incdec(t, op, false);
            }
            if (e.is_array(t)) {  // array/pointer as a value: non-null
                e.value_arrays.push_back(e.canonical(t));
                return e.int_val(1);
            }
            if (e.bits.count(t)) {
                e.check_read(t);
                return e.get(t);
            }
            if (parser.enums.count(t)) return e.int_val(parser.enums.at(t));
            throw ParseFail("UNENCODED: identifier " + t);
        }
        throw ParseFail("bad token " + t);
    };
    parse = [&](int minp) -> TV {
        auto left = nud();
        while (PREC.count(peek()) && PREC.at(peek()) >= minp) {
            auto op = eat();
            int p = PREC.at(op) + 1;
            if (op == "&&") {
                auto lb = as_bool(left);
                auto right = guarded(e, lb, [&] { return parse(p); });
                left = bool_tv(e, lb && as_bool(right));
            } else if (op == "||") {
                auto lb = as_bool(left);
                auto right = guarded(e, !lb, [&] { return parse(p); });
                left = bool_tv(e, lb || as_bool(right));
            } else {
                auto right = parse(p);
                left = apply_binop(e, left, op, right);
            }
        }
        // C ternary binds below ||, right-associative; only the taken arm runs.
        if (minp <= 5 && peek() == "?") {
            eat("?");
            auto c = as_bool(left);
            auto then_v = guarded(e, c, [&] { return parse(0); });
            eat(":");
            auto else_v = guarded(e, !c, [&] { return parse(5); });
            auto ct = e.common(then_v, else_v);
            left = TV{z3::ite(c, e.conv(then_v, ct).v, e.conv(else_v, ct).v), ct.w, ct.u};
        }
        // Comma: evaluate left for side effects; value is the right.
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
            auto d = model.eval(e.ctx.bv_const(name.c_str(), static_cast<unsigned>(ctype_of(typ).w)), true);
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
