// The pir stage: Clang -> LLVM IR -> normalisation -> PIR -> Z3 (docs/PIR.md).
//
// Clang and opt only compile the scanned code; nothing from the scanned tree
// runs unless --allow-exec is given (Law 9). With it, every verdict is
// translation-validated: the PIR interpreter and `lli` (sandboxed) run the
// same concrete inputs and must agree wherever PIR reports no UB.

#include "prism/laws.hpp"
#include "prism/pipeline.hpp"
#include "prism/pir.hpp"
#include "prism/sandbox.hpp"
#include "prism/threads.hpp"

#include "../proc.hpp"
#include "fp.hpp"
#include "lower_ctl.hpp"
#include "stage_mem.hpp"

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>

namespace prism::pir {
namespace fs = std::filesystem;

namespace {

constexpr int kTvVectors = 64;
constexpr const char* kInstall = "install clang and llvm 18 (apt install clang-18 llvm-18)";

std::optional<fs::path> tool(const Config& cfg, std::initializer_list<std::string_view> names) {
    for (auto n : names) {
        auto it = cfg.tools.find(std::string(n));
        if (it != cfg.tools.end()) {
            std::error_code ec;
            if (fs::is_regular_file(it->second, ec)) return it->second;
        }
    }
    return cfg.which(names);
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_file(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary);
    out << s;
    return static_cast<bool>(out);
}

struct TmpDir {
    fs::path path;
    TmpDir() {
        static std::atomic<unsigned> n{0};
        std::random_device rd;
        auto base = fs::temp_directory_path();
        for (int i = 0; i < 64; ++i) {
            auto cand = base / ("prism_pir_" + std::to_string(rd()) + "_" + std::to_string(n++));
            std::error_code ec;
            if (fs::create_directory(cand, ec)) {
                path = cand;
                return;
            }
        }
        throw std::runtime_error("pir: could not create a temporary directory");
    }
    ~TmpDir() {
        std::error_code ec;
        if (!path.empty()) fs::remove_all(path, ec);
    }
    TmpDir(const TmpDir&) = delete;
    TmpDir& operator=(const TmpDir&) = delete;
};

bool is_cxx(const fs::path& p) {
    auto e = p.extension().string();
    return e == ".cc" || e == ".cpp" || e == ".cxx" || e == ".C" || e == ".c++" || e == ".ii";
}

bool is_unit(const fs::path& p) { return p.extension() == ".c" || p.extension() == ".i" || is_cxx(p); }

std::string first_error(const std::string& text) {
    std::istringstream ss(text);
    std::string line;
    std::string fallback;
    while (std::getline(ss, line)) {
        if (line.find("error:") != std::string::npos) return line;
        if (fallback.empty() && !line.empty()) fallback = line;
    }
    return fallback.empty() ? "no output" : fallback;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        auto nl = s.find('\n', pos);
        if (nl == std::string::npos) nl = s.size();
        out.push_back(s.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

// clang warnings that mean "this constant expression has undefined
// behaviour and was folded to a value". The division-by-zero and
// shift-count cases fold to poison (or stay in the IR), which the poison
// marker / IR checks already cover.
const std::map<std::string, std::string>& folding_flags() {
    static const std::map<std::string, std::string> k{
        {"-Wshift-sign-overflow", "INT-SHIFT-UB"},
        {"-Wshift-negative-value", "INT-SHIFT-UB"},
        {"-Wshift-overflow", "INT-SHIFT-UB"},
        {"-Winteger-overflow", "INT-SIGNED-OVF"},
    };
    return k;
}

std::vector<FoldedUb> parse_folded(const std::string& text, const fs::path& src) {
    std::vector<FoldedUb> out;
    const auto want = src.string();
    for (auto& line : split_lines(text)) {
        auto w = line.find(": warning: ");
        if (w == std::string::npos) continue;
        auto lb = line.rfind(" [");
        if (lb == std::string::npos || line.back() != ']') continue;
        auto flag = line.substr(lb + 2, line.size() - lb - 3);
        auto it = folding_flags().find(flag);
        if (it == folding_flags().end()) continue;
        // path:line:col
        auto head = line.substr(0, w);
        auto c2 = head.rfind(':');
        if (c2 == std::string::npos) continue;
        auto c1 = head.rfind(':', c2 - 1);
        if (c1 == std::string::npos) continue;
        auto path = head.substr(0, c1);
        if (path != want && fs::path(path).filename() != src.filename()) continue;
        if (path != want) continue;  // header diagnostics: not attributed (docs/PIR.md)
        FoldedUb f;
        try {
            f.line = std::stoi(head.substr(c1 + 1, c2 - c1 - 1));
            f.col = std::stoi(head.substr(c2 + 1));
        } catch (...) {
            continue;
        }
        f.cls = it->second;
        f.msg = line.substr(w + 11, lb - w - 11) + " [" + flag + "]";
        out.push_back(std::move(f));
    }
    return out;
}

// "!N = !DILocation(line: L, column: C, ...)" -> line/col
std::map<std::string, std::pair<int, int>> scan_locs(const std::vector<std::string>& lines) {
    std::map<std::string, std::pair<int, int>> out;
    for (auto& l : lines) {
        if (l.empty() || l[0] != '!') continue;
        auto eq = l.find(" = ");
        if (eq == std::string::npos) continue;
        auto d = l.find("!DILocation(", eq);
        if (d == std::string::npos) continue;
        auto field = [&](const char* key) {
            auto p = l.find(key, d);
            if (p == std::string::npos) return 0;
            try {
                return std::stoi(l.substr(p + std::strlen(key)));
            } catch (...) {
                return 0;
            }
        };
        out[l.substr(0, eq)] = {field("line: "), field("column: ")};
    }
    return out;
}

std::string dbg_ref(const std::string& line) {
    auto p = line.find("!dbg !");
    if (p == std::string::npos) return {};
    auto e = p + 5;
    auto q = e + 1;
    while (q < line.size() && std::isdigit(static_cast<unsigned char>(line[q]))) ++q;
    return line.substr(e, q - e);
}

std::string dbg_suffix(const std::string& line) {
    auto r = dbg_ref(line);
    return r.empty() ? std::string() : ", !dbg " + r;
}

// Pre-mem2reg instrumentation of the -O0 IR (docs/PIR.md "Instrumentation
// before mem2reg"):
//  * every promotable iN local gets an initial value from
//    @__prism.uninit.iN() so a read before the first store stays visible
//    after mem2reg (mem2reg would otherwise fold the undef away);
//  * every `iN poison` operand clang emitted for folded UB becomes a call to
//    @__prism.poison.iN() at that instruction;
//  * every value-folding UB diagnostic gets @__prism.folded(i32 k) before
//    the instruction that carries its line.
std::string instrument(const std::string& ir, const std::vector<FoldedUb>& folded,
                       std::vector<int>& placed) {
    auto lines = split_lines(ir);
    auto locs = scan_locs(lines);
    // choose the marker position for each folded diagnostic
    std::map<std::size_t, std::vector<int>> markers;
    {
        bool in_body = false;
        std::vector<std::size_t> cand_idx;
        for (std::size_t k = 0; k < folded.size(); ++k) {
            const auto& f = folded[k];
            std::size_t best = std::string::npos;
            int best_col = -1;
            std::size_t first = std::string::npos;
            in_body = false;
            for (std::size_t i = 0; i < lines.size(); ++i) {
                const auto& l = lines[i];
                if (l.starts_with("define ")) {
                    in_body = true;
                    continue;
                }
                if (l == "}") {
                    in_body = false;
                    continue;
                }
                if (!in_body || l.size() < 3 || l[0] != ' ' || l.find(" = phi ") != std::string::npos) continue;
                auto ref = dbg_ref(l);
                if (ref.empty()) continue;
                auto it = locs.find(ref);
                if (it == locs.end() || it->second.first != f.line) continue;
                if (first == std::string::npos) first = i;
                int col = it->second.second;
                if (col <= f.col && col > best_col) {
                    best_col = col;
                    best = i;
                }
            }
            if (best == std::string::npos) best = first;
            if (best != std::string::npos) {
                markers[best].push_back(static_cast<int>(k));
                placed.push_back(static_cast<int>(k));
            }
        }
    }
    std::ostringstream out;
    std::set<unsigned> uninit_w, poison_w;
    bool uninit_ptr = false;
    bool in_body = false;
    int uniq = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto l = lines[i];
        if (l.starts_with("define ")) {
            in_body = true;
            out << l << "\n";
            continue;
        }
        if (l == "}") {
            in_body = false;
            out << l << "\n";
            continue;
        }
        if (!in_body || l.empty() || l[0] != ' ') {
            out << l << "\n";
            continue;
        }
        const auto sfx = dbg_suffix(l);
        if (auto it = markers.find(i); it != markers.end())
            for (int k : it->second) out << "  call void @__prism.folded(i32 " << k << ")" << sfx << "\n";
        // poison operands
        if (l.find(" = phi ") == std::string::npos) {
            std::size_t pos = 0;
            while ((pos = l.find(" poison", pos)) != std::string::npos) {
                auto end = pos + 7;
                if (end < l.size() && (std::isalnum(static_cast<unsigned char>(l[end])) || l[end] == '_')) {
                    pos = end;
                    continue;
                }
                // preceding "iN"
                auto t = pos;
                std::size_t s = t;
                while (s > 0 && std::isdigit(static_cast<unsigned char>(l[s - 1]))) --s;
                if (s == t || s < 1 || l[s - 1] != 'i' || (s >= 2 && l[s - 2] != ' ' && l[s - 2] != '(' && l[s - 2] != ',')) {
                    pos = end;
                    continue;
                }
                unsigned w = static_cast<unsigned>(std::stoul(l.substr(s, t - s)));
                if (w == 0 || w > 64) {
                    pos = end;
                    continue;
                }
                std::string name = "%__prism_poison." + std::to_string(uniq++);
                out << "  " << name << " = call i" << w << " @__prism.poison.i" << w << "()" << sfx << "\n";
                poison_w.insert(w);
                l = l.substr(0, pos + 1) + name + l.substr(end);
                pos = pos + 1 + name.size();
            }
        }
        out << l << "\n";
        // pointer local: "  %p = alloca ptr, align 8" -> @__prism.uninit.ptr()
        if (auto ap = l.find(" = alloca ptr"); ap != std::string::npos) {
            auto nm = l.substr(0, ap);
            nm.erase(0, nm.find_first_not_of(' '));
            auto tail = l.substr(ap + 13);
            if ((tail.empty() || tail.starts_with(", align ")) && tail.find(", i") == std::string::npos &&
                nm != "%retval") {
                std::string v = "%__prism_uninit." + std::to_string(uniq++);
                out << "  " << v << " = call ptr @__prism.uninit.ptr()\n";
                out << "  store ptr " << v << ", ptr " << nm << "\n";
                uninit_ptr = true;
            }
        }
        // promotable scalar local: "  %x = alloca iN, align A"
        auto a = l.find(" = alloca i");
        if (a != std::string::npos) {
            auto nm = l.substr(0, a);
            nm.erase(0, nm.find_first_not_of(' '));
            auto rest = l.substr(a + 10);  // "iN, align A"
            std::size_t d = 1;
            while (d < rest.size() && std::isdigit(static_cast<unsigned char>(rest[d]))) ++d;
            auto tail = rest.substr(d);
            bool simple = d > 1 && (tail.empty() || tail.starts_with(", align "));
            if (simple && tail.find(", i") == std::string::npos && nm != "%retval") {
                unsigned w = static_cast<unsigned>(std::stoul(rest.substr(1, d - 1)));
                if (w > 0 && w <= 64) {
                    std::string v = "%__prism_uninit." + std::to_string(uniq++);
                    out << "  " << v << " = call i" << w << " @__prism.uninit.i" << w << "()\n";
                    out << "  store i" << w << " " << v << ", ptr " << nm << "\n";
                    uninit_w.insert(w);
                }
            }
        }
    }
    for (auto w : uninit_w) out << "declare i" << w << " @__prism.uninit.i" << w << "()\n";
    if (uninit_ptr) out << "declare ptr @__prism.uninit.ptr()\n";
    for (auto w : poison_w) out << "declare i" << w << " @__prism.poison.i" << w << "()\n";
    if (!markers.empty()) out << "declare void @__prism.folded(i32)\n";
    return out.str();
}

std::vector<std::pair<int, int>> parse_ubsan_locs(const std::string& ir) {
    std::vector<std::pair<int, int>> out;
    std::size_t pos = 0;
    while ((pos = ir.find("{ ptr @", pos)) != std::string::npos) {
        auto end = ir.find('}', pos);
        if (end == std::string::npos) break;
        auto body = ir.substr(pos, end - pos);
        pos = end;
        auto a = body.find(", i32 ");
        if (a == std::string::npos) continue;
        auto b = body.find(", i32 ", a + 6);
        if (b == std::string::npos) continue;
        try {
            out.emplace_back(std::stoi(body.substr(a + 6)), std::stoi(body.substr(b + 6)));
        } catch (...) {
        }
    }
    return out;
}

std::vector<std::string> base_flags(const fs::path& src) {
    std::vector<std::string> f{"-S", "-emit-llvm", "-O0", "-Xclang", "-disable-O0-optnone",
                               "-fno-discard-value-names", "-gline-tables-only"};
    f.push_back(is_cxx(src) ? "-std=c++23" : "-std=c17");
    // Keep the UB-folding diagnostics on (they are how folded UB is found).
    for (auto* w : {"-Wshift-sign-overflow", "-Winteger-overflow", "-Wshift-overflow",
                    "-Wshift-negative-value", "-Wshift-count-overflow", "-Wshift-count-negative",
                    "-Wdivision-by-zero"})
        f.push_back(w);
    // Front-end leniency, not a disabled check: a C call to an undeclared
    // function compiles (C89 rule) instead of failing the whole unit. The
    // call itself is an unknown external and becomes "UNENCODED: call @f".
    if (!is_cxx(src)) f.push_back("-Wno-error=implicit-function-declaration");
    // Turns ON the C++ library's precondition checks (operator[] bounds,
    // optional/unique_ptr dereference, ...): PIR reports a reachable
    // __glibcxx_assert_fail as a violation (docs/PIR.md "C++ library").
    if (is_cxx(src)) f.push_back("-D_GLIBCXX_ASSERTIONS");
    return f;
}

}  // namespace

Frontend find_frontend(const Config& cfg) {
    Frontend fe;
    fe.clang = tool(cfg, {"clang-18", "clang"});
    fe.clangxx = tool(cfg, {"clang++-18", "clang++"});
    fe.opt = tool(cfg, {"opt-18", "opt"});
    fe.lli = tool(cfg, {"lli-18", "lli"});
    fe.version = "clang";
    if (fe.clang) {
        auto r = detail::run_process({fe.clang->string(), "--version"}, 20.0);
        auto p = r.text.find("clang version ");
        if (p != std::string::npos) {
            auto s = p + 14;
            auto e = s;
            while (e < r.text.size() && std::isdigit(static_cast<unsigned char>(r.text[e]))) ++e;
            if (e > s) fe.version = "clang-" + r.text.substr(s, e - s);
        }
    }
    return fe;
}

std::optional<std::string> lower_to_ir(const Frontend& fe, const fs::path& src, double timeout_s,
                                       std::string& err, std::vector<FoldedUb>* folded,
                                       std::vector<std::pair<int, int>>* signed_shl, std::string* cxx_models) {
    const bool cxx = is_cxx(src);
    const auto& cc = cxx ? fe.clangxx : fe.clang;
    if (!cc || !fe.opt) {
        err = std::string(cxx ? "clang++" : "clang") + "/opt not found";
        return std::nullopt;
    }
    TmpDir td;
    auto o0 = td.path / "o0.ll", o1 = td.path / "o1.ll", o2 = td.path / "o2.ll";
    std::vector<std::string> argv{cc->string()};
    auto fl = base_flags(src);
    if (cxx_models) cxx_models->clear();
    // C++ library models (docs/PIR.md "C++ library models"): the model
    // headers come first on the include path. A unit that does not compile
    // against them (an API or specialisation the models leave out, e.g.
    // vector<bool>) is lowered again with the platform library and says so.
    std::optional<detail::ProcOut> modelled;
    if (cxx && !fe.cxx_models.empty()) {
        auto mf = fl;
        mf.insert(mf.end(), {"-isystem", fe.cxx_models.string()});
        std::vector<std::string> av{cc->string()};
        av.insert(av.end(), mf.begin(), mf.end());
        av.insert(av.end(), {"-o", o0.string(), src.string()});
        auto rm = detail::run_process(av, timeout_s);
        if (!rm.failed && !rm.timed_out && rm.rc == 0) {
            // which model headers the unit's code actually uses (their DIFiles)
            const auto ir0 = read_file(o0);
            std::string used;
            for (auto& [name, text] : model_sources())
                if (name.starts_with("cxx/") && ir0.find((fe.cxx_models / name.substr(4)).string()) != std::string::npos)
                    used += (used.empty() ? "" : ", ") + name.substr(4);
            if (cxx_models && !used.empty()) *cxx_models = "model: " + used;
            modelled = std::move(rm);
        } else if (rm.timed_out) {
            err = "clang timed out";
            return std::nullopt;
        } else if (cxx_models) {
            *cxx_models = "libstdc++ (fallback: the unit does not compile against the PRISM models: " +
                          first_error(rm.text) + ")";
        }
    }
    argv.insert(argv.end(), fl.begin(), fl.end());
    argv.insert(argv.end(), {"-o", o0.string(), src.string()});
    auto r = modelled ? std::move(*modelled) : detail::run_process(argv, timeout_s);
    if (!cxx && !r.timed_out && (r.failed || r.rc != 0)) {
        // C23 code (bool, nullptr, typeof, digit separators ...) does not
        // compile as C17: try once more as C23 before giving up. Only the
        // language level changes; every check flag stays.
        auto c23 = fl;
        std::replace(c23.begin(), c23.end(), std::string("-std=c17"), std::string("-std=c23"));
        std::vector<std::string> av{cc->string()};
        av.insert(av.end(), c23.begin(), c23.end());
        av.insert(av.end(), {"-o", o0.string(), src.string()});
        auto r2 = detail::run_process(av, timeout_s);
        if (!r2.failed && !r2.timed_out && r2.rc == 0) {
            r = std::move(r2);
            fl = std::move(c23);
        }
    }
    if (r.failed || r.timed_out || r.rc != 0) {
        err = r.timed_out ? "clang timed out" : "clang: " + first_error(r.text);
        return std::nullopt;
    }
    auto diags = parse_folded(r.text, src);
    std::vector<int> placed;
    auto text = pirctl::keep_setjmp_locals(pirctl::uninit_fp_locals(instrument(read_file(o0), diags, placed)));
    if (folded) {
        // index = marker id; unplaced diagnostics are kept with line < 0
        folded->clear();
        for (std::size_t k = 0; k < diags.size(); ++k) {
            auto d = diags[k];
            if (std::find(placed.begin(), placed.end(), static_cast<int>(k)) == placed.end()) d.line = -d.line;
            folded->push_back(d);
        }
    }
    if (!write_file(o1, text)) {
        err = "cannot write temporary IR";
        return std::nullopt;
    }
    // C++: coroutines are lowered to state machines by LLVM's coroutine
    // passes (roadmap 2.3), then encoded like any other code (docs/PIR.md "Coroutines");
    // fix-irreducible gives their resume functions (loops re-entered at a
    // suspend point) a single loop header.
    const char* passes = cxx ? "-passes=function(mem2reg),coro-early,cgscc(coro-split),coro-cleanup,"
                               "function(lowerswitch,fix-irreducible,loop-simplify,lcssa,instnamer)"
                             : "-passes=mem2reg,lowerswitch,loop-simplify,lcssa,instnamer";
    auto ro = detail::run_process({fe.opt->string(), passes,
                                   "-S", "-o", o2.string(), o1.string()},
                                  timeout_s);
    if (ro.failed || ro.timed_out || ro.rc != 0) {
        err = ro.timed_out ? "opt timed out" : "opt: " + first_error(ro.text);
        return std::nullopt;
    }
    auto out = read_file(o2);
    if (signed_shl) {
        signed_shl->clear();
        // C only: C++20 and later define signed left shift (no base UB).
        if (!cxx && out.find(" = shl ") != std::string::npos) {
            std::vector<std::string> av{cc->string()};
            av.insert(av.end(), fl.begin(), fl.end());
            auto os = td.path / "shift.ll";
            av.insert(av.end(), {"-fsanitize=shift-base", "-o", os.string(), src.string()});
            auto rs = detail::run_process(av, timeout_s);
            if (rs.failed || rs.timed_out || rs.rc != 0) {
                err = "clang (signed-shift locator): " + first_error(rs.text);
                return std::nullopt;
            }
            *signed_shl = parse_ubsan_locs(read_file(os));
        }
    }
    return out;
}

namespace {

std::string rel_of(const fs::path& p, const fs::path& root) {
    std::error_code ec;
    if (fs::is_directory(root, ec)) {
        auto r = fs::relative(p, root, ec);
        if (!ec) return r.generic_string();
    }
    return p.filename().string();
}

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

std::vector<std::vector<uint64_t>> tv_inputs(const Function& fn, const std::vector<uint64_t>* cex) {
    std::vector<std::vector<uint64_t>> out;
    if (cex && cex->size() == fn.params.size()) out.push_back(*cex);
    std::vector<unsigned> ws;
    for (int p : fn.params) ws.push_back(fn.vars[static_cast<std::size_t>(p)].width);
    auto edges = [](unsigned w) {
        uint64_t m = w >= 64 ? ~uint64_t{0} : ((uint64_t{1} << w) - 1);
        uint64_t smin = w ? (uint64_t{1} << (w - 1)) : 0;
        return std::vector<uint64_t>{smin, m /* -1 */, 0, 1, (smin - 1) & m /* INT_MAX */};
    };
    for (std::size_t e = 0; e < 5; ++e) {
        std::vector<uint64_t> v;
        for (auto w : ws) v.push_back(edges(w)[e]);
        out.push_back(v);
    }
    uint64_t seed = 0;
    for (char c : fn.ir_name) seed = seed * 131 + static_cast<unsigned char>(c);
    // floating-point parameters: ordinary values too, not only bit patterns
    static const double kFp[] = {0.0,   -0.0,   1.0,    -1.0,          0.5,   2.5,    -7.75,  100.0,   -1000.25,
                                 3e9,   -3e9,   1e300,  16777217.0,    1e-310, 65504.0, 1e10,  INFINITY, NAN};
    while (static_cast<int>(out.size()) < kTvVectors) {
        std::vector<uint64_t> v;
        for (std::size_t k = 0; k < ws.size(); ++k) {
            auto w = ws[k];
            uint64_t m = w >= 64 ? ~uint64_t{0} : ((uint64_t{1} << w) - 1);
            auto r = splitmix(seed);
            if (fn.vars[static_cast<std::size_t>(fn.params[k])].fp && r % 3 != 0) {
                v.push_back(fp::from_double(kFp[(r >> 8) % std::size(kFp)], w) & m);
                continue;
            }
            auto pick = r % 4;
            if (pick == 0) v.push_back(edges(w)[(r >> 8) % 5]);
            else if (pick == 1) v.push_back(((r >> 8) % 33) & m);                     // small
            else if (pick == 2) v.push_back((0 - ((r >> 8) % 33)) & m);               // small negative
            else v.push_back(splitmix(seed) & m);
        }
        out.push_back(v);
    }
    return out;
}

std::string ll_const(uint64_t v, unsigned w) {
    if (w == 1) return (v & 1) ? "true" : "false";
    uint64_t m = w >= 64 ? ~uint64_t{0} : ((uint64_t{1} << w) - 1);
    v &= m;
    int64_t s = w >= 64 ? static_cast<int64_t>(v)
                        : static_cast<int64_t>((v ^ (uint64_t{1} << (w - 1))) - (uint64_t{1} << (w - 1)));
    return std::to_string(s);
}

struct Unit {
    fs::path path;
    std::string rel;
};

struct FnRec {
    Finding f;
    std::optional<Function> fn;
    std::vector<uint64_t> cex;
    const ir::Function* irf = nullptr;
};

// Replace the __prism.* declarations with trivial definitions so lli can run
// the instrumented module. The markers only matter to PIR.
std::string tv_module_text(const std::string& ir) {
    std::ostringstream out;
    for (auto& l : split_lines(ir)) {
        if (l.starts_with("declare ") && l.find("@__prism.") != std::string::npos) {
            auto at = l.find("@__prism.");
            auto paren = l.find('(', at);
            auto name = l.substr(at, paren - at);
            auto ty = l.substr(8, at - 8);  // "i32 " / "void "
            while (!ty.empty() && ty.back() == ' ') ty.pop_back();
            // strip attributes before the type (e.g. "noundef")
            auto sp = ty.rfind(' ');
            if (sp != std::string::npos) ty = ty.substr(sp + 1);
            if (ty == "void") out << "define void " << name << (name == "@__prism.keep" ? "(ptr %p)" : "(i32 %k)") << " {\n  ret void\n}\n";
            else out << "define " << ty << " " << name << "() {\n  ret " << ty << (ty == "ptr" ? " null" : ty == "half" || ty == "float" || ty == "double" ? " 0.0" : " 0")
                     << "\n}\n";
            continue;
        }
        out << l << "\n";
    }
    return out.str();
}

// Translation validation for one unit (roadmap 2.4). Adds extra["tv"] to
// each record; a divergence turns the verdict into ERROR.
void validate(std::vector<FnRec>& recs, const std::string& ir, const Frontend& fe, const Config& cfg) {
    struct Case {
        std::size_t rec;
        std::size_t idx;
        uint64_t expect;
        unsigned w;
        std::vector<uint64_t> args;
    };
    std::vector<Case> cases;
    std::vector<std::size_t> todo;
    for (std::size_t i = 0; i < recs.size(); ++i) {
        auto& r = recs[i];
        if (!r.fn) continue;
        const auto& st = r.f.status;
        if (!(st == laws::FAILED || st == laws::PROVED || st == laws::PROVED_CERTIFIED ||
              st == laws::PROVED_UNBOUNDED || st == laws::BOUNDED || st == laws::PROVED_ASSUMING))
            continue;
        if (!cfg.allow_exec) {
            r.f.extra["tv"] = "NOTRUN (needs --allow-exec)";
            r.f.extra["exec"] = std::string(laws::NOTRUN);
            continue;
        }
        if (!fe.lli || !fe.opt) {
            r.f.extra["tv"] = "NOTRUN (lli not found; " + std::string(kInstall) + ")";
            continue;
        }
        if (r.fn->nondet) {
            r.f.extra["tv"] = "PARTIAL (nondet inputs are not replayed; not validated)";
            continue;
        }
        if (r.irf && r.irf->params.size() != r.fn->params.size()) {
            r.f.extra["tv"] = "PARTIAL (sret/byval object parameters are not replayed; not validated)";
            continue;
        }
        if (auto why = pirmem::tv_exclusion(*r.fn); !why.empty()) {
            r.f.extra["tv"] = "PARTIAL (" + why + "; not validated)";
            continue;
        }
        auto ins = tv_inputs(*r.fn, r.cex.empty() ? nullptr : &r.cex);
        int kept = 0;
        for (std::size_t k = 0; k < ins.size(); ++k) {
            auto res = interpret(*r.fn, ins[k]);
            if (res.status != InterpResult::Returned || res.ret_nondet) continue;
            cases.push_back(Case{i, k, res.ret, r.fn->ret_width, ins[k]});
            ++kept;
        }
        if (kept == 0) {
            r.f.extra["tv"] = "NONE (no UB-free terminating input among " + std::to_string(ins.size()) + ")";
            continue;
        }
        r.f.extra["tv_inputs"] = std::to_string(kept) + "/" + std::to_string(ins.size());
        todo.push_back(i);
    }
    if (cases.empty()) return;
    auto fail_all = [&](const std::string& why) {
        for (auto i : todo) recs[i].f.extra["tv"] = "NOTRUN (" + why + ")";
    };
    if (ir.find("@printf(") != std::string::npos && ir.find("define") != std::string::npos &&
        ir.find("define i32 @printf(") != std::string::npos) {
        fail_all("unit defines printf");
        return;
    }
    std::ostringstream drv;
    drv << tv_module_text(ir);
    drv << "@__prism_tv_fmt = private unnamed_addr constant [14 x i8] c\"R %d %d %llu\\0A\\00\"\n";
    if (ir.find("@printf(") == std::string::npos) drv << "declare i32 @printf(ptr, ...)\n";
    if (ir.find("@fflush(") == std::string::npos) drv << "declare i32 @fflush(ptr)\n";
    drv << "define i32 @__prism_tv_main() {\nentry:\n";
    int t = 0;
    for (auto& c : cases) {
        auto& r = recs[c.rec];
        const auto* irf = r.irf;
        std::string args;
        for (std::size_t k = 0; k < c.args.size(); ++k) {
            auto w = r.fn->vars[static_cast<std::size_t>(r.fn->params[k])].width;
            if (k) args += ", ";
            if (irf->params[k].ty.kind == ir::Type::Float) {  // IEEE bits as an LLVM float literal
                args += irf->params[k].ty.text + " " + fp::ll_const(c.args[k], w);
                continue;
            }
            args += "i" + std::to_string(w);
            auto& at = irf->params[k].attrs;
            if (at.find("signext") != std::string::npos) args += " signext";
            if (at.find("zeroext") != std::string::npos) args += " zeroext";
            args += " " + ll_const(c.args[k], w);
        }
        std::string callee = "@\"" + irf->name + "\"";
        if (c.w && irf->ret.kind == ir::Type::Float) {
            drv << "  %q" << t << " = call " << irf->ret.text << " " << callee << "(" << args << ")\n";
            drv << "  %r" << t << " = bitcast " << irf->ret.text << " %q" << t << " to i" << c.w << "\n";
        } else if (c.w) {
            drv << "  %r" << t << " = call i" << c.w << " " << callee << "(" << args << ")\n";
        }
        if (c.w) {
            if (c.w < 64) drv << "  %z" << t << " = zext i" << c.w << " %r" << t << " to i64\n";
            else drv << "  %z" << t << " = add i64 %r" << t << ", 0\n";
        } else {
            drv << "  call void " << callee << "(" << args << ")\n";
            drv << "  %z" << t << " = add i64 0, 0\n";
        }
        drv << "  %p" << t << " = call i32 (ptr, ...) @printf(ptr @__prism_tv_fmt, i32 " << c.rec << ", i32 "
            << c.idx << ", i64 %z" << t << ")\n";
        drv << "  %f" << t << " = call i32 @fflush(ptr null)\n";
        ++t;
    }
    drv << "  ret i32 0\n}\n";
    TmpDir td;
    auto tv = td.path / "tv.ll", tv2 = td.path / "tv2.ll";
    if (!write_file(tv, drv.str())) {
        fail_all("cannot write driver");
        return;
    }
    // Compile-only reduction: keep the driver and what it reaches.
    auto ro = detail::run_process({fe.opt->string(), "-passes=internalize,globaldce",
                                   "-internalize-public-api-list=__prism_tv_main", "-S", "-o", tv2.string(),
                                   tv.string()},
                                  cfg.timeout);
    if (ro.failed || ro.timed_out || ro.rc != 0) {
        fail_all("driver link: " + first_error(ro.text));
        return;
    }
    std::vector<std::string> argv{fe.lli->string(), "--entry-function=__prism_tv_main", tv2.string()};
    if (auto pl = Config{}.which({"prlimit"})) {
        // rlimits for the JIT process (sandbox.hpp numbers), on top of bwrap
        argv.insert(argv.begin(), {pl->string(),
                                   "--as=" + std::to_string(sandbox::LIMIT_AS_BYTES),
                                   "--nofile=" + std::to_string(sandbox::LIMIT_NOFILE),
                                   "--fsize=" + std::to_string(sandbox::LIMIT_FSIZE_BYTES),
                                   "--cpu=" + std::to_string(static_cast<long>(cfg.timeout) + 2), "--"});
    }
    auto rr = detail::run_process(sandbox::wrap_argv(argv, td.path), cfg.timeout + 5.0, td.path);
    std::map<std::pair<std::size_t, std::size_t>, uint64_t> got;
    for (auto& line : split_lines(rr.text)) {
        if (!line.starts_with("R ")) continue;
        std::istringstream ls(line.substr(2));
        std::size_t a = 0, b = 0;
        unsigned long long v = 0;
        if (ls >> a >> b >> v) got[{a, b}] = v;
    }
    if (got.empty() && (rr.failed || rr.timed_out || rr.rc != 0)) {
        fail_all(rr.timed_out ? "lli timed out" : "lli: " + first_error(rr.text));
        return;
    }
    std::map<std::size_t, std::string> diverged;
    std::map<std::size_t, int> agreed;
    for (auto& c : cases) {
        auto& r = recs[c.rec];
        if (diverged.count(c.rec)) continue;
        auto it = got.find({c.rec, c.idx});
        uint64_t m = c.w >= 64 ? ~uint64_t{0} : c.w ? ((uint64_t{1} << c.w) - 1) : 0;
        auto args = format_cex(*r.fn, c.args);
        if (it == got.end()) {
            diverged[c.rec] = "translation validation diverged: " + r.fn->name + "(" + args +
                              ") returns under PIR but lli produced no result (rc=" + std::to_string(rr.rc) +
                              (rr.timed_out ? ", timeout" : "") + ")";
            continue;
        }
        const bool fp_ret = r.irf->ret.kind == ir::Type::Float;
        if ((it->second & m) != (c.expect & m) &&
            !(fp_ret && fp::is_nan(it->second & m, c.w) && fp::is_nan(c.expect & m, c.w))) {  // NaN payloads are unspecified
            diverged[c.rec] = "translation validation diverged: " + r.fn->name + "(" + args + ") PIR=" +
                              ll_const(c.expect, c.w ? c.w : 1) + " lli=" + ll_const(it->second, c.w ? c.w : 1);
            continue;
        }
        ++agreed[c.rec];
    }
    for (auto i : todo) {
        auto& f = recs[i].f;
        f.extra["sandbox"] = sandbox::kind();
        if (auto d = diverged.find(i); d != diverged.end()) {
            f.extra["verdict_before_tv"] = f.status;
            f.extra.erase(std::string(laws::CERTIFICATE_KEY));  // the certificate was for a wrong VC
            f.status = std::string(laws::ERROR);
            f.message = d->second;
            f.extra["tv"] = "DIVERGED";
            continue;
        }
        f.extra["tv"] = "PASS (" + std::to_string(agreed[i]) + " inputs agree with lli)";
    }
}

// Source text from a function's first line up to the next function's first line.
std::string span_text(const std::vector<std::string>& lines, const std::vector<int>& starts, int line) {
    if (line <= 0) return {};
    int end = static_cast<int>(lines.size());
    for (int s : starts)
        if (s > line) {
            end = s - 1;
            break;
        }
    std::string out;
    for (int i = line; i <= end && i <= static_cast<int>(lines.size()); ++i) out += lines[static_cast<std::size_t>(i - 1)] + "\n";
    return out;
}

// Solver settings of the run: the portfolio and the query cache always
// (docs/SOLVERS.md), a certificate per VC with --certified. The members of
// one query share the cores left to this worker.
CheckOptions check_options(const Config& cfg) {
    CheckOptions o;
    o.unwind = cfg.unwind;
    o.timeout_s = cfg.timeout;
    o.certified = cfg.certified;
    // Certification budget per PROVED function (docs/PIR.md "Solving"):
    // $PRISM_CERTIFY_BUDGET seconds (0: none), else max(240 s, 8 x
    // timeout), the sum of one query's certificate and checker budgets, so
    // a single certificate is never cut shorter than before while a chain of
    // per-VC certificates cannot run away with the run's time.
    o.certify_budget_s = std::max(240.0, 8.0 * cfg.timeout);
    if (const char* e = std::getenv("PRISM_CERTIFY_BUDGET"); e && *e) {
        char* end = nullptr;
        const double b = std::strtod(e, &end);
        if (end && *end == '\0' && b >= 0) o.certify_budget_s = b;
    }
    o.cache_dir = cfg.solver_cache.string();
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    o.max_parallel = std::max(2u, hw / static_cast<unsigned>(std::max(1, cfg.jobs)));
    return o;
}

Finding base_finding(const Unit& u) {
    Finding f;
    f.stage = "pir";
    f.file = u.rel;
    f.strength = std::string(laws::STRENGTH_PROVES);
    return f;
}

// Phase 1 (processes only): clang + opt. Phase 2 (Z3 only): translate and
// check. Phase 3 (processes only): translation validation. Keeping process
// spawns and Z3 solving in separate phases matters: Z3's scoped_timer
// registers a pthread_atfork handler that waits for in-flight solver timers,
// and fork() racing live solver threads on other workers can livelock.
struct Lowered {
    std::optional<std::string> ir;
    std::string err;
    std::vector<FoldedUb> folded;
    std::vector<std::pair<int, int>> sshl;
    std::string cxx_models;  // C++ library used (lower_to_ir), "" for C or no modelled header
};

struct Analyzed {
    std::vector<FnRec> recs;
    std::unique_ptr<ir::Module> mod;  // FnRec::irf points into it
    std::vector<Finding> out;         // unit-level rows (front-end failure, no functions)
};

Analyzed run_unit(const Unit& u, const Frontend& fe, const Config& cfg, const Lowered& low,
                  const ModelLibrary& models) {
    Analyzed res;
    auto& out = res.out;
    auto& recs = res.recs;
    const auto& ir = low.ir;
    const auto& folded = low.folded;
    const auto& sshl = low.sshl;
    if (!ir) {
        auto f = base_finding(u);
        f.status = std::string(laws::ERROR);
        f.strength = std::string(laws::STRENGTH_SOME);
        f.message = "clang front end failed: " + low.err;
        f.extra["frontend"] = fe.version;
        out.push_back(std::move(f));
        return res;
    }
    res.mod = std::make_unique<ir::Module>(ir::parse_module(*ir));
    auto& mod = *res.mod;
    // library operational models for what the unit declares (docs/PIR.md "Library models")
    link_models(mod, models);
    TranslateOptions topt;
    topt.signed_shl = sshl;
    topt.folded = folded;
    std::set<int> used_folded;
    const auto unit_name = u.path.filename().string();
    std::vector<std::string> src_lines = split_lines(read_file(u.path));
    pirmem::UnitInfo uinfo{u.path, u.rel, is_cxx(u.path), src_lines, std::nullopt};
    std::vector<int> starts;  // DISubprogram lines of this unit, sorted
    if (is_cxx(u.path)) {
        for (auto& [ref, sp] : mod.subprograms)
            if (fs::path(sp.file).filename().string() == unit_name && sp.line > 0) starts.push_back(sp.line);
        std::sort(starts.begin(), starts.end());
    }
    for (auto& irf : mod.functions) {
        ir::DISub sub;
        if (auto it = mod.subprograms.find(irf.dbg); it != mod.subprograms.end()) sub = it->second;
        if (sub.artificial || irf.name.starts_with("__cxx_global_var_init") || irf.name.starts_with("_GLOBAL__") ||
            irf.is_model)
            continue;
        if (!sub.file.empty() && fs::path(sub.file).filename().string() != unit_name) continue;  // header code
        FnRec rec;
        rec.irf = &irf;
        auto& f = rec.f;
        f = base_finding(u);
        f.function = sub.name.empty() ? irf.name : sub.name;
        f.line = sub.line ? std::optional<int>(sub.line) : std::nullopt;
        f.extra["frontend"] = fe.version;
        if (!sub.linkage.empty()) f.extra["ir_name"] = irf.name;
        auto fopt = pirmem::function_options(topt, irf, uinfo, sub.line, cfg, sub.name.empty() ? irf.name : sub.name);
        auto tr = translate(mod, irf, fopt);
        export_lean_pair(cfg.out, unit_name, mod, irf, fopt, tr);
        for (int k : tr.folded_used) used_folded.insert(k);
        if (!tr.fn) {
            f.status = tr.status.empty() ? std::string(laws::NEEDS_HARNESS) : tr.status;
            f.strength = std::string(laws::STRENGTH_SOME);
            f.message = tr.reason;
            recs.push_back(std::move(rec));
            continue;
        }
        if (is_cxx(u.path)) {
            // A write through const_cast to a const object is UB that the IR
            // cannot show (clang folds reads of the const, mem2reg promotes the
            // store). Not a PIR property yet: refuse to prove.
            std::vector<std::string> names{irf.name};
            for (auto& n : tr.fn->inlined) names.push_back(n);
            bool hit = false;
            for (auto& n : names) {
                const auto* g = mod.find(n);
                if (!g) continue;
                auto it = mod.subprograms.find(g->dbg);
                if (it == mod.subprograms.end()) continue;
                if (span_text(src_lines, starts, it->second.line).find("const_cast") != std::string::npos) hit = true;
            }
            if (hit) {
                f.status = std::string(laws::NEEDS_HARNESS);
                f.strength = std::string(laws::STRENGTH_SOME);
                f.message = "UNENCODED: const_cast (writes to const objects are not modelled)";
                recs.push_back(std::move(rec));
                continue;
            }
        }
        auto& fn = *tr.fn;
        fn.name = *f.function;
        fn.file = u.rel;
        fn.line = sub.line;
        auto text = to_text(fn);
        f.extra["pir_hash"] = sha256_hex(text);
        if (!fn.inlined.empty()) {
            std::string s;
            for (auto& n : fn.inlined) s += (s.empty() ? "" : ",") + n;
            f.extra["inlined"] = s;
        }
        auto v = check_function(fn, check_options(cfg));
        pirmem::apply_memory_policy(f, v, fn, mod, irf, fopt, cfg);
        f.status = v.status;
        f.message = v.message;
        f.cls = v.cls;
        for (auto& [k, val] : v.extra) f.extra[k] = val;
        if (v.status == laws::NEEDS_HARNESS) f.strength = std::string(laws::STRENGTH_SOME);
        if (v.status == laws::FAILED) {
            rec.cex = v.cex_args;
            f.counterexample = format_cex(fn, v.cex_args);
            if (f.counterexample.empty()) f.counterexample = v.prop + "=sat";
            f.extra["cex"] = f.counterexample;
            f.extra["prop"] = v.prop;
            if (v.line) f.line = v.line;
        }
        rec.fn = std::move(fn);
        recs.push_back(std::move(rec));
    }
    // Folded UB that no instruction carries: the function cannot be proved.
    for (std::size_t k = 0; k < folded.size(); ++k) {
        if (folded[k].line > 0) continue;  // marker is in the IR: checked (or its function is not encoded)
        int line = -folded[k].line;
        FnRec* owner = nullptr;
        for (auto& r : recs)
            if (r.f.line && *r.f.line <= line && (!owner || *owner->f.line < *r.f.line)) owner = &r;
        if (!owner) continue;
        if (owner->f.status == laws::FAILED || owner->f.status == laws::NEEDS_HARNESS) continue;
        owner->f.extra["verdict_before_folded"] = owner->f.status;
        owner->f.extra.erase(std::string(laws::CERTIFICATE_KEY));
        owner->f.status = std::string(laws::NEEDS_HARNESS);
        owner->f.strength = std::string(laws::STRENGTH_SOME);
        owner->f.message = "UNENCODED: clang-folded UB at line " + std::to_string(line) +
                           " not attributable to an IR instruction (" + folded[k].msg + ")";
        owner->fn.reset();
    }
    if (recs.empty()) {
        auto f = base_finding(u);
        f.status = std::string(laws::NOFUNC);
        f.strength = std::string(laws::STRENGTH_SOME);
        f.message = "no function definitions in this unit";
        f.extra["frontend"] = fe.version;
        out.push_back(std::move(f));
    }
    return res;
}

}  // namespace

std::vector<Finding> run_pir(const std::vector<fs::path>& sources, const Config& cfg) {
    std::vector<Unit> units;
    for (auto& p : sources)
        if (is_unit(p)) units.push_back(Unit{p, rel_of(p, cfg.root)});
    if (!z3_available()) {
        Finding f;
        f.stage = "pir";
        f.status = std::string(laws::NOTRUN);
        f.message = "z3 not built";
        f.strength = std::string(laws::STRENGTH_PROVES);
        f.extra["install"] = "rebuild with -DPRISM_Z3=ON (vendored third_party/z3)";
        return {f};
    }
    auto fe = find_frontend(cfg);
    if (units.empty()) return {};
    if (!fe.clang || !fe.opt || (!fe.clangxx && std::any_of(units.begin(), units.end(), [](auto& u) {
            return is_cxx(u.path);
        }))) {
        Finding f;
        f.stage = "pir";
        f.status = std::string(laws::NOTRUN);
        f.message = std::string(!fe.clang ? "clang" : !fe.opt ? "opt" : "clang++") +
                    " not found: the Clang/LLVM front end cannot run";
        f.strength = std::string(laws::STRENGTH_PROVES);
        f.extra["install"] = kInstall;
        return {f};
    }
    // library models: lowered once per run (process phase)
    auto models = build_models(fe, std::max(10.0, cfg.timeout));
    // C++ library model headers (docs/PIR.md "C++ library models"): written
    // once per run; without them C++ units use the platform library, and
    // every C++ function says which one it was checked against.
    std::optional<TmpDir> cxx_dir;
    std::string cxx_models_error;
    if (std::any_of(units.begin(), units.end(), [](auto& u) { return is_cxx(u.path); })) {
        cxx_dir.emplace();
        if (write_cxx_models(cxx_dir->path / "prism_cxx"))
            fe.cxx_models = cxx_dir->path / "prism_cxx";
        else
            cxx_models_error = "libstdc++ (the PRISM C++ model headers could not be written)";
    }
    std::vector<Lowered> low(units.size());
    parallel_for(cfg.jobs, units, [&](std::size_t i, const Unit& u) {
        auto& l = low[i];
        try {
            l.ir = lower_to_ir(fe, u.path, std::max(10.0, cfg.timeout), l.err, &l.folded, &l.sshl, &l.cxx_models);
            if (is_cxx(u.path) && l.cxx_models.empty() && !cxx_models_error.empty()) l.cxx_models = cxx_models_error;
        } catch (const std::exception& ex) {
            l.ir.reset();
            l.err = std::string("internal error: ") + ex.what();
        }
    });
    std::vector<Analyzed> an(units.size());
    parallel_for(cfg.jobs, units, [&](std::size_t i, const Unit& u) {
        try {
            an[i] = run_unit(u, fe, cfg, low[i], models);
        } catch (const std::exception& ex) {
            auto f = base_finding(u);
            f.status = std::string(laws::ERROR);
            f.strength = std::string(laws::STRENGTH_SOME);
            f.message = std::string("pir internal error: ") + ex.what();
            an[i].recs.clear();
            an[i].out = {f};
        }
    });
    std::vector<std::vector<Finding>> per(units.size());
    parallel_for(cfg.jobs, units, [&](std::size_t i, const Unit&) {
        auto& a = an[i];
        if (low[i].ir && !a.recs.empty()) validate(a.recs, *low[i].ir, fe, cfg);
        per[i] = std::move(a.out);
        for (auto& r : a.recs) per[i].push_back(std::move(r.f));
        if (!low[i].cxx_models.empty())
            for (auto& f : per[i]) f.extra["cxx_models"] = low[i].cxx_models;
    });
    std::vector<Finding> out;
    if (!models.error.empty()) {
        // Law 7: without the models every library call stays UNENCODED; say why
        Finding f;
        f.stage = "pir";
        f.status = std::string(laws::ERROR);
        f.strength = std::string(laws::STRENGTH_SOME);
        f.message = "library models could not be built: " + models.error;
        out.push_back(std::move(f));
    }
    for (auto& v : per)
        for (auto& f : v) out.push_back(std::move(f));
    return exec_gate_note("pir", std::move(out), "pir translation validation (lli)");
}

}  // namespace prism::pir
