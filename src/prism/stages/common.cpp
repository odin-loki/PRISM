// Helpers shared by the stages (declared in common.hpp): text and regex
// helpers, findings, source lookup, argument decoding and seeds, pir_one and
// the ACSL / comment contract parser.
#include "common.hpp"
#include "prism/config.hpp"
#include "prism/pir.hpp"
#include "prism/astlint.hpp"
#include "prism/pipeline.hpp"
#include "prism/pool.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#  include <process.h>
#  define getpid _getpid
#else
#  include <unistd.h>
#endif

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace stages_detail {
std::string strip(std::string s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s.at(a)))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s.at(b - 1)))) --b;
    return s.substr(a, b - a);
}

std::string lstrip(std::string s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s.at(i)))) ++i;
    return s.substr(i);
}

std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string join_sv(const std::vector<std::string>& v, std::string_view sep) {
    std::string o;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) o += sep;
        o += v.at(i);
    }
    return o;
}

bool starts_kw(std::string_view text, std::string_view kw) {
    if (!text.starts_with(kw)) return false;
    if (text.size() == kw.size()) return true;
    unsigned char c = static_cast<unsigned char>(text[kw.size()]);
    return !(std::isalnum(c) || c == '_');
}

std::optional<Match> match_at(const Regex& re, std::string_view s) {
    auto m = re.search_match(s, 0);
    if (!m || m->spans.empty() || m->spans[0].first != 0) return std::nullopt;
    return m;
}

bool fullmatch(const Regex& re, std::string_view s) {
    auto m = match_at(re, s);
    return m && static_cast<std::size_t>(m->spans[0].second) == s.size();
}

int32_t i32(int64_t x) { return static_cast<int32_t>(x); }

bool truth(int64_t v) { return v != 0; }

Finding make_find(std::string stage, std::string_view status, const FunctionInfo& fn, std::string cls,
                  std::string msg, std::string_view strength) {
    Finding f;
    f.stage = std::move(stage);
    f.status = std::string(status);
    f.file = fn.file;
    f.function = fn.name;
    f.line = fn.line;
    f.cls = std::move(cls);
    f.message = std::move(msg);
    f.strength = std::string(strength);
    return f;
}

Finding nr(std::string stage, std::string msg, std::string strength) {
    Finding f;
    f.stage = std::move(stage);
    f.status = std::string(laws::NOTRUN);
    f.message = std::move(msg);
    f.strength = std::move(strength);
    return f;
}

std::vector<std::string> split_comma(const std::string& s) {
    std::vector<std::string> parts;
    int pdepth = 0, bdepth = 0;
    std::string cur;
    for (char ch : s) {
        if (ch == '(') ++pdepth;
        else if (ch == ')') --pdepth;
        else if (ch == '[') ++bdepth;
        else if (ch == ']') --bdepth;
        if (ch == ',' && pdepth == 0 && bdepth == 0) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    parts.push_back(cur);
    return parts;
}

std::pair<std::string, std::string> paren(std::string text) {
    text = lstrip(text);
    if (!text.starts_with("(")) throw ParseFail("expected (");
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text.at(i) == '(') ++depth;
        else if (text.at(i) == ')') {
            --depth;
            if (depth == 0) return {text.substr(1, i - 1), text.substr(i + 1)};
        }
    }
    throw ParseFail("unbalanced (");
}

std::pair<std::string, std::string> brace(std::string text) {
    text = lstrip(text);
    if (!text.starts_with("{")) throw ParseFail("expected {");
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text.at(i) == '{') ++depth;
        else if (text.at(i) == '}') {
            --depth;
            if (depth == 0) return {text.substr(1, i - 1), text.substr(i + 1)};
        }
    }
    throw ParseFail("unbalanced {");
}

std::pair<std::string, std::string> stmt(const std::string& text) {
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char ch = text.at(i);
        if (ch == '(') ++depth;
        else if (ch == ')') --depth;
        else if (ch == '{' && depth == 0) break;
        else if (ch == ';' && depth == 0) return {text.substr(0, i + 1), text.substr(i + 1)};
    }
    throw ParseFail("no semicolon in " + text.substr(0, std::min<std::size_t>(80, text.size())));
}

std::string read_text_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::optional<fs::path> locate_source(const FunctionInfo& fn) {
    if (fn.file.empty()) return std::nullopt;
    fs::path p = fn.file;
    if (fs::is_regular_file(p)) return p;
    auto cwd = fs::current_path() / p;
    if (fs::is_regular_file(cwd)) return cwd;
    // In a pipeline run fn.file is relative to the scanned root, which need
    // not be the cwd: without this, `// requires:` specs were silently not
    // found (contracts/wp/harness saw no spec) for `prism /some/tree`.
    if (auto* c = ai::session_config()) {
        std::error_code ec;
        auto base = fs::is_directory(c->root, ec) ? c->root : c->root.parent_path();
        if (fs::is_regular_file(base / p, ec)) return base / p;
    }
    fs::path name = p.filename();
    fs::path walk = fs::current_path();
    for (int i = 0; i < 6; ++i) {
        auto planted = walk / "testdata" / "planted" / name;
        if (fs::is_regular_file(planted)) return planted;
        auto td = walk / "testdata" / name;
        if (fs::is_regular_file(td)) return td;
        if (!walk.has_parent_path() || walk == walk.parent_path()) break;
        walk = walk.parent_path();
    }
    return std::nullopt;
}

std::string read_fn_source(const FunctionInfo& fn) {
    auto p = locate_source(fn);
    if (!p) return {};
    try {
        return read_text_file(*p);
    } catch (...) {
        return {};
    }
}

// gcc or clang: the callers pass gcc-style flags (-std=c11 -O0 -o), so MSVC's
// cl is not a compiler for them; without gcc/clang their step is NOTRUN.
std::optional<fs::path> which_cc() {
    Config cfg;
    return cfg.which({"gcc", "clang"});
}

std::string collapse_ws(const std::string& typ) {
    std::istringstream ss(typ);
    std::string w, out;
    while (ss >> w) {
        if (!out.empty()) out += ' ';
        out += w;
    }
    return out;
}

std::string ctype_key(std::string_view typ) {
    std::string key;
    bool sp = false;
    for (char c : typ) {
        if (c == '*' || std::isspace(static_cast<unsigned char>(c))) {
            sp = !key.empty();
            continue;
        }
        if (sp) key.push_back(' ');
        sp = false;
        key.push_back(c);
    }
    return key;
}

int param_nbytes(const std::vector<std::pair<std::string, std::string>>& params) {
    int n = 0;
    for (auto& [typ, _] : params) {
        auto it = kCTypeSize.find(ctype_key(typ));
        n += it == kCTypeSize.end() ? 4 : it->second;
    }
    return n ? n : 1;
}

Args decode_args(const FunctionInfo& fn, const std::vector<uint8_t>& data) {
    Args args;
    std::size_t off = 0;
    for (auto& [typ, name] : fn.params) {
        if (name.empty()) continue;
        auto key = ctype_key(typ);
        int sz = kCTypeSize.contains(key) ? kCTypeSize.at(key) : 4;
        std::vector<uint8_t> chunk(static_cast<std::size_t>(sz), 0);
        for (int i = 0; i < sz && off + static_cast<std::size_t>(i) < data.size(); ++i)
            chunk.at(static_cast<std::size_t>(i)) = data.at(off + static_cast<std::size_t>(i));
        bool uns = key.starts_with("uint") || key.starts_with("unsigned") || key == "size_t" || key == "uintptr_t";
        uint64_t bits = 0;
        for (int i = sz - 1; i >= 0; --i) bits = (bits << 8) | chunk.at(static_cast<std::size_t>(i));
        int64_t raw = static_cast<int64_t>(bits);
        if (!uns && sz < 8 && (raw & (int64_t{1} << (sz * 8 - 1)))) raw -= int64_t{1} << (sz * 8);
        // 8-byte values are kept whole (an unsigned one as its bit pattern).
        args[name] = sz >= 8 ? raw : i32(raw);
        off += static_cast<std::size_t>(sz);
    }
    return args;
}

std::vector<std::vector<uint8_t>> interesting_seeds(const FunctionInfo& fn) {
    int n = param_nbytes(fn.params);
    if (n <= 0) return {{0}};
    std::vector<int64_t> values{0, -1, INT_MAX_32, INT_MIN_32, 1, 4, 31, 32, 100, INT_MAX_32 - 100, INT_MAX_32 - 99};
    std::vector<std::vector<uint8_t>> out;
    std::set<std::vector<uint8_t>> seen;
    auto add = [&](std::vector<uint8_t> row) {
        row.resize(static_cast<std::size_t>(n), 0);
        if (seen.insert(row).second) out.push_back(std::move(row));
    };
    // Each value is packed at its parameter's own width (the decode_args
    // layout), so a 64-bit parameter gets the whole value.
    auto pack = [](int64_t v, int sz) {
        std::vector<uint8_t> bytes;
        for (int k = 0; k < sz; ++k)
            bytes.push_back(static_cast<uint8_t>(static_cast<uint64_t>(v) >> (8 * k)));
        return bytes;
    };
    std::vector<int> sizes;
    for (auto& [t, name] : fn.params) {
        if (name.empty()) continue;
        auto key = ctype_key(t);
        sizes.push_back(kCTypeSize.contains(key) ? kCTypeSize.at(key) : 4);
    }
    if (sizes.empty()) sizes.push_back(4);
    auto append_row = [&](const std::vector<int64_t>& per_slot) {
        std::vector<uint8_t> blob;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            auto p = pack(per_slot.at(i), sizes.at(i));
            blob.insert(blob.end(), p.begin(), p.end());
        }
        add(std::move(blob));
    };
    add(std::vector<uint8_t>(static_cast<std::size_t>(n), 0));
    add(std::vector<uint8_t>(static_cast<std::size_t>(n), 0xFF));
    for (auto v : values) append_row(std::vector<int64_t>(sizes.size(), i32(v)));
    // 64-bit extremes for the 8-byte parameters (long, long long, size_t)
    if (std::find(sizes.begin(), sizes.end(), 8) != sizes.end()) {
        for (int64_t v : {INT64_MAX_V, INT64_MIN_V, INT64_MAX_V - 100}) {
            std::vector<int64_t> per;
            for (auto sz : sizes) per.push_back(sz == 8 ? v : 0);
            append_row(per);
        }
    }
    if (sizes.size() >= 2) {
        std::vector<int64_t> a(sizes.size(), 0), b(sizes.size(), 0), c(sizes.size(), 0);
        a.at(0) = INT_MAX_32;
        b.at(1) = INT_MAX_32;
        c.at(0) = 1;
        append_row(a);
        append_row(b);
        append_row(c);
    }
    return out;
}

// One function through the pir stage: fn's file with fn's body (possibly
// instrumented: requires as an early return, ensures as assert) spliced in
// at its span, so types, globals and callees keep their context. #line keeps
// the reported lines on the original source.
const char* const kPirHookPrelude =
    "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
    "void __VERIFIER_assume(int);\n"
    "void __prism_havoc_bytes(void*, unsigned long);\n"
    "#ifdef __cplusplus\n}\n#endif\n";

std::string pir_definition(const FunctionInfo& fn) {
    // A signature with no named parameters ("int f(...)", or none at all) but
    // known params is rebuilt from them, so the body's names are declared.
    std::string sig = fn.signature;
    if (!fn.params.empty() && (sig.empty() || sig.find("(...)") != std::string::npos)) {
        sig = (fn.return_type.empty() ? std::string("int") : fn.return_type) + " " + fn.name + "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i)
            sig += (i ? ", " : "") + fn.params[i].first + " " + fn.params[i].second;
        sig += ")";
    }
    return "#line " + std::to_string(std::max(1, fn.span.first)) + "\n" + sig + " {\n" + fn.body + "\n}\n";
}

namespace {

// The unit pir_one checks: fn's file with fn's signature and body spliced in
// at its span (#line keeps the reported lines on the original source).
std::string pir_unit_text(const FunctionInfo& fn, const std::string& prelude) {
    std::vector<std::string> lines;
    if (std::ifstream in{source_path(fn.file)}; in) for (std::string l; std::getline(in, l);) lines.push_back(l);
    const int start = fn.span.first, end = fn.span.second;
    const bool ctx = start >= 1 && end >= start && static_cast<std::size_t>(end) <= lines.size();
    std::string unit = "#include <assert.h>\n" + prelude + "#line 1\n";
    if (ctx)
        for (int i = 0; i < start - 1; ++i) unit += lines[static_cast<std::size_t>(i)] + "\n";
    unit += pir_definition(fn);
    if (ctx) {
        unit += "#line " + std::to_string(end + 1) + "\n";
        for (std::size_t i = static_cast<std::size_t>(end); i < lines.size(); ++i) unit += lines[i] + "\n";
    }
    return unit;
}

// Results of pir checks already made in this process, keyed by everything
// the check reads (the unit text, the function, the options, --allow-exec):
// the same query is the same check, so it is not run twice (concolic asks
// the seed-free flip of a branch once per seed).
struct PirMemo {
    std::mutex mu;
    std::unordered_map<std::string, Finding> rows;
};
PirMemo& pir_memo() {
    static PirMemo m;
    return m;
}

// Functions the pir stage may check in a unit: the parser's count for C; for
// C++ twice that plus two (lambdas and instantiated templates are functions
// too); unknown counts as a chunk of its own.
std::size_t unit_weight(const std::string& text, const std::string& name) {
    std::size_t n = 0;
    try {
        n = extract_functions_from_text(text, name).size();
    } catch (...) {
        n = 0;
    }
    const auto ext = fs::path(name).extension().string();
    const bool c = ext == ".c";
    return n == 0 ? std::size_t{1000} : (c ? n : 2 * n + 2);
}

// collect_pir_queries: the list pir_one appends to on this thread instead of
// checking (nullptr: pir_one checks).
thread_local std::vector<PirQuery>* t_collect = nullptr;

}  // namespace

Finding pir_one(const FunctionInfo& fn, int unwind, const std::vector<std::string>& only_props,
                const std::string& prelude) {
    return pir_query(PirQuery{fn, unwind, only_props, prelude, std::nullopt});
}

Finding pir_query(const PirQuery& q) {
    if (t_collect) {
        t_collect->push_back(q);
        // A stand-in for the dry run only: never reported (the real pass
        // asks again and gets the checked row).
        return make_find("pir", laws::UNKNOWN, q.fn, "", "pir query collected, not checked yet",
                         laws::STRENGTH_READS);
    }
    return pir_many({q}, 1).at(0);
}

bool collecting_pir() { return t_collect != nullptr; }

std::vector<PirQuery> collect_pir_queries(const std::function<void()>& dry_run) {
    std::vector<PirQuery> qs;
    auto* prev = t_collect;
    t_collect = &qs;
    try {
        dry_run();
    } catch (...) {
        // a dry run that throws has still named the queries it reached
    }
    t_collect = prev;
    return qs;
}

void prefetch_pir(int jobs, const std::function<void()>& dry_run) {
    auto qs = collect_pir_queries(dry_run);
    if (!qs.empty()) (void)pir_many(qs, jobs);
}

std::vector<Finding> map_functions(int jobs, std::size_t n,
                                   const std::function<std::vector<Finding>(std::size_t)>& rows) {
    std::vector<std::vector<Finding>> per(n);
    if (n == 1 || jobs == 1) {
        for (std::size_t i = 0; i < n; ++i) per[i] = rows(i);
    } else {
        // An exception is rethrown after the loop, the lowest index's first:
        // the one a serial loop would have stopped at.
        std::vector<std::exception_ptr> err(n);
        prism::pool::for_each_index(jobs, n, [&](std::size_t i) {
            try {
                per[i] = rows(i);
            } catch (...) {
                err[i] = std::current_exception();
            }
        });
        for (auto& e : err)
            if (e) std::rethrow_exception(e);
    }
    std::vector<Finding> out;
    for (auto& v : per)
        for (auto& f : v) out.push_back(std::move(f));
    return out;
}

// One pir run (pir::run_pir) checks many units: its fixed cost (library
// models, C++ model headers, the run's set-up) is paid once per chunk, not
// once per query. Each query is its own unit in its own subdirectory of the
// run's root, checked exactly as pir_one checks it alone: pir lowers,
// analyses and validates every unit on its own. The one thing a run shares
// is its Houdini budget (max(3600 s, 120 x timeout)); a chunk holds at most
// 12 functions, each of which spends at most its function budget
// (max(300 s, 10 x timeout)) there, so no function of a chunk gets less of
// that budget than it would alone. With PRISM_HOUDINI_BUDGET or
// PRISM_FUNCTION_BUDGET set, every query is a run of its own.
std::vector<Finding> pir_many(const std::vector<PirQuery>& qs, int jobs) {
    std::vector<Finding> res(qs.size());
    struct Item {
        std::string key, unit, name;   // name: the unit's file name
        std::vector<std::size_t> idx;  // the queries asking exactly this
        std::size_t weight = 1;
    };
    const bool allowed = sandbox::allowed();
    std::vector<Item> items;
    {
        std::unordered_map<std::string, std::size_t> pos;
        for (std::size_t i = 0; i < qs.size(); ++i) {
            const auto& q = qs[i];
            const auto ext = fs::path(q.fn.file).extension().string();
            Item it;
            it.unit = q.unit ? *q.unit : pir_unit_text(q.fn, q.prelude);
            it.name = fs::path(q.fn.file).stem().string() + (ext.empty() ? std::string(".c") : ext);
            it.key = std::to_string(q.unwind) + '\0' + (allowed ? "x" : "-") + '\0' + q.fn.file + '\0' +
                     q.fn.name + '\0' + std::to_string(q.fn.line) + '\0' + it.name + '\0';
            for (auto& p : q.only_props) it.key += p + '\1';
            it.key += '\0' + q.prelude + '\0' + it.unit;
            {
                std::lock_guard<std::mutex> lk(pir_memo().mu);
                if (auto m = pir_memo().rows.find(it.key); m != pir_memo().rows.end()) {
                    res[i] = m->second;
                    continue;
                }
            }
            if (auto p = pos.find(it.key); p != pos.end()) {
                items[p->second].idx.push_back(i);
                continue;
            }
            it.idx.push_back(i);
            pos.emplace(it.key, items.size());
            items.push_back(std::move(it));
        }
    }
    if (items.empty()) return res;
    const bool solo = std::getenv("PRISM_HOUDINI_BUDGET") || std::getenv("PRISM_FUNCTION_BUDGET");
    constexpr std::size_t kChunkFunctions = 12;
    // Chunks of items with the same options (one run has one unwind and one
    // property filter), in query order.
    std::vector<std::vector<std::size_t>> chunks;
    {
        std::map<std::pair<int, std::vector<std::string>>, std::size_t> open;  // options -> open chunk
        std::vector<std::size_t> load;
        for (std::size_t k = 0; k < items.size(); ++k) {
            const auto& q = qs[items[k].idx.front()];
            items[k].weight = solo ? kChunkFunctions + 1 : unit_weight(items[k].unit, items[k].name);
            auto opt = std::make_pair(q.unwind, q.only_props);
            auto o = open.find(opt);
            if (o == open.end() || load[o->second] + items[k].weight > kChunkFunctions) {
                chunks.emplace_back();
                load.push_back(0);
                o = open.insert_or_assign(opt, chunks.size() - 1).first;
            }
            chunks[o->second].push_back(k);
            load[o->second] += items[k].weight;
        }
    }
    static std::atomic<unsigned> seq{0};
    auto run_chunk = [&](std::size_t c) {
        const auto& chunk = chunks[c];
        const auto& q0 = qs[items[chunk.front()].idx.front()];
        const auto dir = fs::temp_directory_path() /
                         ("prism_pir_one_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
        std::vector<fs::path> srcs;
        std::vector<std::string> rels;
        for (std::size_t j = 0; j < chunk.size(); ++j) {
            const auto sub = dir / ("q" + std::to_string(j));
            fs::create_directories(sub);
            srcs.push_back(sub / items[chunk[j]].name);
            std::error_code rec;
            auto rel = fs::relative(srcs.back(), dir, rec);  // as pir names the unit
            rels.push_back(rec ? srcs.back().filename().string() : rel.generic_string());
            std::ofstream(srcs.back()) << items[chunk[j]].unit;
        }
        auto cfg = default_config();
        cfg.root = dir;
        cfg.jobs = 1;
        cfg.unwind = q0.unwind;
        cfg.pir_only_props = q0.only_props;
        auto rows = pir::run_pir(srcs, cfg);
        std::error_code ec;
        fs::remove_all(dir, ec);
        // Rows of no unit (models error, z3 not built, no front end) belong
        // to every query. The --allow-exec note is made again per unit: a
        // run of that unit alone writes it only when that unit needs it.
        std::set<std::string> unit_rels(rels.begin(), rels.end());
        std::vector<Finding> shared;
        std::map<std::string, std::vector<Finding>> per;
        for (auto& f : rows) {
            if (unit_rels.contains(f.file)) {
                per[f.file].push_back(std::move(f));
                continue;
            }
            auto r = f.extra.find("reason");
            if (f.status == laws::NOTRUN && r != f.extra.end() && r->second == sandbox::EXEC_REASON) continue;
            shared.push_back(std::move(f));
        }
        for (std::size_t j = 0; j < chunk.size(); ++j) {
            auto& item = items[chunk[j]];
            const auto& fn = qs[item.idx.front()].fn;
            auto mine = shared;
            for (auto& f : per[rels[j]]) mine.push_back(f);
            mine = exec_gate_note("pir", std::move(mine), "pir translation validation (lli)");
            std::optional<Finding> out, stage_row;
            for (auto& f : mine) {
                if (f.function && *f.function == fn.name) {
                    out = f;
                    break;
                }
                // A whole-stage row (z3 not built, no front end) applies to every
                // function: keep its NOTRUN, never turn it into "no result" ERROR.
                if (!f.function && !stage_row && !laws::is_proof(f.status)) stage_row = f;
            }
            if (!out && stage_row) {
                out = std::move(stage_row);
                out->function = fn.name;
            }
            Finding got;
            if (!out) {
                got = make_find("pir", laws::ERROR, fn, "", "pir produced no result for the function",
                                laws::STRENGTH_READS);
            } else {
                got = std::move(*out);
                got.file = fn.file;
                if (!got.line) got.line = fn.line;
            }
            {
                std::lock_guard<std::mutex> lk(pir_memo().mu);
                auto& memo = pir_memo().rows;
                if (memo.size() >= 16384) memo.clear();
                memo.insert_or_assign(item.key, got);
            }
            for (auto i : item.idx) res[i] = got;
        }
    };
    if (chunks.size() == 1 || jobs == 1) {
        for (std::size_t c = 0; c < chunks.size(); ++c) run_chunk(c);
    } else {
        std::vector<std::exception_ptr> err(chunks.size());
        prism::pool::for_each_index(jobs, chunks.size(), [&](std::size_t c) {
            try {
                run_chunk(c);
            } catch (...) {
                err[c] = std::current_exception();
            }
        });
        for (auto& e : err)
            if (e) std::rethrow_exception(e);
    }
    return res;
}

}  // namespace stages_detail

namespace {
void parse_acsl_body(std::string body, Spec& spec) {
    auto pos = body.find("\\result");
    while (pos != std::string::npos) {
        body.replace(pos, 7, "result");
        pos = body.find("\\result", pos + 6);
    }
    std::istringstream ss(body);
    std::string part;
    // (?i) is local to this keyword pattern, not a CASELESS compile of the
    // other patterns in this file. The value subpattern has no cased letters.
    static Regex kw("(?i)^(requires|ensures|invariant|decreases|assigns)\\s+(.+)$");
    while (std::getline(ss, part, ';')) {
        part = strip(part);
        if (part.empty()) continue;
        auto m = match_at(kw, part);
        if (!m) continue;
        auto kind = lower_copy(m->group(1));
        if (kind == "assigns") continue;
        auto val = strip(m->group(2));
        if (kind == "requires") spec.requires_.push_back(val);
        else if (kind == "ensures") spec.ensures.push_back(val);
        else if (kind == "invariant") spec.invariant.push_back(val);
        else if (kind == "decreases") spec.decreases.push_back(val);
    }
}

std::vector<std::string> extract_acsl_blocks(const std::string& text) {
    std::vector<std::string> bodies;
    std::size_t i = 0;
    while (i < text.size()) {
        auto start = text.find("/*@", i);
        if (start == std::string::npos) break;
        auto end = text.find("*/", start + 3);
        if (end == std::string::npos) break;
        bodies.push_back(text.substr(start + 3, end - (start + 3)));
        i = end + 2;
    }
    return bodies;
}

std::string acsl_preamble(const std::vector<std::string>& lines, int body_start) {
    int i = body_start - 1;
    bool in_block = false;
    while (i >= 0) {
        auto raw = lines.at(static_cast<std::size_t>(i));
        auto s = strip(raw);
        if (in_block) {
            if (raw.find("/*@") != std::string::npos || s.starts_with("/*")) in_block = false;
            --i;
            continue;
        }
        if (s.empty()) {
            --i;
            continue;
        }
        if (s.starts_with("//")) {
            --i;
            continue;
        }
        if (s.ends_with("*/") || s.starts_with("/*") || s.starts_with("*")) {
            if (raw.find("/*") != std::string::npos && raw.find("*/") != std::string::npos) {
                --i;
                continue;
            }
            in_block = true;
            --i;
            continue;
        }
        break;
    }
    std::string out;
    for (int j = i + 1; j < body_start && j < static_cast<int>(lines.size()); ++j) {
        if (!out.empty()) out += '\n';
        out += lines.at(static_cast<std::size_t>(j));
    }
    return out;
}

}  // namespace

namespace stages_detail {
Spec parse_comments(const FunctionInfo& fn) {
    Spec spec;
    auto text = read_fn_source(fn);
    if (text.empty()) return spec;
    std::vector<std::string> lines;
    {
        std::istringstream ss(text);
        std::string ln;
        while (std::getline(ss, ln)) lines.push_back(ln);
    }
    int start = std::max(0, (fn.line ? fn.line : 1) - 2);
    int end = (fn.span.second ? fn.span.second : (fn.line ? fn.line : 1) + 40);
    end = std::min(static_cast<int>(lines.size()), end);
    int fn_line = fn.line ? fn.line : 1;
    int body_start = std::max(0, fn_line - 1);
    for (auto& b : extract_acsl_blocks(acsl_preamble(lines, body_start))) parse_acsl_body(b, spec);
    std::string region_body;
    for (int i = body_start; i < end; ++i) {
        if (!region_body.empty()) region_body += '\n';
        region_body += lines.at(static_cast<std::size_t>(i));
    }
    for (auto& b : extract_acsl_blocks(region_body)) parse_acsl_body(b, spec);
    // `// Ensures:` and `/*@ Requires ... */` are clauses too. (?i) is on this
    // pattern only; kind is folded after the match.
    static Regex clause("(?i)(?://|/\\*|\\*)\\s*(requires|ensures|invariant|decreases|diff)\\s*:\\s*(.+?)(?:\\*/)?\\s*$");
    for (int i = start; i < end; ++i) {
        auto ln = lines.at(static_cast<std::size_t>(i));
        auto stripped = strip(ln);
        if (stripped.starts_with("//@")) {
            parse_acsl_body(stripped.substr(3), spec);
            continue;
        }
        auto m = clause.search_match(ln);
        if (!m) continue;
        auto kind = lower_copy(m->group(1));
        auto val = strip(m->group(2));
        if (val.ends_with("*/")) val = strip(val.substr(0, val.size() - 2));
        if (kind == "diff") {
            auto sp = val.find(' ');
            spec.diff = sp == std::string::npos ? val : val.substr(0, sp);
            if (spec.diff->empty()) spec.diff.reset();
        } else if (kind == "requires") spec.requires_.push_back(val);
        else if (kind == "ensures") spec.ensures.push_back(val);
        else if (kind == "invariant") spec.invariant.push_back(val);
        else if (kind == "decreases") spec.decreases.push_back(val);
    }
    return spec;
}

}  // namespace stages_detail

Finding check_function_pir(const FunctionInfo& fn, int unwind) {
    return stages_detail::pir_one(fn, unwind, {}, stages_detail::kPirHookPrelude);
}

int cxx_std_year(std::string_view flag) {
    if (flag.starts_with("-std=")) flag.remove_prefix(5);
    if (flag.starts_with("gnu++")) flag.remove_prefix(5);
    else if (flag.starts_with("c++")) flag.remove_prefix(3);
    else return 0;
    static const std::map<std::string, int, std::less<>> years = {
        {"98", 3}, {"03", 3}, {"0x", 11}, {"11", 11}, {"1y", 14}, {"14", 14}, {"1z", 17}, {"17", 17},
        {"2a", 20}, {"20", 20}, {"2b", 23}, {"23", 23}, {"2c", 26}, {"26", 26}};
    auto it = years.find(flag);
    return it == years.end() ? 0 : it->second;
}

std::vector<FunctionInfo> with_cxx_std(std::vector<FunctionInfo> functions, const std::filesystem::path& root) {
    std::error_code ec;
    const bool dir = std::filesystem::is_directory(root, ec);
    std::map<std::string, int> seen;
    for (auto& fn : functions) {
        auto it = seen.find(fn.file);
        if (it == seen.end()) {
            int year = 0;
            auto src = dir ? root / fn.file : root;
            for (auto& f : astlint::compile_db_flags(root, src))
                if (f.starts_with("-std=")) year = cxx_std_year(f);  // the last one wins, as in the compiler
            it = seen.emplace(fn.file, year).first;
        }
        fn.cxx_std = it->second;
    }
    return functions;
}

}  // namespace prism
