// The pir stage: Clang -> LLVM IR -> normalisation -> PIR -> Z3 (docs/PIR.md).
//
// Clang and opt only compile the scanned code; nothing from the scanned tree
// runs unless --allow-exec is given (Law 9). With it, every verdict is
// translation-validated: the PIR interpreter and `lli` (sandboxed) run the
// same concrete inputs and must agree wherever PIR reports no UB.

#include "prism/membudget.hpp"
#include "prism/abi_model.hpp"
#include "prism/astlint.hpp"
#include "prism/laws.hpp"
#include "prism/regex.hpp"
#include "prism/pipeline.hpp"
#include "prism/pir.hpp"
#include "prism/pir/folded_diag.hpp"
#include "prism/pir/frontend.hpp"
#include "prism/sandbox.hpp"
#include "prism/threads.hpp"

#include "../proc.hpp"
#include "fp.hpp"
#include "lower_ctl.hpp"
#include "stage_mem.hpp"

#include <cctype>
#include <cstring>
#include <algorithm>
#include <tuple>
#include <nlohmann/json.hpp>
#include <map>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <algorithm>
#include <functional>
#include <regex>
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
    if (!out.is_open()) return false;
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
        std::string line = s.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
        pos = nl + 1;
    }
    if (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

bool ir_line_closes_function(const std::string& l) {
    std::string t = l;
    while (!t.empty() && t.back() == '\r') t.pop_back();
    return t == "}";
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
    while (q < line.size() && std::isdigit(static_cast<unsigned char>(line.at(q)))) ++q;
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
            const auto& f = folded.at(k);
            std::size_t best = std::string::npos;
            int best_col = -1;
            std::size_t first = std::string::npos;
            in_body = false;
            for (std::size_t i = 0; i < lines.size(); ++i) {
                const auto& l = lines.at(i);
                if (l.starts_with("define ")) {
                    in_body = true;
                    continue;
                }
                if (ir_line_closes_function(l)) {
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
        auto l = lines.at(i);
        if (l.starts_with("define ")) {
            in_body = true;
            out << l << "\n";
            continue;
        }
        if (ir_line_closes_function(l)) {
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
        } catch (const std::exception&) {
            (void)0;
        }
    }
    return out;
}

// C23 (N3038) storage-class specifiers in compound literals:
// (static T){...}, (constexpr T){...}, (static constexpr T){...},
// (thread_local static T){...}, (register T){...}. clang (checked through
// 23.1.4) rejects them ("expected expression"). This rewrites each one into
// an equivalent clang accepts:
//   block scope:  (*({ static T __prism_clN = {...}; &__prism_clN; }))
//                 (a GNU statement expression; an lvalue of type T, arrays
//                 included, with static storage, as the literal has)
//   constexpr without static at block scope becomes static constexpr: the
//   value and constness are the same, only the object's address is shared
//   across calls (observable only by comparing addresses of two
//   evaluations, which C23 leaves unspecified for const literals anyway,
//   6.5.2.5p8). register is dropped (it only forbids taking the address).
//   file scope:   the specifier is dropped (file-scope literals already
//                 have static storage); constexpr becomes const.
// Returns the input unchanged when it has no such literal.
std::string c23_compound_literal_rewrite(const std::string& in, bool* changed) {
    std::string out;
    size_t i = 0, n = in.size();
    int depth = 0, counter = 0;
    if (changed) *changed = false;
    auto skip_ws = [&](size_t j) { while (j < n && std::isspace(static_cast<unsigned char>(in[j]))) ++j; return j; };
    auto word_at = [&](size_t j, const char* w) {
        size_t L = std::strlen(w);
        return in.compare(j, L, w) == 0 && (j + L >= n || !(std::isalnum(static_cast<unsigned char>(in[j + L])) || in[j + L] == '_'));
    };
    while (i < n) {
        char c = in[i];
        if (c == '/' && i + 1 < n && in[i + 1] == '/') { size_t e = in.find('\n', i); e = e == std::string::npos ? n : e; out.append(in, i, e - i); i = e; continue; }
        if (c == '/' && i + 1 < n && in[i + 1] == '*') { size_t e = in.find("*/", i + 2); e = e == std::string::npos ? n : e + 2; out.append(in, i, e - i); i = e; continue; }
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < n && in[j] != c) { if (in[j] == '\\') ++j; ++j; }
            j = std::min(n, j + 1);
            out.append(in, i, j - i); i = j; continue;
        }
        if (c == '{') ++depth;
        if (c == '}') --depth;
        if (c == '(') {
            size_t j = skip_ws(i + 1);
            bool st = false, cx = false, tl = false, any = false;
            for (;;) {
                if (word_at(j, "static")) { st = true; j = skip_ws(j + 6); }
                else if (word_at(j, "constexpr")) { cx = true; j = skip_ws(j + 9); }
                else if (word_at(j, "thread_local")) { tl = true; j = skip_ws(j + 12); }
                else if (word_at(j, "_Thread_local")) { tl = true; j = skip_ws(j + 13); }
                else if (word_at(j, "register")) { j = skip_ws(j + 8); }
                else break;
                any = true;
            }
            if (any) {
                // type name up to the matching ')'
                size_t k = j;
                int pd = 1;
                while (k < n && pd > 0) { if (in[k] == '(') ++pd; else if (in[k] == ')') --pd; if (pd > 0) ++k; }
                size_t b = k < n ? skip_ws(k + 1) : n;
                if (k < n && b < n && in[b] == '{') {
                    size_t e = b;
                    int bd = 0;
                    for (; e < n; ++e) {
                        if (in[e] == '"' || in[e] == '\'') { const char q = in[e]; for (++e; e < n && in[e] != q; ++e) if (in[e] == '\\') ++e; continue; }
                        if (in[e] == '/' && e + 1 < n && in[e + 1] == '*') { e = in.find("*/", e + 2); if (e == std::string::npos) e = n; else ++e; continue; }
                        if (in[e] == '/' && e + 1 < n && in[e + 1] == '/') { e = in.find('\n', e); if (e == std::string::npos) e = n; continue; }
                        if (in[e] == '{') ++bd; else if (in[e] == '}' && --bd == 0) break;
                    }
                    if (e < n) {
                        std::string type = in.substr(j, k - j);
                        std::string init = in.substr(b, e - b + 1);
                        std::string quals = std::string(tl ? "_Thread_local " : "") + (cx ? "constexpr " : "");
                        if (depth > 0) {
                            // declarator: an array type "int[3]" -> "int __n[3]"
                            std::string name = "__prism_cl" + std::to_string(counter++);
                            size_t br = type.find('[');
                            std::string decl = br == std::string::npos ? type + " " + name : type.substr(0, br) + " " + name + type.substr(br);
                            (void)st;
                            out += "(*({ static " + quals + decl + " = " + init + "; &" + name + "; }))";
                        } else {
                            out += "(" + std::string(tl ? "_Thread_local " : "") + (cx ? "const " : "") + type + ")" + init;
                        }
                        if (changed) *changed = true;
                        i = e + 1;
                        continue;
                    }
                }
            }
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

// C23 decimal floating point (models/dfp/prism_dfp.hpp explains the route).
// Rewrites decimal literal suffixes DF/DD/DL to the shim's user-defined
// literals and wraps the unit in extern "C" so its functions keep C names;
// #line keeps diagnostics and debug info on the original file. Text in
// string/char literals and comments is left alone.
std::string dfp_rewrite(const std::string& in, const fs::path& orig) {
    std::string out = "#line 1 \"" + orig.string() + "\"\nextern \"C\" {\n#line 1 \"" + orig.string() + "\"\n";
    size_t i = 0;
    const size_t n = in.size();
    auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    while (i < n) {
        char c = in[i];
        if (c == '/' && i + 1 < n && in[i + 1] == '/') { size_t e = in.find('\n', i); e = e == std::string::npos ? n : e; out.append(in, i, e - i); i = e; continue; }
        if (c == '/' && i + 1 < n && in[i + 1] == '*') { size_t e = in.find("*/", i + 2); e = e == std::string::npos ? n : e + 2; out.append(in, i, e - i); i = e; continue; }
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < n && in[j] != c) { if (in[j] == '\\') ++j; ++j; }
            j = std::min(n, j + 1);
            out.append(in, i, j - i); i = j; continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(in[i + 1])))) {
            if (i > 0 && ident(in[i - 1])) { out.push_back(c); ++i; continue; }
            size_t j = i;
            while (j < n && (ident(in[j]) || in[j] == '.' || in[j] == '\'' ||
                             ((in[j] == '+' || in[j] == '-') && (in[j - 1] == 'e' || in[j - 1] == 'E')))) ++j;
            std::string tok = in.substr(i, j - i);
            if (tok.size() > 2 && !(tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X'))) {
                std::string suf = tok.substr(tok.size() - 2);
                for (auto& ch : suf) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                if (suf == "DF" || suf == "DD" || suf == "DL") {
                    std::string lo = suf == "DF" ? "_prism_df" : suf == "DD" ? "_prism_dd" : "_prism_dl";
                    tok = tok.substr(0, tok.size() - 2) + lo;
                }
            }
            out += tok; i = j; continue;
        }
        if ((std::isalpha(static_cast<unsigned char>(c)) || c == '_') && !(i > 0 && ident(in[i - 1]))) {
            size_t j = i;
            while (j < n && ident(in[j])) ++j;
            std::string id = in.substr(i, j - i);
            size_t k = j;
            while (k < n && (in[k] == ' ' || in[k] == '\t')) ++k;
            static const std::set<std::string> kIo{"printf", "fprintf", "sprintf", "snprintf", "scanf", "fscanf", "sscanf", "vprintf", "vfprintf", "vsprintf", "vsnprintf", "vscanf", "vfscanf", "vsscanf"};
            // C++-only keywords are ordinary identifiers in C
            static const std::set<std::string> kKw{
                "class", "new", "delete", "this", "template", "typename", "namespace", "operator", "private",
                "public", "protected", "virtual", "friend", "using", "throw", "try", "catch", "explicit",
                "mutable", "export", "typeid", "reinterpret_cast", "static_cast", "dynamic_cast", "const_cast",
                "concept", "requires", "co_await", "co_yield", "co_return", "char8_t", "consteval", "constinit",
                "noexcept", "decltype", "final", "override", "import", "module"};
            if (kIo.count(id) && k < n && in[k] == '(') out += "__prism_dfp_" + id;
            else if (kKw.count(id) && id != "final" && id != "override" && id != "import" && id != "module") out += "__prism_c_" + id;
            else out += id;
            i = j;
            continue;
        }
        out.push_back(c); ++i;
    }
    out += "\n}\n";
    // #include lines leave the extern "C" block (a C++ header such as
    // libstdc++'s <math.h> wrapper declares templates, which need C++ linkage).
    std::string fixed;
    size_t line = 0, pos = 0;
    int body_line = -3;  // the first three lines are the prologue
    int depth = 0;
    std::string file_mode;  // active file-scope FENV_DEC_ROUND, "" = dynamic
    char last_top = 0;      // last non-space char seen at file scope
    bool in_bcomment = false, stmt_start = false, init_open = false, fn_body = false;
    while (pos < out.size()) {
        size_t e = out.find('\n', pos);
        e = e == std::string::npos ? out.size() : e + 1;
        std::string l = out.substr(pos, e - pos);
        pos = e;
        ++line;
        ++body_line;
        size_t f = l.find_first_not_of(" \t");
        const bool inc = body_line >= 1 && f != std::string::npos && l[f] == '#' &&
                         l.find_first_not_of(" \t", f + 1) != std::string::npos &&
                         l.compare(l.find_first_not_of(" \t", f + 1), 7, "include") == 0;
        // #pragma STDC FENV_DEC_ROUND <mode>: block scope -> scoped mode,
        // file scope -> static initialiser (braces counted per line; the
        // extern "C" wrapper itself is depth 1).
        const bool prag = f != std::string::npos && l.compare(f, 7, "#pragma") == 0 &&
                          l.find("STDC") != std::string::npos && l.find("FENV_DEC_ROUND") != std::string::npos;
        if (prag) {
            size_t m = l.find("FENV_DEC_ROUND") + 14;
            std::string mode = l.substr(m);
            while (!mode.empty() && std::isspace(static_cast<unsigned char>(mode.back()))) mode.pop_back();
            while (!mode.empty() && std::isspace(static_cast<unsigned char>(mode.front()))) mode.erase(0, 1);
            if (depth > 1) fixed += "__PRISM_DFP_ROUND_BLOCK(" + mode + ")\n";
            else { file_mode = mode == "FE_DEC_DYNAMIC" ? "" : mode; fixed += "\n"; }  // keep line numbers
            continue;
        }
        // C23 7.6.3: a file-scope FENV_DEC_ROUND is lexical, from the pragma
        // to the end of the unit (or the next file-scope one). Each function
        // body opened after it starts with a scoped mode, so code before the
        // pragma and functions called from elsewhere keep the dynamic mode.
        // A body is a depth-1 '{' whose preceding top-level text ends in ')'.
        // Braces are counted outside comments, string and character
        // literals (block comments span lines). After a file-scope pragma,
        // each top-level declaration is bracketed by FILE_INIT_BEGIN/END so
        // its initialiser runs in the pragma's mode (C23 7.6.3).
        {
            std::string nl;
            const bool pp = f != std::string::npos && l[f] == '#' && !in_bcomment;
            for (size_t ci = 0; ci < l.size(); ++ci) {
                const char ch = l[ci];
                if (in_bcomment) {
                    nl.push_back(ch);
                    if (ch == '*' && ci + 1 < l.size() && l[ci + 1] == '/') { nl.push_back('/'); ++ci; in_bcomment = false; }
                    continue;
                }
                if (pp) { nl.push_back(ch); continue; }
                if (ch == '/' && ci + 1 < l.size() && l[ci + 1] == '/') { nl.append(l, ci, std::string::npos); break; }
                if (ch == '/' && ci + 1 < l.size() && l[ci + 1] == '*') { nl += "/*"; ++ci; in_bcomment = true; continue; }
                if (depth == 1 && stmt_start && !file_mode.empty() && !std::isspace(static_cast<unsigned char>(ch)) && ch != '}') {
                    nl += "__PRISM_DFP_FILE_INIT_BEGIN(" + file_mode + ") ";
                    init_open = true;
                }
                if (depth == 1 && !std::isspace(static_cast<unsigned char>(ch))) stmt_start = false;
                if (ch == '"' || ch == '\'') {
                    size_t j = ci + 1;
                    while (j < l.size() && l[j] != ch && l[j] != '\n') { if (l[j] == '\\') ++j; ++j; }
                    j = std::min(l.size(), j + 1);
                    nl.append(l, ci, j - ci);
                    ci = j - 1;
                    last_top = ch;
                    continue;
                }
                nl.push_back(ch);
                if (ch == '{') {
                    if (depth == 1 && !file_mode.empty() && last_top == ')') {
                        nl += " __PRISM_DFP_ROUND_BLOCK(" + file_mode + ")";
                        fn_body = true;
                    }
                    if (depth == 0) stmt_start = true;  // the extern "C" wrapper
                    ++depth;
                } else if (ch == '}') {
                    --depth;
                    if (depth == 1 && fn_body) {
                        fn_body = false;
                        stmt_start = true;
                        if (init_open) { nl += " __PRISM_DFP_FILE_INIT_END"; init_open = false; }
                    }
                } else if (ch == ';' && depth == 1) {
                    stmt_start = true;
                    if (init_open) { nl += " __PRISM_DFP_FILE_INIT_END"; init_open = false; }
                }
                if (depth == 1 && !std::isspace(static_cast<unsigned char>(ch))) last_top = ch;
            }
            if (pp) stmt_start = true;
            l = nl;
        }
        if (inc)
            fixed += "}\n" + l + (l.ends_with("\n") ? "" : "\n") + "extern \"C\" {\n#line " +
                     std::to_string(body_line + 1) + " \"" + orig.string() + "\"\n";
        else
            fixed += l;
    }
    return fixed;
}

// C is not C++: the implicit conversions C allows (void* -> T*, int ->
// enum, T* -> unrelated T*) are errors in C++. clang names each one with
// the target type and the expression's source range, so the cast C performs
// implicitly is inserted explicitly: "((T)(expr))". Diagnostics are read
// from a probe copy whose #line directives are neutralised, so lines and
// columns are those of the text being edited. Up to 8 rounds.
std::string dfp_cxx_fixups(std::string text, const std::function<std::string(const std::string&)>& diagnose) {
    static const std::regex kDiag(
        R"([^:]*:(\d+):\d+:\{(\d+):(\d+)-(\d+):(\d+)\}.*: error: (?:cannot initialize .* of type '([^']+)' with an [lr]value of type '[^']+'|assigning to '([^']+)' from incompatible type '[^']+').*)");
    for (int round = 0; round < 8; ++round) {
        std::string probe;
        {
            std::istringstream is(text);
            std::string l;
            while (std::getline(is, l)) probe += (l.rfind("#line", 0) == 0 ? "// " + l : l) + "\n";
        }
        const std::string d = diagnose(probe);
        struct Edit { size_t at; std::string s; };
        std::vector<Edit> edits;
        std::vector<size_t> starts{0};
        for (size_t k = 0; k < text.size(); ++k) if (text[k] == '\n') starts.push_back(k + 1);
        auto off = [&](int line, int col) -> std::optional<size_t> {
            if (line < 1 || static_cast<size_t>(line) > starts.size()) return std::nullopt;
            return starts[line - 1] + static_cast<size_t>(col - 1);
        };
        std::istringstream ds(d);
        std::string dl;
        while (std::getline(ds, dl)) {
            std::smatch m;
            if (dl.find(".dfp.probe.cpp:") == std::string::npos || !std::regex_match(dl, m, kDiag)) continue;
            const std::string ty = m[6].matched ? m[6].str() : m[7].str();
            auto b = off(std::stoi(m[2]), std::stoi(m[3]));
            auto e = off(std::stoi(m[4]), std::stoi(m[5]));
            if (!b || !e || *e < *b || *e > text.size()) continue;
            edits.push_back({*e, "))"});
            edits.push_back({*b, "((" + ty + ")("});
        }
        if (edits.empty()) break;
        std::stable_sort(edits.begin(), edits.end(), [](const Edit& x, const Edit& y) { return x.at > y.at; });
        for (auto& ed : edits) text.insert(ed.at, ed.s);
    }
    return text;
}

std::vector<std::string> base_flags(const fs::path& src) {
    std::vector<std::string> f{"-S"};
    for (auto& t : lp64_clang_driver_flags()) f.push_back(t);
    f.insert(f.end(), {"-emit-llvm", "-O0", "-Xclang", "-disable-O0-optnone", "-fno-discard-value-names",
                       "-gline-tables-only"});
    f.push_back(is_cxx(src) ? "-std=c++23" : "-std=c17");
    if (!is_cxx(src)) {
        // C23 library declarations the libc may lack (memset_explicit)
        const fs::path forced = fs::path(__FILE__).parent_path() / "models" / "libc" / "forced" / "prism_c23_decls.h";
        std::error_code fec;
        if (fs::is_regular_file(forced, fec)) {
            f.push_back("-include");
            f.push_back(forced.string());
        }
    }
    // A C/C++ memcpy call stays a call to memcpy (its library model checks
    // C's rule: no overlap at all, C17 7.24.2.1) instead of becoming the
    // llvm.memcpy intrinsic, which the LangRef lets copy an object exactly
    // onto itself (struct assignment; refinement finding 6). Not a disabled
    // check: the call is checked by the model.
    f.push_back("-fno-builtin-memcpy");
    // Keep the UB-folding diagnostics on (they are how folded UB is found).
    for (auto* w : {"-Wshift-sign-overflow", "-Winteger-overflow", "-Wshift-overflow",
                    "-Wshift-negative-value", "-Wshift-count-overflow", "-Wshift-count-negative",
                    "-Wdivision-by-zero"})
        f.push_back(w);
    // Front-end leniency, not a disabled check: a C call to an undeclared
    // function compiles (C89 rule) instead of failing the whole unit. The
    // call itself is an unknown external and becomes "UNENCODED: call @f".
    if (!is_cxx(src)) f.push_back("-Wno-error=implicit-function-declaration");
    // Front-end limit, not a check: generated code (SV-COMP's reducer
    // tasks) nests parentheses deeper than clang's default of 256.
    f.push_back("-fbracket-depth=4096");
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


// Item 6: C with no C++ spelling (K&R definitions, reordered designated
// initialisers, [static N], VLA forms) cannot go through the C++ shim. The
// unit is split at file scope instead: function definitions that use a
// decimal type (directly or by calling one that does) go to a C++ file
// lowered against the shim; everything else stays C. Both sides see every
// type, prototype and object declaration; objects are defined only on the
// C side (the C++ side gets `extern` declarations) unless their type is
// decimal. Limits: a file-scope `static` non-decimal object used by both
// sides is duplicated (each side gets its own copy); a declaration with
// several initialised declarators keeps only the first declarator extern.
struct DfpSplit { std::string c, cxx; bool any = false; };
// K&R head "T f(a, b) int a; char *b;" -> "T f(int a, char *b)"
std::string knr_to_prototype(const std::string& head) {
    const size_t lp = head.find('('), rp = head.find(')', lp == std::string::npos ? 0 : lp);
    if (lp == std::string::npos || rp == std::string::npos) return head;
    std::vector<std::string> ids;
    {
        std::string cur;
        for (size_t k = lp + 1; k < rp; ++k) {
            if (head[k] == ',') { ids.push_back(cur); cur.clear(); }
            else if (!std::isspace(static_cast<unsigned char>(head[k]))) cur += head[k];
        }
        if (!cur.empty()) ids.push_back(cur);
    }
    std::map<std::string, std::string> decl;
    const std::string rest = head.substr(rp + 1);
    size_t a = 0;
    while (a < rest.size()) {
        size_t e = rest.find(';', a);
        if (e == std::string::npos) break;
        std::string d = rest.substr(a, e - a);
        a = e + 1;
        // base type = text before the first declarator: up to the last identifier token
        // preceding the first ',' / '*' / identifier list; split declarators at commas
        std::vector<std::string> parts;
        std::string cur; int pd = 0;
        for (char ch : d) { if (ch == '(' || ch == '[') ++pd; if (ch == ')' || ch == ']') --pd; if (ch == ',' && pd == 0) { parts.push_back(cur); cur.clear(); } else cur += ch; }
        parts.push_back(cur);
        static const std::regex kLastId(R"(([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*\)*\s*$)");
        std::smatch m0;
        if (!std::regex_search(parts[0], m0, kLastId)) continue;
        // base: parts[0] up to the start of its declarator (the leading '*'s and '(' before the id)
        size_t ds = static_cast<size_t>(m0.position(1));
        while (ds > 0 && (parts[0][ds - 1] == '*' || parts[0][ds - 1] == '(' || std::isspace(static_cast<unsigned char>(parts[0][ds - 1])))) --ds;
        const std::string base = parts[0].substr(0, ds);
        for (size_t k = 0; k < parts.size(); ++k) {
            std::string dcl = k == 0 ? parts[0].substr(ds) : parts[k];
            std::smatch m;
            if (std::regex_search(dcl, m, kLastId)) decl[m[1]] = base + " " + dcl;
        }
    }
    std::string out = head.substr(0, lp + 1);
    for (size_t k = 0; k < ids.size(); ++k) {
        if (k) out += ", ";
        auto it = decl.find(ids[k]);
        out += it != decl.end() ? it->second : "int " + ids[k];  // C89 implicit int
    }
    if (ids.empty()) out += "void";
    return out + ")";
}
// C-only declarator syntax in the C++ part: [static N], [const N], [*]
std::string c_only_to_cxx(std::string t) {
    t = std::regex_replace(t, std::regex(R"(\[\s*(?:(?:static|const|volatile|restrict)\s+)+)"), "[");
    t = std::regex_replace(t, std::regex(R"(\[\s*\*\s*\])"), "[]");
    return t;
}
// Token-level renaming of file-scope identifiers (coverage item 6): strings,
// character literals, comments, member accesses (. and ->), struct/union
// member declarations and #include lines are left alone.
std::string rename_tokens(const std::string& in, const std::map<std::string, std::string>& ren) {
    if (ren.empty()) return in;
    std::string out;
    out.reserve(in.size() + 64);
    const size_t n = in.size();
    std::vector<bool> member_scope{false};  // per brace depth: inside a struct/union body
    bool pending_aggr = false;               // saw struct/union, awaiting '{' or ';'
    char prev = 0, prev2 = 0;                // last two non-space characters emitted
    for (size_t i = 0; i < n;) {
        const char c = in[i];
        if (c == '/' && i + 1 < n && (in[i + 1] == '/' || in[i + 1] == '*')) {
            size_t e = in[i + 1] == '/' ? in.find('\n', i) : in.find("*/", i + 2);
            e = e == std::string::npos ? n : (in[i + 1] == '/' ? e : e + 2);
            out.append(in, i, e - i); i = e; continue;
        }
        if (c == '"' || c == '\'') {
            size_t e = i + 1;
            for (; e < n && in[e] != c; ++e) if (in[e] == '\\') ++e;
            e = std::min(n, e + 1);
            out.append(in, i, e - i); i = e; prev2 = prev; prev = c; continue;
        }
        if (c == '#') {  // #include <...> / "..." : copy the line verbatim
            size_t ls = in.rfind('\n', i == 0 ? 0 : i - 1);
            ls = (ls == std::string::npos || i == 0) ? 0 : ls + 1;
            if (in.find_first_not_of(" \t", ls) == i && in.compare(in.find_first_not_of(" \t", i + 1), 7, "include") == 0) {
                size_t e = in.find('\n', i); e = e == std::string::npos ? n : e;
                out.append(in, i, e - i); i = e; continue;
            }
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t e = i;
            while (e < n && (std::isalnum(static_cast<unsigned char>(in[e])) || in[e] == '_')) ++e;
            const std::string tok = in.substr(i, e - i);
            if (tok == "struct" || tok == "union") pending_aggr = true;
            auto it = ren.find(tok);
            const bool member = prev == '.' || (prev == '>' && prev2 == '-');
            if (it != ren.end() && !member && !member_scope.back()) out += it->second;
            else out += tok;
            prev2 = prev; prev = 'a'; i = e; continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {  // pp-number: 1e5f, 0x1p3 ...
            size_t e = i;
            while (e < n && (std::isalnum(static_cast<unsigned char>(in[e])) || in[e] == '_' || in[e] == '.' ||
                             ((in[e] == '+' || in[e] == '-') && std::strchr("eEpP", in[e - 1])))) ++e;
            out.append(in, i, e - i); i = e; prev2 = prev; prev = '0'; continue;
        }
        if (c == '{') { member_scope.push_back(pending_aggr); pending_aggr = false; }
        else if (c == '}') { if (member_scope.size() > 1) member_scope.pop_back(); }
        else if (c == ';' || c == ')' || c == '(' || c == '=' || c == ',') pending_aggr = false;
        out += c;
        if (!std::isspace(static_cast<unsigned char>(c))) { prev2 = prev; prev = c; }
        ++i;
    }
    return out;
}

// Real reordering of designated initialisers for the C++ part (C++20 wants
// declaration order; C allows any order). Field orders come from the
// struct/union definitions in the unit; a list is reordered by the unique
// field order that covers all its designators (lists that match none, or
// several orders that disagree, are left to clang's extension). Array
// designators [N] = v with integer-literal N become positional elements,
// gaps filled with {} (value-initialised, as C zero-initialises them).
std::vector<std::vector<std::string>> collect_field_orders(const std::string& src) {
    std::vector<std::vector<std::string>> r;
    static const std::regex kAggr(R"(\b(?:struct|union)\s*(?:\w+\s*)?\{)");
    for (auto it = std::sregex_iterator(src.begin(), src.end(), kAggr); it != std::sregex_iterator(); ++it) {
        size_t b = static_cast<size_t>(it->position() + it->length()), e = b;
        int d = 1;
        for (; e < src.size() && d; ++e) { if (src[e] == '{') ++d; else if (src[e] == '}') --d; }
        std::string body = src.substr(b, e - b - 1);
        // drop nested aggregate bodies (their members are not ours)
        std::string flat; int dd = 0;
        for (char ch : body) { if (ch == '{') { ++dd; continue; } if (ch == '}') { --dd; continue; } if (!dd) flat += ch; }
        std::vector<std::string> f;
        size_t s0 = 0;
        for (size_t z = 0; z <= flat.size(); ++z) {
            if (z == flat.size() || flat[z] == ';') {
                std::string decl = flat.substr(s0, z - s0);
                s0 = z + 1;
                // every declarator: name before [..], :bits, or end
                std::string cur; int pd = 0;
                auto take = [&](const std::string& q) {
                    std::smatch m;
                    std::string q2 = q.substr(0, q.find(':'));
                    if (std::regex_search(q2, m, std::regex(R"(([A-Za-z_]\w*)\s*\)?\s*(?:\[[^\]]*\]\s*)*(?:\([^)]*\)\s*)?$)"))) f.push_back(m[1]);
                };
                for (char ch : decl) { if (ch == '(' || ch == '[') ++pd; if (ch == ')' || ch == ']') --pd; if (ch == ',' && !pd) { take(cur); cur.clear(); } else cur += ch; }
                if (cur.find_first_not_of(" \t\r\n") != std::string::npos) take(cur);
            }
        }
        if (!f.empty()) r.push_back(std::move(f));
    }
    return r;
}
std::string reorder_designators(const std::string& t, const std::vector<std::vector<std::string>>& orders);
std::string reorder_list(const std::string& inner, const std::vector<std::vector<std::string>>& orders) {
    // inner: text between '{' and '}', already processed recursively
    std::vector<std::string> el;
    std::string cur; int d = 0; char q = 0;
    for (size_t z = 0; z < inner.size(); ++z) {
        char ch = inner[z];
        if (q) { cur += ch; if (ch == '\\' && z + 1 < inner.size()) cur += inner[++z]; else if (ch == q) q = 0; continue; }
        if (ch == '"' || ch == '\'') q = ch;
        if (ch == '(' || ch == '[' || ch == '{') ++d;
        if (ch == ')' || ch == ']' || ch == '}') --d;
        if (ch == ',' && !d) { el.push_back(cur); cur.clear(); } else cur += ch;
    }
    if (cur.find_first_not_of(" \t\r\n") != std::string::npos) el.push_back(cur);
    if (el.empty()) return inner;
    static const std::regex kField(R"(^\s*\.\s*([A-Za-z_]\w*)\s*=([\s\S]*)$)");
    static const std::regex kIndex(R"(^\s*\[\s*(\d+)[uUlL]*\s*\]\s*=([\s\S]*)$)");
    std::smatch m;
    bool all_field = true, any_index = false, idx_ok = true;
    for (auto& e : el) {
        if (!std::regex_match(e, m, kField)) all_field = false;
        if (std::regex_match(e, m, kIndex)) any_index = true;
        else if (std::regex_search(e, std::regex(R"(^\s*[.\[])"))) idx_ok = false;  // .x or [i+1] mixed in
    }
    if (all_field) {
        std::vector<std::pair<std::string, std::string>> kv;
        for (auto& e : el) { std::regex_match(e, m, kField); kv.emplace_back(m[1], m[2]); }
        const std::vector<std::string>* pick = nullptr;
        for (const auto& o : orders) {
            bool cover = true;
            for (auto& [k, v] : kv) if (std::find(o.begin(), o.end(), k) == o.end()) { cover = false; break; }
            if (!cover) continue;
            if (pick) {
                // a second candidate: fine only when it orders these names the same way
                auto pos = [&](const std::vector<std::string>& oo, const std::string& k) { return std::find(oo.begin(), oo.end(), k) - oo.begin(); };
                for (size_t a = 0; a < kv.size(); ++a) for (size_t b = a + 1; b < kv.size(); ++b)
                    if ((pos(*pick, kv[a].first) < pos(*pick, kv[b].first)) != (pos(o, kv[a].first) < pos(o, kv[b].first))) return inner;
            } else pick = &o;
        }
        if (!pick) return inner;
        std::stable_sort(kv.begin(), kv.end(), [&](auto& a, auto& b) {
            return std::find(pick->begin(), pick->end(), a.first) < std::find(pick->begin(), pick->end(), b.first);
        });
        std::string r;
        for (size_t z = 0; z < kv.size(); ++z) r += (z ? ", ." : " .") + kv[z].first + " =" + kv[z].second;
        return r + " ";
    }
    if (any_index && idx_ok) {
        std::map<unsigned long, std::string> at;
        unsigned long nx = 0;
        for (auto& e : el) {
            if (std::regex_match(e, m, kIndex)) { nx = std::stoul(m[1]); at[nx++] = m[2]; }
            else at[nx++] = e;  // positional continues after the last designator
        }
        if (at.rbegin()->first > 4096) return inner;
        std::string r;
        for (unsigned long z = 0; z <= at.rbegin()->first; ++z) {
            auto f = at.find(z);
            r += (z ? ", " : " ") + (f == at.end() ? std::string("{}") : f->second);
        }
        return r + " ";
    }
    return inner;
}
std::string reorder_designators(const std::string& t, const std::vector<std::vector<std::string>>& orders) {
    if (orders.empty() && t.find('[') == std::string::npos) return t;
    std::string out;
    size_t i = 0;
    while (i < t.size()) {
        char c = t[i];
        if (c == '"' || c == '\'') { size_t e = i + 1; for (; e < t.size() && t[e] != c; ++e) if (t[e] == '\\') ++e; e = std::min(t.size(), e + 1); out.append(t, i, e - i); i = e; continue; }
        if (c == '/' && i + 1 < t.size() && (t[i + 1] == '/' || t[i + 1] == '*')) { size_t e = t[i + 1] == '/' ? t.find('\n', i) : t.find("*/", i + 2); e = e == std::string::npos ? t.size() : (t[i + 1] == '/' ? e : e + 2); out.append(t, i, e - i); i = e; continue; }
        if (c == '{') {
            size_t e = i + 1; int d = 1;
            for (; e < t.size() && d; ++e) { if (t[e] == '"' || t[e] == '\'') { char q = t[e]; for (++e; e < t.size() && t[e] != q; ++e) if (t[e] == '\\') ++e; continue; } if (t[e] == '{') ++d; else if (t[e] == '}') --d; }
            if (d) { out.append(t, i, std::string::npos); break; }
            std::string inner = reorder_designators(t.substr(i + 1, e - i - 2), orders);
            out += "{" + reorder_list(inner, orders) + "}";
            i = e;
            continue;
        }
        out += c; ++i;
    }
    return out;
}
std::optional<DfpSplit> dfp_split(const std::string& in, const std::string& salt = "") {
    struct Item { std::string text; bool fn = false, pp = false; std::string name, head; };
    std::vector<Item> items;
    const size_t n = in.size();
    size_t i = 0, start = 0;
    int depth = 0, paren = 0;
    auto push = [&](size_t end, bool fn, size_t body) {
        Item it;
        it.text = in.substr(start, end - start);
        it.fn = fn;
        if (fn) {
            it.head = in.substr(start, body - start);
            static const std::regex kName(R"(([A-Za-z_]\w*)\s*\([^()]*(?:\([^()]*\)[^()]*)*\)[^()]*$)");
            std::smatch m;
            if (std::regex_search(it.head, m, kName)) it.name = m[1];
        }
        items.push_back(std::move(it));
        start = end;
    };
    size_t body = 0;
    char last = 0;
    while (i < n) {
        const char c = in[i];
        if (c == '/' && i + 1 < n && in[i + 1] == '/') { i = in.find('\n', i); if (i == std::string::npos) i = n; continue; }
        if (c == '/' && i + 1 < n && in[i + 1] == '*') { i = in.find("*/", i + 2); i = i == std::string::npos ? n : i + 2; continue; }
        if (c == '"' || c == '\'') { for (++i; i < n && in[i] != c; ++i) if (in[i] == '\\') ++i; ++i; continue; }
        if (c == '#' && depth == 0) {
            // a preprocessor line (with continuations) is its own item
            size_t ls = in.rfind('\n', i);
            ls = ls == std::string::npos ? 0 : ls + 1;
            if (in.find_first_not_of(" \t", ls) == i) {
                if (ls > start && in.find_first_not_of(" \t\r\n", start) < ls) push(ls, false, 0);
                start = ls;
                size_t e = i;
                for (;;) { e = in.find('\n', e); if (e == std::string::npos) { e = n; break; } if (e > 0 && in[e - 1] == '\\') { ++e; continue; } ++e; break; }
                push(e, false, 0);
                items.back().pp = true;
                i = e;
                continue;
            }
        }
        if (c == '(') ++paren;
        else if (c == ')') --paren;
        else if (c == '{') {
            // a function body: '{' at file scope after ')' or after K&R parameter declarations ending ';'
            if (depth == 0 && paren == 0 && (last == ')' || last == ';')) {
                // K&R: last==';' only counts if a ')' precedes the parameter declarations in this item
                const std::string pre = in.substr(start, i - start);
                if (last == ')' || (pre.find(')') != std::string::npos && pre.find('=') == std::string::npos && pre.find("struct") == std::string::npos))
                    body = i;
                else body = 0;
            } else if (depth == 0) body = 0;
            ++depth;
        } else if (c == '}') {
            if (--depth == 0 && body) { push(i + 1, true, body); body = 0; last = '}'; ++i; continue; }
        } else if (c == ';' && depth == 0 && paren == 0) {
            // a ';' between ')' and '{' belongs to K&R parameter declarations
            size_t k = in.find_first_not_of(" \t\r\n", i + 1);
            const std::string pre = in.substr(start, i - start);
            const bool knr = pre.find(')') != std::string::npos && pre.find('=') == std::string::npos &&
                             std::regex_search(pre, std::regex(R"(^\s*(?:\w+\s+)*\w+\s*\(\s*\w+(?:\s*,\s*\w+)*\s*\))"));
            if (!knr || k == std::string::npos) { push(i + 1, false, 0); last = ';'; ++i; continue; }
            (void)k;
        }
        if (!std::isspace(static_cast<unsigned char>(c))) last = c;
        ++i;
    }
    if (depth != 0) return std::nullopt;
    if (start < n) push(n, false, 0);
    static const std::regex kDec(R"(\b_Decimal(?:32|64|128)\b|\b\d[\d.eE+-]*(?:df|dd|dl|DF|DD|DL)\b|FENV_DEC_ROUND|\bfe_dec_)");
    std::vector<bool> dec(items.size(), false);
    for (size_t k = 0; k < items.size(); ++k) if (items[k].fn && std::regex_search(items[k].text, kDec)) dec[k] = true;
    for (bool grew = true; grew;) {  // callers of decimal functions are decimal
        grew = false;
        for (size_t k = 0; k < items.size(); ++k) {
            if (!items[k].fn || dec[k]) continue;
            for (size_t j = 0; j < items.size() && !dec[k]; ++j)
                if (dec[j] && !items[j].name.empty() && std::regex_search(items[k].text, std::regex("\\b" + items[j].name + "\\s*\\(")))
                    dec[k] = grew = true;
        }
    }
    DfpSplit out;
    // Non-decimal file-scope statics shared with the C++ part: external
    // linkage under a unit-unique name (token-level rename in both parts), defined
    // once on the C side.
    static const std::regex kStatic(R"(^(\s*(?:(?:inline|const|volatile|_Thread_local|__inline__)\s+)*)static\b)");
    static const std::regex kDeclName(R"(([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*(?:\)\s*\([^)]*\))?\s*(?:=|,|;|\(|$))");
    std::string defs;
    std::set<std::string> renamed;
    for (size_t k = 0; k < items.size(); ++k) {
        Item& it = items[k];
        if (it.pp || dec[k] || std::regex_search(it.text, kDec)) continue;
        const std::string& hd = it.fn ? it.head : it.text;
        if (!std::regex_search(hd, kStatic)) continue;
        std::string nm = it.fn ? it.name : "";
        if (!it.fn) {
            // first declarator's name (functions prototypes: the name before '(')
            std::string body = std::regex_replace(it.text, kStatic, "$1");
            std::smatch m;
            if (std::regex_search(body, m, std::regex(R"(([A-Za-z_]\w*)\s*\()")) && body.find('=') == std::string::npos) nm = m[1];
            else if (std::regex_search(body, m, kDeclName)) nm = m[1];
            if (body.find(',') != std::string::npos && body.find('(') == std::string::npos) {
                // several declarators: rename them all
                std::string cur; int pd = 0;
                for (char ch : body) { if (ch == '(' || ch == '[' || ch == '{') ++pd; if (ch == ')' || ch == ']' || ch == '}') --pd; if ((ch == ',' || ch == ';') && pd == 0) { std::smatch mm; std::string c2 = cur.substr(0, cur.find('=')); if (std::regex_search(c2, mm, std::regex(R"(([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*$)"))) renamed.insert(mm[1]); cur.clear(); } else cur += ch; }
            }
        }
        if (!nm.empty()) renamed.insert(nm);
        if (it.fn) { it.text = std::regex_replace(it.text, kStatic, "$1"); it.head = std::regex_replace(it.head, kStatic, "$1"); }
        else it.text = std::regex_replace(it.text, kStatic, "$1");
    }
    std::map<std::string, std::string> ren;
    for (const auto& nm : renamed) ren[nm] = "__prism_split_" + salt + "_" + nm;
    if (!ren.empty())
        for (auto& it : items) { it.text = rename_tokens(it.text, ren); if (!it.head.empty()) it.head = rename_tokens(it.head, ren); it.name = ren.count(it.name) ? ren[it.name] : it.name; }
    (void)defs;
    const auto orders = collect_field_orders(in);
    auto cx = [&](const std::string& t) { return reorder_designators(c_only_to_cxx(t), orders); };
    // extern declarations for every declarator, initialisers dropped
    auto externs = [](const std::string& text) {
        std::string body = text.substr(0, text.rfind(';'));
        std::vector<std::string> parts;
        std::string cur; int pd = 0;
        for (char ch : body) { if (ch == '(' || ch == '[' || ch == '{') ++pd; if (ch == ')' || ch == ']' || ch == '}') --pd; if (ch == ',' && pd == 0) { parts.push_back(cur); cur.clear(); } else cur += ch; }
        parts.push_back(cur);
        for (auto& q : parts) { int d2 = 0; for (size_t z = 0; z < q.size(); ++z) { char ch = q[z]; if (ch == '(' || ch == '[' || ch == '{') ++d2; if (ch == ')' || ch == ']' || ch == '}') --d2; if (ch == '=' && d2 == 0) { q = q.substr(0, z); break; } } }
        std::string r = "extern ";
        for (size_t z = 0; z < parts.size(); ++z) r += (z ? "," : "") + parts[z];
        return r.substr(0, 7) + r.substr(7 + std::min(r.size() - 7, r.substr(7).find_first_not_of(" \t\r\n"))) + ";\n";
    };
    for (size_t k = 0; k < items.size(); ++k) {
        const Item& it = items[k];
        const bool d = std::regex_search(it.text, kDec);
        if (it.pp) { out.c += it.text; out.cxx += it.text; continue; }
        if (it.fn) {
            const std::string proto = it.head + ";\n";
            if (dec[k]) {
                out.any = true;
                // C-only spellings: K&R head -> prototype, [static N] -> [N]
                std::string body = it.text.substr(it.head.size());
                std::string head = it.head.find(';') != std::string::npos ? knr_to_prototype(it.head) : it.head;
                out.cxx += cx(head + body);
                if (!std::regex_search(it.head, kDec)) out.c += proto;
            } else {
                out.c += it.text;
                // a K&R head has no C++ spelling: the C++ side gets no prototype
                out.cxx += c_only_to_cxx((it.head.find(';') == std::string::npos ? it.head : knr_to_prototype(it.head)) + ";\n");
            }
            continue;
        }
        // declarations, types, prototypes
        static const std::regex kObjDef(R"(^\s*(?!typedef\b|extern\b|static\b|struct\s+\w+\s*\{|union\s+\w+\s*\{|enum\b)[^(){};]*(?:=[^;]*)?;\s*$)");
        if (d) { out.cxx += cx(it.text); continue; }
        out.c += it.text;
        if (std::regex_match(it.text, kObjDef)) out.cxx += externs(it.text);
        else out.cxx += c_only_to_cxx(it.text);
    }
    return out;
}

std::optional<std::string> lower_to_ir(const Frontend& fe, const fs::path& src, double timeout_s,
                                       std::string& err, std::vector<FoldedUb>* folded,
                                       std::vector<std::pair<int, int>>* signed_shl, std::string* cxx_models,
                                       const std::vector<std::string>& include_flags, int cxx_std) {
    const bool cxx = is_cxx(src);
    const auto& cc = cxx ? fe.clangxx : fe.clang;
    if (!cc || !fe.opt) {
        err = std::string(cxx ? "clang++" : "clang") + "/opt not found";
        return std::nullopt;
    }
#ifdef _WIN32
    if (cxx) {
        err = "C++ PIR needs libstdc++ (not available on Windows)";
        return std::nullopt;
    }
#endif
    TmpDir td;
    auto o0 = td.path / "o0.ll", o1 = td.path / "o1.ll", o2 = td.path / "o2.ll";
    std::vector<std::string> argv{cc->string()};
    auto fl = base_flags(src);
    fl.insert(fl.end(), include_flags.begin(), include_flags.end());
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
        const bool rm_ok = !rm.failed && !rm.timed_out && rm.rc == 0;
        const bool rm_timed = rm.timed_out;
        const std::string rm_err = rm.text;
        if (rm_ok) {
            // which model headers the unit's code actually uses (their DIFiles)
            const auto ir0 = read_file(o0);
            std::string used;
            for (auto& [name, text] : model_sources())
                // (cxx/prism_*.h are the models' internal headers, not a library header)
                if (name.starts_with("cxx/") && !name.starts_with("cxx/prism_") &&
                    ir0.find((fe.cxx_models / name.substr(4)).string()) != std::string::npos)
                    used += (used.empty() ? "" : ", ") + name.substr(4);
            if (cxx_models && !used.empty()) *cxx_models = "model: " + used;
            modelled = std::move(rm);
        } else if (rm_timed) {
            err = "clang timed out";
            return std::nullopt;
        } else if (cxx_models) {
            *cxx_models = "libstdc++ (fallback: the unit does not compile against the PRISM models: " +
                          first_error(rm_err) + ")";
        }
    }
    argv.insert(argv.end(), fl.begin(), fl.end());
    argv.insert(argv.end(), {"-o", o0.string(), src.string()});
    auto r = modelled.has_value() ? std::move(*modelled) : detail::run_process(argv, timeout_s);
    // C23's static_assert keyword (no <assert.h>) compiles as C17 only as an
    // implicit call to a function named static_assert: also a C23 unit.
    const bool c23_kw = !r.failed && r.rc == 0 && r.text.find("function 'static_assert'") != std::string::npos;
    if (!cxx && !r.timed_out && (r.failed || r.rc != 0 || c23_kw)) {
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
    if (cxx && !r.timed_out && (r.failed || r.rc != 0) &&
        r.text.find("ISO C++17 does not allow") != std::string::npos) {
        // Code from before C++17 (dynamic exception specifications,
        // `register`): try once more as C++14. Only the language level changes.
        auto c14 = fl;
        std::replace(c14.begin(), c14.end(), std::string("-std=c++23"), std::string("-std=c++14"));
        std::vector<std::string> av{cc->string()};
        av.insert(av.end(), c14.begin(), c14.end());
        av.insert(av.end(), {"-o", o0.string(), src.string()});
        auto r2 = detail::run_process(av, timeout_s);
        if (!r2.failed && !r2.timed_out && r2.rc == 0) {
            r = std::move(r2);
            fl = std::move(c14);
            cxx_std = std::min(cxx_std, 14);
        }
    }
    if (!cxx && !r.timed_out && (r.failed || r.rc != 0)) {
        // C23 storage-class compound literals: rewrite, then retry as C23.
        bool changed = false;
        const std::string rw = c23_compound_literal_rewrite(read_file(src), &changed);
        if (changed) {
            const fs::path csrc = o0.parent_path() / (o0.stem().string() + ".cl.c");
            {
                std::ofstream os(csrc, std::ios::binary);
                os << "#line 1 \"" << src.string() << "\"\n" << rw;
            }
            auto c23 = fl;
            std::replace(c23.begin(), c23.end(), std::string("-std=c17"), std::string("-std=c23"));
            c23.insert(c23.end(), {"-I", src.parent_path().string(), "-Wno-gnu-statement-expression"});
            std::vector<std::string> av{cc->string()};
            av.insert(av.end(), c23.begin(), c23.end());
            av.insert(av.end(), {"-o", o0.string(), csrc.string()});
            auto r2 = detail::run_process(av, timeout_s);
            if (!r2.failed && !r2.timed_out && r2.rc == 0) {
                r = std::move(r2);
                fl = std::move(c23);
            }
        }
    }
    if (!cxx && !r.timed_out && (r.failed || r.rc != 0) &&
        r.text.find("decimal type extension not supported") != std::string::npos) {
        // C23 _Decimal32/64/128: clang has no decimal floating point. Lower
        // the unit as C++23 against the PRISM decimal shim instead.
        const fs::path shim = !fe.cxx_models.empty() ? fe.cxx_models / "prism_dfp.hpp"
                                                     : fs::path(__FILE__).parent_path() / "models" / "cxx" / "prism_dfp.hpp";
        const fs::path dsrc = o0.parent_path() / (o0.stem().string() + ".dfp.cpp");
        std::error_code dec;
        if (fs::is_regular_file(shim, dec)) {
            std::string dtext = dfp_rewrite(read_file(src), src);
            std::vector<std::string> dfl;
            for (size_t k = 0; k < fl.size(); ++k) {
                if (fl[k].starts_with("-std=")) { dfl.push_back("-std=c++23"); continue; }
                dfl.push_back(fl[k]);
            }
            dfl.insert(dfl.end(), {"-I", src.parent_path().string(), "-include", shim.string(),
                                   "-D__PRISM_DFP_C_UNIT=1", "-Wno-c99-designator", "-Wno-writable-strings"});
            dtext = dfp_cxx_fixups(std::move(dtext), [&](const std::string& probe) {
                const fs::path pf = o0.parent_path() / (o0.stem().string() + ".dfp.probe.cpp");
                {
                    std::ofstream ps(pf, std::ios::binary);
                    ps << probe;
                }
                std::vector<std::string> pv{cc->string()};
                for (auto& t : dfl)
                    if (t != "-S") pv.push_back(t);
                pv.insert(pv.end(), {"-fsyntax-only", "-fdiagnostics-print-source-range-info", "-fno-caret-diagnostics",
                                     "-fno-color-diagnostics", "-ferror-limit=0", "-x", "c++", pf.string()});
                auto pr = detail::run_process(pv, timeout_s);
                std::error_code rmec;
                fs::remove(pf, rmec);
                return pr.text;
            });
            {
                std::ofstream os(dsrc, std::ios::binary);
                os << dtext;
            }
            std::vector<std::string> av{cc->string()};
            av.insert(av.end(), dfl.begin(), dfl.end());
            av.insert(av.end(), {"-o", o0.string(), "-x", "c++", dsrc.string()});
            auto r2 = detail::run_process(av, timeout_s);
            if (!r2.failed && !r2.timed_out && r2.rc == 0) {
                r = std::move(r2);
                fl = std::move(dfl);
                if (cxx_models) *cxx_models = "dfp shim: C23 decimal floating point lowered as C++ (models/cxx/prism_dfp.hpp)";
            } else {
                // Item 6: split decimal functions into their own C++ file,
                // keep the rest as C, and link the two modules.
                bool split_ok = false;
                if (auto sp = dfp_split(read_file(src), std::to_string(std::hash<std::string>{}(src.string()) % 1000000007u)); sp && sp->any) {
                    const fs::path cpart = o0.parent_path() / (o0.stem().string() + ".split.c");
                    const fs::path xpart = o0.parent_path() / (o0.stem().string() + ".split.cpp");
                    const fs::path cll = o0.parent_path() / (o0.stem().string() + ".split.c.ll");
                    const fs::path xll = o0.parent_path() / (o0.stem().string() + ".split.cpp.ll");
                    {
                        std::ofstream a1(cpart, std::ios::binary);
                        a1 << "#line 1 \"" << src.string() << "\"\n" << sp->c;
                    }
                    std::string xt = dfp_rewrite(sp->cxx, src);
                    {
                        std::ofstream a2(xpart, std::ios::binary);
                        a2 << xt;
                    }
                    std::vector<std::string> cv{cc->string()};
                    cv.insert(cv.end(), fl.begin(), fl.end());
                    cv.insert(cv.end(), {"-I", src.parent_path().string(), "-o", cll.string(), cpart.string()});
                    std::vector<std::string> xv{cc->string()};
                    xv.insert(xv.end(), dfl.begin(), dfl.end());
                    xv.insert(xv.end(), {"-Wno-reorder-init-list", "-Wno-vla-cxx-extension", "-o", xll.string(), "-x", "c++", xpart.string()});
                    auto rc1 = detail::run_process(cv, timeout_s);
                    auto rc2 = detail::run_process(xv, timeout_s);
                    if (!rc1.failed && !rc1.timed_out && rc1.rc == 0 && !rc2.failed && !rc2.timed_out && rc2.rc == 0) {
                        const fs::path link = fs::path(*cc).parent_path() / "llvm-link";
                        auto rl = detail::run_process({link.string(), "-S", "-o", o0.string(), cll.string(), xll.string()}, timeout_s);
                        if (!rl.failed && !rl.timed_out && rl.rc == 0) {
                            r = std::move(rc1);
                            r.text += rc2.text;
                            split_ok = true;
                            if (cxx_models) *cxx_models = "dfp shim (split): decimal functions lowered as C++, the rest as C, linked";
                        } else r.text += "\nprism: decimal split link failed: " + first_error(rl.text);
                    } else {
                        r.text += "\nprism: decimal split failed: " + first_error(rc1.rc ? rc1.text : rc2.text);
                    }
                }
                if (!split_ok) r.text += "\nprism: C23 decimal floating point shim also failed: " + first_error(r2.text);
            }
        }
    }
    if (r.failed || r.timed_out || r.rc != 0) {
        err = r.timed_out ? "clang timed out" : "clang: " + first_error(r.text);
        return std::nullopt;
    }
    // Nothing defined: the unit's functions are all inline, constexpr or
    // static and unused, so clang emitted none. Emit them anyway to check them
    // (only then: in other units it would pull in every unused library inline).
    if (read_file(o0).find("\ndefine ") == std::string::npos) {
        // clang writes -o through a temporary file, so a failed run leaves
        // o0 as it was.
        std::vector<std::string> av{cc->string()};
        av.insert(av.end(), fl.begin(), fl.end());
        av.insert(av.end(), {"-femit-all-decls", "-o", o0.string(), src.string()});
        (void)detail::run_process(av, timeout_s);
    }
    auto diags = parse_folded_clang_diagnostics(r.text, src);
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
        bool want_shift_loc = false;
        for (auto& d : diags)
            if (d.cls == "INT-SHIFT-UB") {
                want_shift_loc = true;
                break;
            }
        // C++ before C++20 (P1236) has the C++11 rule (CWG 1457): the result
        // must fit the unsigned counterpart. clang's shift-base check follows
        // the -std it is given, so the locator runs at the unit's standard.
        const bool shl_ub = !cxx || cxx_std < 20;
        if (shl_ub && (out.find(" = shl ") != std::string::npos || want_shift_loc)) {
            std::vector<std::string> av{cc->string()};
            av.insert(av.end(), fl.begin(), fl.end());
            if (cxx)
                for (auto& a : av)
                    if (a.starts_with("-std=")) a = "-std=c++" + std::to_string(cxx_std < 11 ? 98 : cxx_std);
            auto os = td.path / "shift.ll";
            av.insert(av.end(), {"-fsanitize=shift-base", "-o", os.string(), src.string()});
            auto rs = detail::run_process(av, timeout_s);
            if (cxx && (rs.failed || rs.timed_out || rs.rc != 0)) {
                // The unit needs a later standard than it declares: C++20 rules.
            } else if (rs.failed || rs.timed_out || rs.rc != 0) {
                err = "clang (signed-shift locator): " + first_error(rs.text);
                return std::nullopt;
            } else {
                *signed_shl = parse_ubsan_locs(read_file(os));
            }
        }
    }
    return out;
}

namespace {

// "17", "1z", "20", "2a", "23", "2b", "2c", "98", "03", "0x" -> 17, 20, ...
int cxx_std_number(const std::string& v) {
    static const std::map<std::string, int> m{{"98", 98}, {"03", 98}, {"0x", 11}, {"11", 11}, {"1y", 14}, {"14", 14},
                                              {"1z", 17}, {"17", 17}, {"2a", 20}, {"20", 20}, {"2b", 23},
                                              {"23", 23}, {"2c", 26}, {"26", 26}};
    auto it = m.find(v.substr(0, 2));
    return it == m.end() ? 17 : it->second == 98 ? 3 : it->second;
}

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
            auto w = ws.at(k);
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
    bool header = false;  // a C header lowered alone for its unused inline functions
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
        auto& r = recs.at(i);
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
        for (auto i : todo) recs.at(i).f.extra["tv"] = "NOTRUN (" + why + ")";
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
        auto& r = recs.at(c.rec);
        const auto* irf = r.irf;
        std::string args;
        for (std::size_t k = 0; k < c.args.size(); ++k) {
            auto w = r.fn->vars[static_cast<std::size_t>(r.fn->params[k])].width;
            if (k) args += ", ";
            if (irf->params[k].ty.kind == ir::Type::Float) {  // IEEE bits as an LLVM float literal
                args += irf->params.at(k).ty.text + " " + fp::ll_const(c.args.at(k), w);
                continue;
            }
            args += "i" + std::to_string(w);
            auto& at = irf->params[k].attrs;
            if (at.find("signext") != std::string::npos) args += " signext";
            if (at.find("zeroext") != std::string::npos) args += " zeroext";
            args += " " + ll_const(c.args.at(k), w);
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
        auto& r = recs.at(c.rec);
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
        auto& f = recs.at(i).f;
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
    for (int i = line; i <= end && i <= static_cast<int>(lines.size()); ++i)
        out += lines.at(static_cast<std::size_t>(i - 1)) + "\n";
    return out;
}

// Solver settings of the run: the portfolio and the query cache always
// (docs/SOLVERS.md), a certificate per VC with --certified. The members of
// one query share the cores left to this worker.
// The run's Houdini budget (docs/PIR.md "Loop invariants"): seconds the
// loop-invariant search may take over the whole run, summed over the
// workers. $PRISM_HOUDINI_BUDGET seconds (0: none), else max(3600 s,
// 120 x timeout). A function that finds it spent stays BOUNDED with an
// invariants_note (Law 7: it says what was not tried).
// A budget in seconds from the environment: unset or empty leaves `out`
// alone; a value that is not a number >= 0 is rejected (returned as a note,
// Law 7: a typo must not run unbudgeted without saying so).
std::optional<std::string> env_seconds(const char* name, double& out) {
    const char* e = std::getenv(name);
    if (!e || !*e) return std::nullopt;
    char* end = nullptr;
    const double x = std::strtod(e, &end);
    if (end && *end == '\0' && x >= 0 && std::isfinite(x)) {
        out = x;
        return std::nullopt;
    }
    return std::string(name) + "='" + e + "' is not a number of seconds >= 0; ignored";
}

// The budget variables whose value was rejected (one note, "; "-joined;
// empty: none). Every checked pir row carries it as extra.env_rejected.
std::string rejected_budget_env() {
    std::string out;
    for (const char* name : {"PRISM_FUNCTION_BUDGET", "PRISM_HOUDINI_BUDGET", "PRISM_CERTIFY_BUDGET"}) {
        double ignored = 0;
        if (auto why = env_seconds(name, ignored)) out += (out.empty() ? "" : "; ") + *why;
    }
    return out;
}

// $PRISM_PIR_PROGRESS=FILE: one line per unit analysed ("start PATH" and
// "done SECONDS PATH"), appended and flushed, so a slow or stuck unit in a
// long run can be named from outside the process.
void progress_note(const char* what, const fs::path& unit, double secs) {
    static std::mutex m;
    const char* f = std::getenv("PRISM_PIR_PROGRESS");
    if (!f || !*f) return;
    std::lock_guard<std::mutex> lk(m);
    std::ofstream o(f, std::ios::app);
    o << what << ' ';
    if (secs > 0) o << secs << ' ';
    o << unit.string() << '\n';
}

}  // namespace

double default_function_budget_s(const Config& cfg) { return std::max(300.0, 10.0 * cfg.timeout); }

namespace {
std::shared_ptr<HoudiniBudget> houdini_budget(const Config& cfg) {
    double b = std::max(3600.0, 120.0 * cfg.timeout);
    (void)env_seconds("PRISM_HOUDINI_BUDGET", b);  // a rejected value: rejected_budget_env
    if (b <= 0) return nullptr;
    return std::make_shared<HoudiniBudget>(b);
}

CheckOptions check_options(const Config& cfg, std::shared_ptr<HoudiniBudget> budget = nullptr) {
    CheckOptions o;
    o.houdini_budget = std::move(budget);
    o.unwind = cfg.unwind;
    o.timeout_s = cfg.timeout;
    o.certified = cfg.certified;
    // Certification budget per PROVED function (docs/PIR.md "Solving"):
    // $PRISM_CERTIFY_BUDGET seconds (0: none), else max(240 s, 8 x
    // timeout), the sum of one query's certificate and checker budgets, so
    // a single certificate is never cut shorter than before while a chain of
    // per-VC certificates cannot run away with the run's time.
    o.certify_budget_s = std::max(240.0, 8.0 * cfg.timeout);
    (void)env_seconds("PRISM_CERTIFY_BUDGET", o.certify_budget_s);
    // Per-function pir budget (docs/PIR.md "Solving"): $PRISM_FUNCTION_BUDGET
    // seconds (0: none). Spent before every VC is answered: TIMEOUT with the
    // budget message and extra.function_budget_s (CheckOptions).
    // Default: max(300 s, 10 x timeout). With none, one function whose Z3
    // query grows without tripping the memory budget (filesystem_contracts.cpp
    // fs_stem_dots_true under a 3000 MB budget) held its whole batch, and the
    // other workers idle, for 10+ minutes; with 60 s the unit took 192 s.
    o.function_budget_s = default_function_budget_s(cfg);
    (void)env_seconds("PRISM_FUNCTION_BUDGET", o.function_budget_s);
    o.cache_dir = cfg.solver_cache.string();
    // Solver and checker binaries: --tool NAME=PATH first, then the pinned
    // build under tools_home() (not inside the scanned tree without
    // --allow-exec, Law 9), then PATH (solver::find_tool).
    for (const auto& [name, path] : cfg.tools) o.tool_paths[name] = path.string();
    if (!cfg.allow_exec) o.refuse_tools_under = cfg.root.string();
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
    // Functions the front end rejected, cut out so the rest of the unit could
    // be lowered (salvage_unit): name, line, the first clang error.
    std::vector<std::tuple<std::string, int, std::string>> dropped;
};

// ---- Front-end salvage ----
// The bmc front end checks each function on its own, so one function clang
// rejects (an undeclared name, a construct clang does not support) costs only
// that function. Clang rejects the whole unit. When every error clang reports
// lies inside top-level function bodies, those definitions are turned into
// declarations (same line numbers, the file's own name via #line) and the
// unit is lowered again; each cut function is reported NEEDS-HARNESS.
struct Salvage {
    std::string text;
    std::vector<std::tuple<std::string, int, std::string>> dropped;
};

std::optional<Salvage> salvage_unit(const fs::path& src, const std::string& err, bool hoist = true) {
    const std::string text = read_file(src);
    if (text.empty()) return std::nullopt;
    // error lines in this file
    std::vector<std::pair<int, std::string>> errs;
    {
        const std::string tag = src.string() + ":";
        std::size_t pos = 0;
        while ((pos = err.find(tag, pos)) != std::string::npos) {
            std::size_t q = pos + tag.size();
            int line = 0;
            while (q < err.size() && std::isdigit(static_cast<unsigned char>(err[q]))) line = line * 10 + (err[q++] - '0');
            const std::size_t eol = err.find('\n', q);
            const std::string rest = err.substr(q, eol == std::string::npos ? std::string::npos : eol - q);
            if (line > 0 && rest.find(" error: ") != std::string::npos) {
                const auto e = rest.find("error: ");
                errs.emplace_back(line, rest.substr(e + 7));
            }
            pos = q;
        }
    }
    if (errs.empty() || err.find("fatal error") != std::string::npos) return std::nullopt;
    // top-level { } blocks, skipping comments, strings and preprocessor lines
    struct Block { std::size_t head, open, close; int l0, l1; };
    std::vector<Block> blocks;
    int depth = 0, line = 1, open_line = 0;
    std::size_t head = 0, open = 0;
    bool bol = true;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\n') { ++line; bol = true; continue; }
        if (bol && ch == '#' && depth == 0) {  // directive: to end of line (with continuations)
            while (i < text.size() && !(text[i] == '\n' && text[i - 1] != '\\')) ++i;
            ++line;
            head = i + 1;
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(ch))) bol = false;
        if (ch == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i + 1 < text.size() && text[i + 1] != '\n') ++i;
            continue;
        }
        if (ch == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) { if (text[i] == '\n') ++line; ++i; }
            ++i;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            for (++i; i < text.size() && text[i] != ch; ++i) {
                if (text[i] == '\\') ++i;
                else if (text[i] == '\n') ++line;
            }
            continue;
        }
        if (ch == '{') {
            if (depth == 0) { open = i; open_line = line; }
            ++depth;
        } else if (ch == '}') {
            if (--depth < 0) return std::nullopt;
            if (depth == 0) {
                blocks.push_back({head, open, i, open_line, line});
                head = i + 1;
            }
        } else if (ch == ';' && depth == 0) {
            head = i + 1;
        }
    }
    if (depth != 0) return std::nullopt;
    static const Regex fn_head(R"(([A-Za-z_][A-Za-z_0-9]*)\s*\([^{};]*\)[^{};()]*$)");
    static const Regex not_fn(R"(^\s*(?:struct|class|union|enum|namespace|extern|template|typedef)\b|=\s*$)");
    std::string out = text;
    Salvage sv;
    std::set<std::size_t> cut;
    std::vector<std::pair<int, std::string>> hoisted;
    for (auto& [eline, msg] : errs) {
        const Block* hit = nullptr;
        for (auto& b : blocks)
            if (b.l0 <= eline && eline <= b.l1) { hit = &b; break; }
        if (!hit) return std::nullopt;  // an error outside every function body
        // the error is in the head (the signature itself): not salvageable
        const std::string hd = text.substr(hit->head, hit->open - hit->head);
        if (eline < hit->l0) return std::nullopt;
        if (not_fn.search_match(hd)) return std::nullopt;
        auto m = fn_head.search_match(hd);
        if (!m) return std::nullopt;
        // A GNU C nested function (clang: "function definition is not allowed
        // here"): besides cutting the outer function, hoist the nested one to
        // file scope at its own lines, so it is checked when it uses nothing
        // of the outer frame (if it does, the next round's error cuts it).
        if (hoist && msg.find("function definition is not allowed here") != std::string::npos) {
            std::size_t ls = 0;
            for (int ln = 1; ln < eline && ls != std::string::npos; ++ln) ls = text.find('\n', ls) + 1;
            const std::size_t ob = ls < text.size() ? text.find('{', ls) : std::string::npos;
            if (ob != std::string::npos && ob < hit->close) {
                int d = 0;
                std::size_t k = ob;
                for (; k < hit->close; ++k) {
                    if (text[k] == '{') ++d;
                    else if (text[k] == '}' && --d == 0) break;
                }
                if (d == 0 && k < hit->close && !std::any_of(hoisted.begin(), hoisted.end(), [&](auto& h) { return h.first == eline; }))
                    hoisted.emplace_back(eline, text.substr(ls, k + 1 - ls));
            }
        }
        if (!cut.insert(hit->open).second) continue;
        sv.dropped.emplace_back(m->group(1), hit->l0, msg);
        // body -> ';' keeping every newline
        out[hit->open] = ';';
        for (std::size_t k = hit->open + 1; k <= hit->close; ++k)
            if (out[k] != '\n') out[k] = ' ';
    }
    if (sv.dropped.empty()) return std::nullopt;
    std::string esc;
    for (char c2 : src.string()) { if (c2 == '"' || c2 == '\\') esc += '\\'; esc += c2; }
    sv.text = "#line 1 \"" + esc + "\"\n" + out;
    for (auto& [ln, code] : hoisted) sv.text += "\n#line " + std::to_string(ln) + " \"" + esc + "\"\n" + code + "\n";
    return sv;
}


struct Analyzed {
    std::vector<FnRec> recs;
    std::unique_ptr<ir::Module> mod;  // FnRec::irf points into it
    std::vector<Finding> out;         // unit-level rows (front-end failure, no functions)
};


// ---- Fresh-process continuation after a hard Z3 out-of-memory ----
// Past Z3's hard memory limit Z3 is not usable again in this process
// (encode.cpp z3_hard_oom), so every later function of the run would be
// UNKNOWN. Those functions are checked again by a child PRISM (this binary,
// the same command line, pir stage only, PRISM_PIR_ONLY=<unit>\t<fn>,...),
// whose verdicts replace the UNKNOWN rows. The child re-runs the same
// deterministic checks; a child that runs out of memory again does the same
// for its own remaining functions. Linux only (/proc/self); elsewhere the
// rows stay UNKNOWN (Law 7: said, not dropped).
bool rerun_only_wanted(const std::string& unit, const std::string& fn) {
    static const std::optional<std::set<std::string>> only = []() -> std::optional<std::set<std::string>> {
        const char* e = std::getenv("PRISM_PIR_ONLY");
        if (!e || !*e) return std::nullopt;
        std::set<std::string> r;
        std::string cur;
        for (const char* p = e;; ++p) {
            if (*p == '\n' || *p == '\0') {
                if (!cur.empty()) r.insert(cur);
                cur.clear();
                if (!*p) break;
            } else {
                cur += *p;
            }
        }
        return r;
    }();
    return !only || only->count(unit + "\t" + fn) > 0;
}

std::vector<std::string> self_cmdline() {
    std::vector<std::string> av;
#ifdef __linux__
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    std::string a;
    while (std::getline(in, a, '\0')) av.push_back(a);
#endif
    return av;
}

void rerun_after_oom(const Unit& u, std::vector<FnRec>& recs, const Config& cfg) {
    std::vector<FnRec*> pend;
    for (auto& r : recs)
        if (auto it = r.f.extra.find("pir_rerun"); it != r.f.extra.end() && it->second == "pending") pend.push_back(&r);
    if (pend.empty()) return;
    auto note = [&](const std::string& why) {
        for (auto* r : pend) r->f.extra["pir_rerun"] = why;
    };
    auto av = self_cmdline();
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (av.empty() || ec) return note("not possible here (no /proc/self): stays UNKNOWN");
    static std::atomic<int> seq{0};
    const fs::path out = fs::temp_directory_path() /
                         ("prism_pir_rerun_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
    std::string only;
    for (auto* r : pend) only += u.rel + "\t" + r->f.function.value_or("") + "\n";
    std::vector<std::string> cmd{"/usr/bin/env", "PRISM_PIR_ONLY=" + only};
    // The child gets what the tree leaves of the budget, so parent + child stay in it.
    if (const uint64_t mb = membudget::budget_mb(); mb > 0) {
        const uint64_t rss = membudget::tree_rss_mb();
        cmd.push_back("PRISM_MEM_BUDGET_MB=" + std::to_string(std::max<uint64_t>(256, mb > rss ? mb - rss : 0)));
    }
    cmd.push_back(exe.string());
    for (std::size_t i = 1; i < av.size(); ++i) {
        const std::string& a = av[i];
        if (a == "--out" || a == "--stage" || a == "--skip") { ++i; continue; }
        if (a.starts_with("--out=") || a.starts_with("--stage=") || a.starts_with("--skip=") || a == "--resume" ||
            a == "--gui")
            continue;
        cmd.push_back(a);
    }
    cmd.insert(cmd.end(), {std::string("--stage"), std::string("inventory,classify,pir"), std::string("--out"), out.string()});
    const double t = 120 + 3.0 * std::max(60.0, cfg.timeout) * static_cast<double>(pend.size());
    auto r = detail::run_process(cmd, t);
    std::map<std::string, nlohmann::json> got;
    {
        std::ifstream in(out / "stages.jsonl");
        std::string line;
        while (std::getline(in, line)) {
            auto j = nlohmann::json::parse(line, nullptr, false);
            if (j.is_discarded() || j.value("name", "") != "pir" || !j.contains("findings")) continue;
            for (auto& x : j["findings"])
                if (x.contains("file") && x["file"].is_string() && x["file"] == u.rel && x.contains("function") &&
                    x["function"].is_string())
                    got[x["function"].get<std::string>()] = x;
        }
    }
    fs::remove_all(out, ec);
    for (auto* rp : pend) {
        auto& f = rp->f;
        auto it = got.find(f.function.value_or(""));
        if (it == got.end()) {
            f.extra["pir_rerun"] = std::string("fresh process gave no verdict (") +
                                   (r.timed_out ? "timed out" : "rc " + std::to_string(r.rc)) + "): stays UNKNOWN";
            continue;
        }
        auto& x = it->second;
        f.status = x.value("status", f.status);
        f.message = x.value("message", f.message);
        f.cls = x.value("cls", f.cls);
        f.strength = x.value("strength", f.strength);
        f.counterexample = x.value("counterexample", std::string());
        if (x.contains("line") && x["line"].is_number_integer()) f.line = x["line"].get<int>();
        if (x.contains("extra") && x["extra"].is_object())
            for (auto& [k, v] : x["extra"].items())
                if (v.is_string()) f.extra[k] = v.get<std::string>();
        if (!f.extra.count("pir_rerun") || f.extra["pir_rerun"] == "pending")
            f.extra["pir_rerun"] = "fresh process (Z3 memory used up earlier in this one)";
        rp->cex.clear();
        if (f.status != laws::FAILED) rp->fn.reset();  // nothing to replay
    }
}

// An error sink without parameters (SV-COMP's reach_error: its assertion
// fails whenever it is called) is FAILED when checked alone, though whether
// it fails is decided only by its callers: most of the pir-vs-bmc hard
// conflicts. When every caller in the unit inlined it (so each call site was
// checked, and a reachable call is FAILED in that caller), the sink becomes
// PROVED-ASSUMING "called only from those checked call sites", as bmc treats
// it. A sink with no caller in the unit keeps FAILED (an outside caller may
// reach it); a function with parameters (__VERIFIER_assert(cond)) keeps
// FAILED (some argument fails it), as do failures that are not assertions.
void caller_discharged_asserts(std::vector<FnRec>& recs, const ir::Module& mod) {
    std::map<std::string, std::set<std::string>> callers;  // IR callee -> IR callers
    for (const auto& fn : mod.functions) {
        if (fn.is_declaration || fn.is_model) continue;
        for (const auto& b : fn.blocks)
            for (const auto& in : b.insts)
                if (!in.callee.empty() && in.callee != fn.name) callers[in.callee].insert(fn.name);
    }
    std::map<std::string, FnRec*> by_ir;
    for (auto& r : recs)
        if (r.irf) by_ir[r.irf->name] = &r;
    auto split = [](const std::string& csv) {
        std::set<std::string> out;
        std::string cur;
        for (char ch : csv + ",") {
            if (ch == ',') { if (!cur.empty()) out.insert(cur); cur.clear(); }
            else cur += ch;
        }
        return out;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& r : recs) {
            if (!r.irf || r.f.status != laws::FAILED || !r.f.message.starts_with("assert:")) continue;
            // Only a sink without parameters (reach_error): whether it fails is
            // decided entirely by whether it is called. A function with
            // parameters (__VERIFIER_assert(cond)) can fail for some caller's
            // argument and keeps FAILED alone, as bmc reports it.
            if (!r.irf->params.empty()) continue;
            auto it = callers.find(r.irf->name);
            if (it == callers.end() || it->second.empty()) continue;
            const std::string src_name = r.f.function.value_or(r.irf->name);
            bool ok = true;
            std::string names;
            for (const auto& cn : it->second) {
                auto c = by_ir.find(cn);
                if (c == by_ir.end()) { ok = false; break; }
                auto inl = c->second->f.extra.find("inlined");
                if (inl == c->second->f.extra.end() || !split(inl->second).count(src_name)) { ok = false; break; }
                names += (names.empty() ? "" : ", ") + c->second->f.function.value_or(cn);
            }
            if (!ok) continue;
            r.f.extra["verdict_alone"] = r.f.status + ": " + r.f.message;
            r.f.status = std::string(laws::PROVED_ASSUMING);
            r.f.strength = std::string(laws::STRENGTH_SOME);
            r.f.message = "error sink: it fails exactly when called; every caller in this unit (" + names +
                          ") inlined it, so reaching it is checked (and reported) at each call site";
            r.f.extra["assumption"] = "called only from the call sites in this unit, which are checked";
            r.f.counterexample.clear();
            r.f.extra.erase("cex");
            r.cex.clear();
            r.fn.reset();
            changed = true;
        }
    }
}

Analyzed run_unit(const Unit& u, const Frontend& fe, const Config& cfg, const Lowered& low,
                  const ModelLibrary& models, const std::shared_ptr<HoudiniBudget>& budget) {
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
        // A header the unit's include path does not have (no
        // compile_commands.json entry, or a generated header not built yet)
        // is a gap in what could be compiled, not an internal error (Law 7).
        static const Regex missing_header(
            R"(fatal error:\s*(?:'([^'\n]+)' file not found|([^:\n]+): No such file or directory))");
        if (auto mh = missing_header.search_match(low.err)) {
            f.status = std::string(laws::NOTRUN);
            f.message = "does not compile standalone: header '" + (mh->group(1).empty() ? mh->group(2) : mh->group(1)) +
                        "' not found (include path unknown); not checked";
            f.extra["install"] = "generate compile_commands.json (cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON, bear)";
        }
        out.push_back(std::move(f));
        return res;
    }
    for (const auto& [name, line, why] : low.dropped) {
        auto f = base_finding(u);
        f.function = name;
        f.line = line;
        f.status = std::string(laws::NEEDS_HARNESS);
        f.strength = std::string(laws::STRENGTH_SOME);
        f.message = "the clang front end rejects this function (" + why +
                    "); it was cut out and the rest of the unit checked";
        f.extra["frontend"] = fe.version;
        f.extra["frontend_salvage"] = "cut";
        out.push_back(std::move(f));
    }
    res.mod = std::make_unique<ir::Module>(ir::parse_module(*ir));
    auto& mod = *res.mod;
    // library operational models for what the unit declares (docs/PIR.md "Library models")
    link_models(mod, models);
    TranslateOptions topt;
    topt.signed_shl = sshl;
    topt.shl_cxx = is_cxx(u.path);
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
        if (!rerun_only_wanted(u.rel, *f.function)) continue;  // fresh-process re-run of other functions
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
            if (!models.error.empty() && tr.reason.starts_with("UNENCODED: call @")) {
                // the callee may be one the library models define: not a
                // property of the function but a stage that could not run (Law 7)
                f.status = std::string(laws::NOTRUN);
                f.message = "library models could not be built (" + models.error + "): " + tr.reason;
                f.extra["models_error"] = models.error;
            }
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
        // one function budget for the check and the policy's re-checks of this row
        auto copt = check_options(cfg, budget);
        if (copt.function_budget_s > 0) copt.function_budget = std::make_shared<FunctionBudget>(copt.function_budget_s);
        auto v = check_function(fn, copt);
        // Re-checked fresh only when this process already checked another
        // function (its Z3 memory, or a hard out-of-memory, is what stopped
        // this one), so the child processes always make progress.
        static std::atomic<int> g_checked{0};
        const bool fresh_helps = g_checked++ > 0;
        if (fresh_helps && v.status == laws::UNKNOWN && v.message.starts_with("not checked: the encoder's memory"))
            f.extra["pir_rerun"] = "pending";  // checked again in a fresh process (rerun_after_oom)
        pirmem::apply_memory_policy(f, v, fn, mod, irf, fopt, cfg, copt);
        if (auto rej = rejected_budget_env(); !rej.empty()) f.extra["env_rejected"] = rej;
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
        // Folded UB whose IR marker was dropped or not encoded must not stand as PROVED.
        for (std::size_t k = 0; k < folded.size(); ++k) {
            if (used_folded.count(static_cast<int>(k))) continue;
            if (folded[k].line <= 0) continue;
            if (!f.line || *f.line != folded[k].line) continue;
            if (f.status != std::string(laws::PROVED) && f.status != std::string(laws::PROVED_UNBOUNDED) &&
                f.status != std::string(laws::BOUNDED))
                continue;
            f.status = std::string(laws::FAILED);
            f.strength = std::string(laws::STRENGTH_PROVES);
            f.cls = folded[k].cls;
            f.message = "clang-folded UB: " + folded[k].msg;
            f.extra["prop"] = "folded";
            rec.cex.clear();
            f.counterexample.clear();
            f.extra.erase("cex");
            rec.fn.reset();
            break;
        }
        rec.fn = std::move(tr.fn);
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
        const std::string folded_msg = folded.at(k).msg;
        owner->f.message = "UNENCODED: clang-folded UB at line " + std::to_string(line) +
                           " not attributable to an IR instruction (" + folded_msg + ")";
        owner->fn.reset();
    }
    caller_discharged_asserts(recs, mod);
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
    // the child budgets project this process's growth from its peak in pir,
    // not from earlier stages (bmc) that have released their memory
    membudget::reset_peak();
    // SVCOMP-ILP32: pir's pointer model is LP64. Do not prove an ILP32 task here.
    if (is_ilp32(cfg.data_model)) {
        Finding f;
        f.stage = "pir";
        f.status = std::string(laws::NOTRUN);
        f.message = "pir is LP64-only; ILP32 tasks are analysed by the bmc stage (32-bit long, size_t, pointers)";
        f.strength = std::string(laws::STRENGTH_PROVES);
        return {f};
    }
    std::vector<Unit> units;
    for (auto& p : sources)
        if (is_unit(p)) units.push_back(Unit{p, rel_of(p, cfg.root)});
    // A C header in the tree that defines functions (static inline helpers):
    // a helper no unit calls is emitted by none, so it would have no verdict.
    // The header is lowered alone as C with every declaration emitted, after
    // the units; only functions no unit reported are kept from it.
    if (!units.empty() && std::getenv("PRISM_PIR_NO_HEADERS") == nullptr) {
        static const Regex has_body(R"(\)\s*\{)");
        for (auto& p : sources)
            if (p.extension() == ".h" && has_body.search(read_file(p)))
                units.push_back(Unit{p, rel_of(p, cfg.root), true});
    }
    if (!z3_available()) {
        Finding f;
        f.stage = "pir";
        f.status = std::string(laws::NOTRUN);
        f.message = "z3 not built";
        f.strength = std::string(laws::STRENGTH_PROVES);
        f.extra["install"] = "rebuild with -DPRISM_Z3=ON (vendored third_party/z3)";
        return {f};
    }
#ifdef _WIN32
    {
        std::string why;
        if (!frontend_usable(cfg, &why)) {
            Finding f;
            f.stage = "pir";
            f.status = std::string(laws::NOTRUN);
            if (why.starts_with("NOTRUN: ")) f.message = why.substr(8);
            else f.message = std::move(why);
            f.strength = std::string(laws::STRENGTH_PROVES);
            f.extra["install"] = kInstall;
            return {f};
        }
    }
#endif
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
    // library models: lowered once per run (process phase) and cached across
    // runs; their own time limit, well above the per-unit one: they are
    // PRISM's code, and every library call of every unit depends on them
    auto models = build_models(fe, std::max(120.0, 4 * cfg.timeout));
    // C++ library model headers (docs/PIR.md "C++ library models"): written
    // once per run; without them C++ units use the platform library, and
    // every C++ function says which one it was checked against.
    std::optional<TmpDir> cxx_dir;
    std::string cxx_models_error;
    // C units need the directory too: the C23 decimal route force-includes
    // prism_dfp.hpp from it (embedded, so installed builds have it).
    if (!units.empty()) {
        cxx_dir.emplace();
        if (cxx_dir.has_value() && write_cxx_models(cxx_dir->path / "prism_cxx"))
            fe.cxx_models = cxx_dir->path / "prism_cxx";
        else
            cxx_models_error = "libstdc++ (the PRISM C++ model headers could not be written)";
    }
    // Units go through lower/analyse/re-run/validate in batches, and each
    // batch's LLVM IR is freed before the next is lowered. Lowering every unit
    // first kept the whole corpus's IR alive at once: RSS grew with the corpus
    // (4.9 GB on testdata, OOM-killed at 12.8 GB with --jobs 8), and no
    // per-function budget check can see memory held between phases.
    const std::vector<Unit>& units_all = units;
    std::vector<std::vector<Finding>> per_all(units_all.size());
    const auto budget = houdini_budget(cfg);
    // One pipeline per unit (lower, analyse, validate, free its IR) on a work
    // queue: no phase barrier, so a slow unit holds one worker, not the batch.
    std::size_t batch_n = std::max<std::size_t>(1, units_all.size());
    if (const char* e = std::getenv("PRISM_PIR_BATCH")) batch_n = std::max<std::size_t>(1, std::strtoull(e, nullptr, 10));
    for (std::size_t b0 = 0; b0 < units_all.size(); b0 += batch_n) {
    const std::vector<Unit> batch(units_all.begin() + static_cast<std::ptrdiff_t>(b0),
                                  units_all.begin() + static_cast<std::ptrdiff_t>(std::min(units_all.size(), b0 + batch_n)));
    std::vector<Lowered> low(batch.size());
    std::vector<Analyzed> an(batch.size());
    std::vector<std::vector<Finding>> per(batch.size());
    std::vector<char> pending(batch.size(), 0);
    auto finish = [&](std::size_t i) {
        auto& a = an.at(i);
        const auto vt0 = std::chrono::steady_clock::now();
        progress_note("validate-start", batch.at(i).path, 0.0);
        if (low.at(i).ir && !a.recs.empty()) validate(a.recs, *low.at(i).ir, fe, cfg);
        progress_note("validate-done", batch.at(i).path, std::chrono::duration<double>(std::chrono::steady_clock::now() - vt0).count());
        per.at(i) = std::move(a.out);
        for (auto& r : a.recs) per.at(i).push_back(std::move(r.f));
        if (!low.at(i).cxx_models.empty())
            for (auto& f : per.at(i)) f.extra["cxx_models"] = low.at(i).cxx_models;
        low.at(i).ir.reset();
        a = Analyzed{};
    };
    parallel_for(cfg.jobs, batch, [&](std::size_t i, const Unit& u) {
        {
        auto& l = low.at(i);
        try {
            // The unit's include path: its compile_commands.json entry (without
            // -std=, PRISM's language level stays), else the scan root and
            // root/include searched after the system headers (-idirafter never
            // shadows a system header), as the Clang-AST lints guess it.
            std::vector<std::string> inc;
            const fs::path root = fs::is_directory(cfg.root) ? cfg.root : cfg.root.parent_path();
            int cxx_std = 17;
            for (auto& f : astlint::compile_db_flags(root, u.path)) {
                if (!f.starts_with("-std=")) inc.push_back(f);
                else if (auto k = f.find("++"); k != std::string::npos) cxx_std = cxx_std_number(f.substr(k + 2));
            }
            if (inc.empty() && !root.empty()) {
                std::error_code ec;
                inc = {"-idirafter", root.string()};
                if (fs::is_directory(root / "include", ec)) inc.insert(inc.end(), {"-idirafter", (root / "include").string()});
            }
            if (u.header) inc.insert(inc.begin(), {"-x", "c", "-femit-all-decls"});
            const auto lt0 = std::chrono::steady_clock::now();
            progress_note("lower-start", u.path, 0.0);
            l.ir = lower_to_ir(fe, u.path, std::max(10.0, cfg.timeout), l.err, &l.folded, &l.sshl, &l.cxx_models, inc, cxx_std);
            if (!l.ir && !u.header && std::getenv("PRISM_PIR_NO_SALVAGE") == nullptr) {
                // clang reports the first error only (first_error): cut the
                // functions it names, re-lower, and repeat with the errors
                // gathered so far (the salvaged copy keeps the original file
                // name and lines through #line), a few rounds at most.
                std::string errs = l.err;
                bool hoist = true;
                for (int round = 0; round < 8; ++round) {
                    auto sv = salvage_unit(u.path, errs, hoist);
                    if (!sv) break;
                    TmpDir td;
                    const fs::path cp = td.path / u.path.filename();
                    write_file(cp, sv->text);
                    auto inc2 = inc;
                    inc2.insert(inc2.begin(), {"-iquote", u.path.parent_path().string()});
                    std::string err2;
                    Lowered l2;
                    l2.ir = lower_to_ir(fe, cp, std::max(10.0, cfg.timeout), err2, &l2.folded, &l2.sshl, &l2.cxx_models, inc2, cxx_std);
                    if (l2.ir) {
                        l.ir = std::move(l2.ir);
                        l.folded = std::move(l2.folded);
                        l.sshl = std::move(l2.sshl);
                        l.cxx_models = std::move(l2.cxx_models);
                        l.dropped = std::move(sv->dropped);
                        l.err.clear();
                        break;
                    }
                    if (errs.find(err2) != std::string::npos) {
                        // No new error: a hoisted nested function uses the
                        // outer frame. Try once more without hoisting.
                        if (!hoist) break;
                        hoist = false;
                        continue;
                    }
                    errs += "\n" + err2;
                }
            }
            progress_note("lower-done", u.path, std::chrono::duration<double>(std::chrono::steady_clock::now() - lt0).count());
            if (is_cxx(u.path) && l.cxx_models.empty() && !cxx_models_error.empty()) l.cxx_models = cxx_models_error;
        } catch (const std::exception& ex) {
            l.ir.reset();
            l.err = std::string("internal error: ") + ex.what();
        }
        }
        try {
            const auto t0 = std::chrono::steady_clock::now();
            progress_note("start", u.path, 0.0);
            an.at(i) = run_unit(u, fe, cfg, low.at(i), models, budget);
            progress_note("done", u.path, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        } catch (const std::exception& ex) {
            auto f = base_finding(u);
            f.status = std::string(laws::ERROR);
            f.strength = std::string(laws::STRENGTH_SOME);
            f.message = std::string("pir internal error: ") + ex.what();
            an.at(i).recs.clear();
            an.at(i).out = {f};
        }
        // Functions Z3 could no longer check are re-run in fresh processes,
        // sequentially, after the queue; every other unit finishes here.
        bool needs_rerun = false;
        for (auto& r : an.at(i).recs)
            if (auto it = r.f.extra.find("pir_rerun"); it != r.f.extra.end() && it->second == "pending") needs_rerun = true;
        if (needs_rerun) pending.at(i) = 1;
        else finish(i);
        prism::membudget::release_free_memory();
    });
    for (std::size_t i = 0; i < batch.size(); ++i) {
        if (!pending.at(i)) continue;
        const auto rt0 = std::chrono::steady_clock::now();
        rerun_after_oom(batch.at(i), an.at(i).recs, cfg);
        progress_note("rerun-done", batch.at(i).path, std::chrono::duration<double>(std::chrono::steady_clock::now() - rt0).count());
        finish(i);
    }
    for (std::size_t i = 0; i < per.size(); ++i) per_all.at(b0 + i) = std::move(per.at(i));
    low.clear();
    an.clear();
    prism::membudget::release_free_memory();
    }  // batch: its IR (low) and analyses are freed here
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
    std::set<std::pair<std::string, std::string>> seen;
    for (std::size_t i = 0; i < per_all.size(); ++i)
        if (!units_all.at(i).header)
            for (auto& f : per_all.at(i))
                if (f.function) seen.emplace(f.file, *f.function);
    for (std::size_t i = 0; i < per_all.size(); ++i)
        for (auto& f : per_all.at(i)) {
            if (units_all.at(i).header) {
                // only functions of the header itself that no unit reported;
                // a header that does not compile alone says nothing
                if (!f.function || f.file != units_all.at(i).rel || seen.count({f.file, *f.function})) continue;
                f.extra["lowered_as"] = "the header alone (no unit calls this function)";
            }
            out.push_back(std::move(f));
        }
    return exec_gate_note("pir", std::move(out), "pir translation validation (lli)");
}

}  // namespace prism::pir
