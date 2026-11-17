// Exact x86-64 instruction semantics for inline asm (workstream W4, docs/ASM_CENSUS.md).
// Included by translate.cpp after translate_kernel.inc: Tr member functions.
//
// asm_exact decides, for one LLVM `call asm "<template>", "<constraints>"(...)`:
//   * a template made only of the instructions below, every operand a numbered
//     operand ($N) or %cl, constraints of the forms clang emits: modelled
//     exactly (register, memory and the flags named by "=@cc<cond>" outputs);
//   * a template with a privileged instruction (rdmsr, wrmsr, in, out, cli,
//     sti, hlt, cpuid, control-register moves, ...): the existing havoc,
//     under the assumption asm-privileged;
//   * anything else: false, and the caller keeps the asm-havoc model.
// Instructions: add sub and or xor adc sbb cmp test inc dec neg not, bswap
// popcnt lzcnt tzcnt bsf bsr, shl sal shr sar rol ror (32/64 bit), bt bts btr
// btc (register bit number or constant), xchg xadd cmpxchg (register or one
// memory operand, lock prefix), mul imul (one operand), imul (two operand),
// div idiv (32 bit), stc clc cmc, nop pause lfence mfence sfence, rdtsc.
// Flags are tracked per instruction (CF ZF SF OF); a flag the instruction
// leaves undefined is unknown, and a flag output that needs an unknown flag is
// a free value. adc/sbb need a known carry (from a previous instruction of the
// same template).
// Assumptions named: asm-exact (sequential model: no other thread, interrupt
// or device between the accesses, atomicity of lock-prefixed instructions not
// modelled, flags not named as outputs are clobbered and not modelled, a
// memory clobber is a compiler barrier and has no effect here), asm-nondet
// (rdtsc is an arbitrary value, nothing assumed about monotonicity),
// asm-privileged.

namespace {

struct AsmCon {
    char kind = 'I';  // D direct output, F flag output, M indirect output (memory), m indirect input (memory), I input
    int argi = -1;
    int tied = -1;
    bool ax = false, dx = false, cx = false;
    std::string cc;
    int dk = -1;  // index among the direct outputs
    bool imm = false;  // a literal immediate of the template (13279915): synthetic input constraint
};

struct AsmFlags {
    std::optional<Arg> cf, zf, sf, of;
};

bool all_digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

}  // namespace

bool Tr::asm_exact(Frame& fr, const ir::Inst& in, int& cur, int line) {
    namespace am = prism::pir::asmodel;
    if (in.asm_text.empty()) return false;
    // privileged first (also for templates that do not parse, e.g. `out %al,(%dx)`)
    if (auto priv = am::x86_privileged_in(in.asm_text)) {
        asm_havoc(fr, in, cur, line, "asm-privileged",
                  "privileged or system instruction '" + *priv +
                      "' (ring 0, port, MSR, control register or device effect not modelled: outputs and memory havocked) ");
        return true;
    }
    auto parsed = am::parse_x86_template(in.asm_text);
    if (!parsed) return false;
    const auto& prog = *parsed;

    // ---- constraints ----
    std::vector<AsmCon> con;
    int ndirect = 0, nargs = 0;
    for (auto c : split_commas(in.asm_constraints)) {
        if (c.starts_with("~{")) continue;
        if (c.empty() || c[0] == '+') return false;
        const bool is_out = c[0] == '=';
        std::string body = is_out ? c.substr(1) : c;
        const bool indirect = body.find('*') != std::string::npos;
        AsmCon k;
        if (is_out && !indirect) {
            k.kind = 'D';
            k.dk = ndirect++;
            if (body.starts_with("{@cc")) {
                k.kind = 'F';
                k.cc = body.substr(4, body.size() - 5);
            } else if (body == "{ax}") k.ax = true;
            else if (body == "{dx}") k.dx = true;
            else if (body == "{cx}") k.cx = true;
        } else if (is_out) {
            k.kind = 'M';
            k.argi = nargs++;
        } else if (indirect) {
            k.kind = 'm';
            k.argi = nargs++;
        } else {
            k.kind = 'I';
            k.argi = nargs++;
            if (all_digits(body)) k.tied = std::stoi(body);
            else if (body == "{ax}") k.ax = true;
            else if (body == "{dx}") k.dx = true;
            else if (body == "{cx}") k.cx = true;
        }
        con.push_back(k);
    }
    if (static_cast<std::size_t>(nargs) != in.ops.size()) return false;
    std::vector<Arg> args;
    for (auto& o : in.ops) args.push_back(operand(fr, o, cur, line));

    // the direct outputs' variables
    std::vector<int> dvars;
    if (!in.result.empty()) {
        if (auto pit = fr.pairs.find(in.result); pit != fr.pairs.end()) {
            dvars = {pit->second.first, pit->second.second};
        } else if (in.ty.kind == ir::Type::Int) {
            dvars = {result_var(fr, in)};
        } else {
            return false;
        }
    }
    if (static_cast<int>(dvars.size()) != ndirect) return false;
    auto dwidth = [&](const AsmCon& k) { return out.vars[static_cast<std::size_t>(dvars[static_cast<std::size_t>(k.dk)])].width; };
    int axn = -1, dxn = -1, cxn = -1;  // accumulator constraints
    for (std::size_t i = 0; i < con.size(); ++i) {
        const auto& k = con[i];
        if (k.kind == 'D' && k.ax) axn = static_cast<int>(i);
        if (k.kind == 'D' && k.dx) dxn = static_cast<int>(i);
        if (k.kind == 'I' && k.cx) cxn = static_cast<int>(i);
    }

    // ---- mnemonics ----
    static const std::set<std::string> bases{
        "add", "sub", "and", "or", "xor", "adc", "sbb", "cmp", "test", "inc", "dec", "neg", "not", "bswap",
        "popcnt", "lzcnt", "tzcnt", "bsf", "bsr", "shl", "sal", "shr", "sar", "rol", "ror", "bt", "bts", "btr",
        "btc", "xchg", "xadd", "cmpxchg", "mul", "imul", "div", "idiv", "stc", "clc", "cmc", "nop", "pause",
        "lfence", "mfence", "sfence", "rdtsc"};
    struct Ins {
        std::string base;
        unsigned sw = 0;  // width from the suffix, 0 none
        const am::X86Ins* src = nullptr;
        unsigned w = 0;  // operation width
        std::vector<int> n;  // operand numbers; -1 for %cl
    };
    std::vector<Ins> plan;
    for (auto& pi : prog) {
        Ins I;
        I.src = &pi;
        if (bases.count(pi.mn)) {
            I.base = pi.mn;
        } else if (pi.mn.size() > 1 && std::string("qlwb").find(pi.mn.back()) != std::string::npos &&
                   bases.count(pi.mn.substr(0, pi.mn.size() - 1))) {
            I.base = pi.mn.substr(0, pi.mn.size() - 1);
            I.sw = pi.mn.back() == 'q' ? 64 : pi.mn.back() == 'l' ? 32 : pi.mn.back() == 'w' ? 16 : 8;
        } else {
            return false;
        }
        for (auto& o : pi.ops) {
            if (o == "%cl") {
                I.n.push_back(-1);
            } else if (o.size() > 2 && o[0] == '$' && o[1] == '$') {
                uint64_t iv = 0;
                try {
                    std::size_t used = 0;
                    iv = static_cast<uint64_t>(std::stoll(o.substr(2), &used, 0));
                    if (used != o.size() - 2) return false;
                } catch (const std::exception&) {
                    return false;
                }
                AsmCon k;
                k.kind = 'I';
                k.imm = true;
                k.argi = static_cast<int>(args.size());
                args.push_back(Arg::c(64, iv));
                con.push_back(k);
                I.n.push_back(static_cast<int>(con.size()) - 1);
            } else if (o.size() > 1 && o[0] == '$' && all_digits(o.substr(1))) {
                int v = std::stoi(o.substr(1));
                if (v < 0 || v >= static_cast<int>(con.size()) || con[static_cast<std::size_t>(v)].kind == 'F') return false;
                I.n.push_back(v);
            } else {
                return false;
            }
        }
        plan.push_back(std::move(I));
    }
    auto is_reg = [&](int n) { return n >= 0 && (con[static_cast<std::size_t>(n)].kind == 'D' || con[static_cast<std::size_t>(n)].kind == 'I'); };
    auto is_mem = [&](int n) { return n >= 0 && (con[static_cast<std::size_t>(n)].kind == 'M' || con[static_cast<std::size_t>(n)].kind == 'm'); };
    auto writable = [&](int n) { return n >= 0 && (con[static_cast<std::size_t>(n)].kind == 'D' || con[static_cast<std::size_t>(n)].kind == 'M'); };
    auto regw = [&](int n) -> unsigned {
        const auto& k = con[static_cast<std::size_t>(n)];
        return k.kind == 'D' ? dwidth(k) : args[static_cast<std::size_t>(k.argi)].width;
    };
    // initial register values: an output tied to an input starts with that input's value
    std::set<int> hasval;
    for (std::size_t i = 0; i < con.size(); ++i) {
        if (con[i].kind == 'I') hasval.insert(static_cast<int>(i));
        if (con[i].kind == 'I' && con[i].tied >= 0) hasval.insert(con[i].tied);
    }
    // validate every instruction (nothing is emitted before all of them pass)
    bool cf_known = false;
    bool all_fence = true, any_nondet = false, any_lock = false;
    for (auto& I : plan) {
        const auto& b = I.base;
        const std::size_t nop = I.n.size();
        auto need = [&](std::size_t lo, std::size_t hi) { return nop >= lo && nop <= hi; };
        bool fence = b == "nop" || b == "pause" || b == "lfence" || b == "mfence" || b == "sfence";
        if (!fence) all_fence = false;
        if (I.src->lock) any_lock = true;
        for (int n : I.n)
            if (n >= 0 && !is_reg(n) && !is_mem(n)) return false;
        // operation width
        unsigned w = I.sw;
        if (!w)
            for (int n : I.n)
                if (n >= 0 && is_reg(n) && !con[static_cast<std::size_t>(n)].imm && !(b.starts_with("sh") || b == "sal" || b == "sar" || b == "rol" || b == "ror" ||
                                              b.starts_with("bt")) ) {
                    w = regw(n);
                    break;
                }
        if (!w && (b.starts_with("sh") || b == "sal" || b == "sar" || b == "rol" || b == "ror" || b.starts_with("bt")) && nop > 0 &&
            is_reg(I.n.back()))
            w = regw(I.n.back());
        if (b == "rdtsc") {
            if (nop != 0 || axn < 0 || dxn < 0 || dwidth(con[static_cast<std::size_t>(axn)]) != 32 ||
                dwidth(con[static_cast<std::size_t>(dxn)]) != 32)
                return false;
            any_nondet = true;
            cf_known = false;
            I.w = 32;
            continue;
        }
        if (fence || b == "stc" || b == "clc" || b == "cmc") {
            if (nop != 0) return false;
            if (b == "stc" || b == "clc") cf_known = true;
            else if (b == "cmc" && !cf_known) return false;
            continue;
        }
        if (w != 8 && w != 16 && w != 32 && w != 64) return false;
        I.w = w;
        auto src_ok = [&](int n) {  // readable operand of width w
            if (n < 0) return false;
            if (is_mem(n)) return true;
            if (con[static_cast<std::size_t>(n)].imm) return true;
            return regw(n) == w && hasval.count(n) != 0;
        };
        auto dst_ok = [&](int n) { return writable(n) && (is_mem(n) || regw(n) == w); };
        auto dst_rw = [&](int n) { return dst_ok(n) && (is_mem(n) || hasval.count(n) != 0); };
        if (b == "add" || b == "sub" || b == "and" || b == "or" || b == "xor" || b == "adc" || b == "sbb") {
            if (!need(2, 2) || !src_ok(I.n[0]) || !dst_rw(I.n[1])) return false;
            if ((b == "adc" || b == "sbb") && !cf_known) return false;
            if (b == "add" || b == "sub" || b == "adc" || b == "sbb" || b == "and" || b == "or" || b == "xor") cf_known = true;
        } else if (b == "cmp" || b == "test") {
            if (!need(2, 2) || !src_ok(I.n[0]) || !src_ok(I.n[1])) return false;
            cf_known = true;
        } else if (b == "inc" || b == "dec" || b == "not") {
            if (!need(1, 1) || !dst_rw(I.n[0])) return false;
        } else if (b == "neg") {
            if (!need(1, 1) || !dst_rw(I.n[0])) return false;
            cf_known = true;
        } else if (b == "bswap") {
            if (!need(1, 1) || (w != 32 && w != 64) || !dst_rw(I.n[0]) || is_mem(I.n[0])) return false;
        } else if (b == "popcnt" || b == "lzcnt" || b == "tzcnt" || b == "bsf" || b == "bsr") {
            if (!need(2, 2) || w < 16 || !src_ok(I.n[0]) || !dst_ok(I.n[1]) || is_mem(I.n[1])) return false;
            cf_known = b != "bsf" && b != "bsr";
            if (b == "bsf" || b == "bsr") cf_known = false;
        } else if (b == "shl" || b == "sal" || b == "shr" || b == "sar" || b == "rol" || b == "ror") {
            if (!need(1, 2) || (w != 32 && w != 64)) return false;
            int dst = I.n.back();
            if (!dst_rw(dst)) return false;
            if (nop == 2) {
                int cn = I.n[0];
                if (cn == -1 ? cxn < 0 : (is_mem(cn) || !is_reg(cn) || !hasval.count(cn))) return false;
            }
            cf_known = false;
        } else if (b == "bt" || b == "bts" || b == "btr" || b == "btc") {
            if (!need(2, 2)) return false;
            int bit = I.n[0], dst = I.n[1];
            if (bit < 0 || con[static_cast<std::size_t>(bit)].kind != 'I') return false;
            if (is_mem(dst) && !args[static_cast<std::size_t>(con[static_cast<std::size_t>(bit)].argi)].is_const) return false;
            if (b == "bt") {
                if (!(is_mem(dst) || (is_reg(dst) && regw(dst) == w && hasval.count(dst)))) return false;
            } else if (!dst_rw(dst)) {
                return false;
            }
            cf_known = true;
        } else if (b == "xchg") {
            if (!need(2, 2)) return false;
            int x = I.n[0], y = I.n[1];
            if (!dst_rw(x) || !dst_rw(y) || (is_mem(x) && is_mem(y))) return false;
        } else if (b == "xadd") {
            if (!need(2, 2) || !dst_rw(I.n[0]) || !dst_rw(I.n[1])) return false;
            cf_known = true;
        } else if (b == "cmpxchg") {
            if (!need(2, 2) || axn < 0 || !hasval.count(axn) || dwidth(con[static_cast<std::size_t>(axn)]) != w) return false;
            if (!src_ok(I.n[0]) || is_mem(I.n[0]) || !dst_rw(I.n[1])) return false;
            cf_known = true;
        } else if (b == "mul" || b == "imul" || b == "div" || b == "idiv") {
            const bool two = b == "imul" && nop == 2;
            if (two) {
                if (!src_ok(I.n[0]) || !dst_rw(I.n[1]) || is_mem(I.n[1])) return false;
                cf_known = true;
            } else {
                if (!need(1, 1) || (w != 32 && w != 64) || !src_ok(I.n[0])) return false;
                if (axn < 0 || dxn < 0 || !hasval.count(axn) || dwidth(con[static_cast<std::size_t>(axn)]) != w ||
                    dwidth(con[static_cast<std::size_t>(dxn)]) != w)
                    return false;
                if ((b == "div" || b == "idiv") && (w != 32 || !hasval.count(dxn))) return false;
                cf_known = b == "mul" || b == "imul";
            }
            if (b == "div" || b == "idiv") cf_known = false;
        } else {
            return false;
        }
    }

    // ---- execution ----
    std::map<int, Arg> val;
    std::set<int> written;
    for (std::size_t i = 0; i < con.size(); ++i) {
        const auto& k = con[i];
        if (k.kind != 'I') continue;
        val[static_cast<int>(i)] = args[static_cast<std::size_t>(k.argi)];
        if (k.tied >= 0) val[k.tied] = args[static_cast<std::size_t>(k.argi)];
    }
    AsmFlags fl;
    auto C = [](unsigned w, uint64_t v) { return Arg::c(w, v); };
    auto A = [&](Op op, unsigned w, std::vector<Arg> a) { return assign(cur, op, w, std::move(a), "asm"); };
    auto cmpb = [&](Op op, Arg x, Arg y) { return A(op, 1, {x, y}); };
    auto notb = [&](Arg x) { return A(Op::Xor, 1, {x, C(1, 1)}); };
    auto bitof = [&](Arg x, unsigned w, unsigned i) { return A(Op::Trunc, 1, {A(Op::LShr, w, {x, C(w, i)})}); };
    auto msb = [&](Arg x, unsigned w) { return bitof(x, w, w - 1); };
    auto resize = [&](Arg x, unsigned w) {
        if (x.width == w) return x;
        return A(x.width < w ? Op::ZExt : Op::Trunc, w, {x});
    };
    auto itype = [](unsigned w) { return ir::Type{ir::Type::Int, w, "i" + std::to_string(w), {}}; };
    auto ptr_of = [&](int n) { return args[static_cast<std::size_t>(con[static_cast<std::size_t>(n)].argi)]; };
    auto rd = [&](int n, unsigned w) -> Arg {
        if (is_mem(n)) {
            int v = newvar(tmpname("asm.m"), w);
            mt.load(cur, v, itype(w), ptr_of(n), 1, line);
            return Arg::v(v, w);
        }
        if (con[static_cast<std::size_t>(n)].imm) return C(w, val.at(n).bits);
        return val.at(n);
    };
    auto wr = [&](int n, Arg a, unsigned w) {
        if (is_mem(n)) {
            mt.store(cur, ptr_of(n), a, Arg::c(1, 1), itype(w), 1, line);
        } else {
            val[n] = a;
            written.insert(n);
        }
    };
    auto set_zs = [&](Arg r, unsigned w) {
        fl.zf = cmpb(Op::Eq, r, C(w, 0));
        fl.sf = msb(r, w);
    };
    auto logic_flags = [&](Arg r, unsigned w) {
        fl.cf = C(1, 0);
        fl.of = C(1, 0);
        set_zs(r, w);
    };
    auto add_flags = [&](Arg a, Arg b, Arg r, unsigned w, std::optional<Arg> cin) {
        fl.cf = cin ? A(Op::Select, 1, {*cin, cmpb(Op::Ule, r, a), cmpb(Op::Ult, r, a)}) : cmpb(Op::Ult, r, a);
        fl.of = msb(A(Op::And, w, {A(Op::Xor, w, {a, r}), A(Op::Xor, w, {b, r})}), w);
        set_zs(r, w);
    };
    auto sub_flags = [&](Arg a, Arg b, Arg r, unsigned w, std::optional<Arg> cin) {
        fl.cf = cin ? A(Op::Select, 1, {*cin, cmpb(Op::Ule, a, b), cmpb(Op::Ult, a, b)}) : cmpb(Op::Ult, a, b);
        fl.of = msb(A(Op::And, w, {A(Op::Xor, w, {a, b}), A(Op::Xor, w, {a, r})}), w);
        set_zs(r, w);
    };
    auto unknown_flags = [&] { fl = AsmFlags{}; };

    for (auto& I : plan) {
        const auto& b = I.base;
        const unsigned w = I.w;
        const auto& n = I.n;
        if (b == "rdtsc") {
            val[axn] = havoc(cur, 32, false, false);
            val[dxn] = havoc(cur, 32, false, false);
            written.insert(axn);
            written.insert(dxn);
            unknown_flags();
        } else if (b == "stc") {
            fl.cf = C(1, 1);
        } else if (b == "clc") {
            fl.cf = C(1, 0);
        } else if (b == "cmc") {
            fl.cf = notb(fl.cf.value());
        } else if (b == "nop" || b == "pause" || b == "lfence" || b == "mfence" || b == "sfence") {
            // ordering and hints only: no effect on the sequential model
        } else if (b == "add" || b == "sub" || b == "and" || b == "or" || b == "xor" || b == "adc" || b == "sbb") {
            Arg s = rd(n[0], w), d = rd(n[1], w), r;
            if (b == "add") {
                r = A(Op::Add, w, {d, s});
                add_flags(d, s, r, w, std::nullopt);
            } else if (b == "sub") {
                r = A(Op::Sub, w, {d, s});
                sub_flags(d, s, r, w, std::nullopt);
            } else if (b == "adc") {
                Arg cin = fl.cf.value();
                r = A(Op::Add, w, {A(Op::Add, w, {d, s}), resize(cin, w)});
                add_flags(d, s, r, w, cin);
            } else if (b == "sbb") {
                Arg cin = fl.cf.value();
                r = A(Op::Sub, w, {A(Op::Sub, w, {d, s}), resize(cin, w)});
                sub_flags(d, s, r, w, cin);
            } else {
                r = A(b == "and" ? Op::And : b == "or" ? Op::Or : Op::Xor, w, {d, s});
                logic_flags(r, w);
            }
            wr(n[1], r, w);
        } else if (b == "cmp") {
            Arg s = rd(n[0], w), d = rd(n[1], w);
            sub_flags(d, s, A(Op::Sub, w, {d, s}), w, std::nullopt);
        } else if (b == "test") {
            Arg s = rd(n[0], w), d = rd(n[1], w);
            logic_flags(A(Op::And, w, {d, s}), w);
        } else if (b == "inc" || b == "dec") {
            Arg d = rd(n[0], w);
            Arg r = A(b == "inc" ? Op::Add : Op::Sub, w, {d, C(w, 1)});
            fl.of = cmpb(Op::Eq, r, C(w, b == "inc" ? (uint64_t{1} << (w - 1)) : ((uint64_t{1} << (w - 1)) - 1)));
            set_zs(r, w);
            wr(n[0], r, w);
        } else if (b == "neg") {
            Arg d = rd(n[0], w);
            Arg r = A(Op::Sub, w, {C(w, 0), d});
            fl.cf = cmpb(Op::Ne, d, C(w, 0));
            fl.of = cmpb(Op::Eq, d, C(w, uint64_t{1} << (w - 1)));
            set_zs(r, w);
            wr(n[0], r, w);
        } else if (b == "not") {
            wr(n[0], A(Op::Xor, w, {rd(n[0], w), C(w, w == 64 ? ~uint64_t{0} : ((uint64_t{1} << w) - 1))}), w);
        } else if (b == "bswap") {
            wr(n[0], A(Op::Bswap, w, {rd(n[0], w)}), w);
        } else if (b == "popcnt" || b == "lzcnt" || b == "tzcnt" || b == "bsf" || b == "bsr") {
            Arg s = rd(n[0], w);
            Arg z = cmpb(Op::Eq, s, C(w, 0));
            Arg r;
            unknown_flags();
            if (b == "popcnt") {
                r = A(Op::Ctpop, w, {s});
                fl.cf = C(1, 0);
                fl.of = C(1, 0);
                fl.sf = C(1, 0);
                fl.zf = z;
            } else if (b == "lzcnt" || b == "tzcnt") {
                r = A(b == "lzcnt" ? Op::Ctlz : Op::Cttz, w, {s});
                fl.cf = z;
                fl.zf = cmpb(Op::Eq, r, C(w, 0));
            } else {
                // bsf/bsr: destination undefined (unchanged on Intel) when the source is 0
                Arg v = b == "bsf" ? A(Op::Cttz, w, {s}) : A(Op::Sub, w, {C(w, w - 1), A(Op::Ctlz, w, {s})});
                r = A(Op::Select, w, {z, havoc(cur, w, false, false), v});
                fl.zf = z;
            }
            wr(n[1], r, w);
        } else if (b == "shl" || b == "sal" || b == "shr" || b == "sar" || b == "rol" || b == "ror") {
            int dst = n.back();
            Arg cnt = C(w, 1);
            if (n.size() == 2) cnt = resize(n[0] == -1 ? val.at(cxn) : val.at(n[0]), w);
            Arg amt = A(Op::And, w, {cnt, C(w, w == 64 ? 63 : 31)});
            Arg d = rd(dst, w), r;
            if (b == "shl" || b == "sal") r = A(Op::Shl, w, {d, amt});
            else if (b == "shr") r = A(Op::LShr, w, {d, amt});
            else if (b == "sar") r = A(Op::AShr, w, {d, amt});
            else {
                Arg inv = A(Op::And, w, {A(Op::Sub, w, {C(w, w), amt}), C(w, w - 1)});
                if (b == "rol") r = A(Op::Or, w, {A(Op::Shl, w, {d, amt}), A(Op::LShr, w, {d, inv})});
                else r = A(Op::Or, w, {A(Op::LShr, w, {d, amt}), A(Op::Shl, w, {d, inv})});
            }
            unknown_flags();
            wr(dst, r, w);
        } else if (b == "bt" || b == "bts" || b == "btr" || b == "btc") {
            Arg bitn = A(Op::And, w, {resize(rd(n[0], w), w), C(w, w - 1)});
            Arg d = rd(n[1], w);
            Arg mask = A(Op::Shl, w, {C(w, 1), bitn});
            unknown_flags();
            fl.cf = A(Op::Trunc, 1, {A(Op::LShr, w, {d, bitn})});
            if (b != "bt") {
                Arg r = b == "bts" ? A(Op::Or, w, {d, mask})
                        : b == "btr" ? A(Op::And, w, {d, A(Op::Xor, w, {mask, C(w, w == 64 ? ~uint64_t{0} : ((uint64_t{1} << w) - 1))})})
                                     : A(Op::Xor, w, {d, mask});
                wr(n[1], r, w);
            }
        } else if (b == "xchg") {
            Arg x = rd(n[0], w), y = rd(n[1], w);
            wr(n[0], y, w);
            wr(n[1], x, w);
        } else if (b == "xadd") {
            Arg s = rd(n[0], w), d = rd(n[1], w);
            Arg r = A(Op::Add, w, {d, s});
            add_flags(d, s, r, w, std::nullopt);
            wr(n[0], d, w);
            wr(n[1], r, w);
        } else if (b == "cmpxchg") {
            Arg s = rd(n[0], w), d = rd(n[1], w), acc = val.at(axn);
            Arg eq = cmpb(Op::Eq, acc, d);
            sub_flags(acc, d, A(Op::Sub, w, {acc, d}), w, std::nullopt);
            wr(n[1], A(Op::Select, w, {eq, s, d}), w);
            wr(axn, A(Op::Select, w, {eq, acc, d}), w);
        } else if (b == "imul" && n.size() == 2) {
            Arg s = rd(n[0], w), d = rd(n[1], w);
            Arg ovf = A(Op::SMulOvf, 1, {d, s});
            unknown_flags();
            fl.cf = ovf;
            fl.of = ovf;
            wr(n[1], A(Op::Mul, w, {d, s}), w);
        } else if (b == "mul" || b == "imul") {
            Arg s = rd(n[0], w), a = val.at(axn);
            const bool sg = b == "imul";
            Arg lo = A(Op::Mul, w, {a, s}), hi;
            if (w == 32) {
                Arg x = A(sg ? Op::SExt : Op::ZExt, 64, {a}), y = A(sg ? Op::SExt : Op::ZExt, 64, {s});
                hi = A(Op::Trunc, 32, {A(Op::LShr, 64, {A(Op::Mul, 64, {x, y}), C(64, 32)})});
            } else {
                // 64x64 -> high 64 bits by 32-bit limbs
                Arg m32 = C(64, 0xffffffffu);
                Arg al = A(Op::And, 64, {a, m32}), ah = A(Op::LShr, 64, {a, C(64, 32)});
                Arg sl = A(Op::And, 64, {s, m32}), sh = A(Op::LShr, 64, {s, C(64, 32)});
                Arg ll = A(Op::Mul, 64, {al, sl}), lh = A(Op::Mul, 64, {al, sh});
                Arg hl = A(Op::Mul, 64, {ah, sl}), hh = A(Op::Mul, 64, {ah, sh});
                Arg mid = A(Op::Add, 64, {A(Op::Add, 64, {A(Op::LShr, 64, {ll, C(64, 32)}), A(Op::And, 64, {lh, m32})}),
                                          A(Op::And, 64, {hl, m32})});
                hi = A(Op::Add, 64, {A(Op::Add, 64, {hh, A(Op::LShr, 64, {lh, C(64, 32)})}),
                                     A(Op::Add, 64, {A(Op::LShr, 64, {hl, C(64, 32)}), A(Op::LShr, 64, {mid, C(64, 32)})})});
            }
            if (sg && w == 64) {
                // signed high = unsigned high - (a < 0 ? s : 0) - (s < 0 ? a : 0)
                Arg an = A(Op::Select, 64, {msb(a, 64), s, C(64, 0)});
                Arg sn = A(Op::Select, 64, {msb(s, 64), a, C(64, 0)});
                hi = A(Op::Sub, 64, {A(Op::Sub, 64, {hi, an}), sn});
            }
            unknown_flags();
            Arg ovf = sg ? cmpb(Op::Ne, hi, A(Op::AShr, w, {lo, C(w, w - 1)})) : cmpb(Op::Ne, hi, C(w, 0));
            fl.cf = ovf;
            fl.of = ovf;
            val[axn] = lo;
            val[dxn] = hi;
            written.insert(axn);
            written.insert(dxn);
        } else if (b == "div" || b == "idiv") {
            const bool sg = b == "idiv";
            Arg s = rd(n[0], 32);
            Arg nn = A(Op::Or, 64, {A(Op::Shl, 64, {A(Op::ZExt, 64, {val.at(dxn)}), C(64, 32)}), A(Op::ZExt, 64, {val.at(axn)})});
            Arg dv = A(sg ? Op::SExt : Op::ZExt, 64, {s});
            Arg q = A(sg ? Op::SDiv : Op::UDiv, 64, {nn, dv});
            Arg rem = A(sg ? Op::SRem : Op::URem, 64, {nn, dv});
            Arg bad = cmpb(Op::Eq, dv, C(64, 0));
            Arg ovf = sg ? A(Op::Or, 1, {cmpb(Op::Slt, q, C(64, ~uint64_t{0} << 31)), cmpb(Op::Sgt, q, C(64, 0x7fffffffu))})
                         : cmpb(Op::Ne, A(Op::LShr, 64, {q, C(64, 32)}), C(64, 0));
            check(cur, A(Op::Or, 1, {bad, ovf}), "asm-div", "INT-DIV-ZERO",
                  "divide error (#DE): divisor 0 or quotient does not fit the 32-bit register", line);
            val[axn] = A(Op::Trunc, 32, {q});
            val[dxn] = A(Op::Trunc, 32, {rem});
            written.insert(axn);
            written.insert(dxn);
            unknown_flags();
        }
    }

    // ---- outputs ----
    auto flag_of = [&](const std::string& cc) -> std::optional<Arg> {
        auto nb = [&](std::optional<Arg> x) -> std::optional<Arg> {
            if (!x) return std::nullopt;
            return notb(*x);
        };
        if (cc == "c" || cc == "b" || cc == "nae") return fl.cf;
        if (cc == "nc" || cc == "ae" || cc == "nb") return nb(fl.cf);
        if (cc == "z" || cc == "e") return fl.zf;
        if (cc == "nz" || cc == "ne") return nb(fl.zf);
        if (cc == "s") return fl.sf;
        if (cc == "ns") return nb(fl.sf);
        if (cc == "o") return fl.of;
        if (cc == "no") return nb(fl.of);
        if (cc == "a" || cc == "nbe") return fl.cf && fl.zf ? std::optional<Arg>(notb(A(Op::Or, 1, {fl.cf.value(), *fl.zf}))) : std::nullopt;
        if (cc == "be" || cc == "na") return fl.cf && fl.zf ? std::optional<Arg>(A(Op::Or, 1, {fl.cf.value(), *fl.zf})) : std::nullopt;
        if (cc == "l" || cc == "nge") return fl.sf && fl.of ? std::optional<Arg>(cmpb(Op::Ne, *fl.sf, *fl.of)) : std::nullopt;
        if (cc == "ge" || cc == "nl") return fl.sf && fl.of ? std::optional<Arg>(cmpb(Op::Eq, *fl.sf, *fl.of)) : std::nullopt;
        if (cc == "le" || cc == "ng")
            return fl.sf && fl.of && fl.zf ? std::optional<Arg>(A(Op::Or, 1, {*fl.zf, cmpb(Op::Ne, *fl.sf, *fl.of)})) : std::nullopt;
        if (cc == "g" || cc == "nle")
            return fl.sf && fl.of && fl.zf
                       ? std::optional<Arg>(A(Op::And, 1, {notb(*fl.zf), cmpb(Op::Eq, *fl.sf, *fl.of)}))
                       : std::nullopt;
        return std::nullopt;
    };
    std::size_t nflag_free = 0;
    for (std::size_t i = 0; i < con.size(); ++i) {
        const auto& k = con[i];
        if (k.kind != 'D' && k.kind != 'F') continue;
        const int dv = dvars[static_cast<std::size_t>(k.dk)];
        const unsigned vw = out.vars[static_cast<std::size_t>(dv)].width;
        if (k.kind == 'F') {
            auto f = flag_of(k.cc);
            if (!f) {
                ++nflag_free;
                havoc(cur, vw, false, false, dv);
            } else {
                emit_assign(cur, dv, Op::Copy, {resize(*f, vw)});
            }
        } else if (val.count(static_cast<int>(i))) {
            emit_assign(cur, dv, Op::Copy, {resize(val.at(static_cast<int>(i)), vw)});
        } else {
            havoc(cur, vw, false, false, dv);
        }
    }

    // ---- the named assumptions ----
    std::string text;
    for (auto& I : plan) text += (text.empty() ? "" : "; ") + std::string(I.src->lock ? "lock " : "") + I.src->mn;
    if (any_nondet)
        note_assumption("asm-nondet: rdtsc (line " + std::to_string(line) +
                        "): the time-stamp counter is an arbitrary value; nothing is assumed about its monotonicity or relation to time");
    std::string what = "asm-exact: \"" + text + "\" (line " + std::to_string(line) + "): x86-64 instruction semantics " +
                       "(register, memory and the flags named as outputs) in the sequential model: ";
    if (all_fence)
        what += "fence/hint: ordering only between agents, no effect on this function's values";
    else
        what += "no other thread, interrupt or device between its accesses";
    if (any_lock) what += "; lock prefix: atomicity not modelled (sequential execution)";
    what += "; flags not named as outputs are clobbered and not modelled";
    if (nflag_free) what += "; " + std::to_string(nflag_free) + " flag output(s) of an undefined flag are arbitrary";
    what += "; a memory clobber is a compiler barrier with no effect here";
    note_assumption(what);
    (void)written;
    return true;
}
