// Whole-function verdict cache: see fncache.hpp.
#include "fncache.hpp"

#include "prism/cache.hpp"
#include "prism/laws.hpp"
#include "prism/solver.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
extern char** environ;
#endif

namespace prism::pir::fncache {
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// Bumped whenever the entry format or the meaning of a stored verdict changes.
constexpr int kSchema = 1;

void put_arg(std::ostringstream& o, const Arg& a) {
    o << (a.is_const ? 'c' : 'v') << a.width << ':' << (a.is_const ? a.bits : static_cast<uint64_t>(a.var)) << ' ';
}

void put_str(std::ostringstream& o, const std::string& s) { o << s.size() << '"' << s << '"'; }

void put_strs(std::ostringstream& o, const std::vector<std::string>& v) {
    o << '[' << v.size();
    for (const auto& s : v) {
        o << ' ';
        put_str(o, s);
    }
    o << ']';
}

// The file the engine's code was loaded from (this translation unit's).
fs::path engine_file() {
#if defined(__linux__)
    const auto here = reinterpret_cast<std::uintptr_t>(&engine_file);
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        // "lo-hi perms offset dev inode path"
        std::istringstream ls(line);
        std::string range, perms, off, dev, inode, path;
        ls >> range >> perms >> off >> dev >> inode;
        std::getline(ls, path);
        const auto dash = range.find('-');
        if (dash == std::string::npos) continue;
        const auto lo = std::strtoull(range.substr(0, dash).c_str(), nullptr, 16);
        const auto hi = std::strtoull(range.substr(dash + 1).c_str(), nullptr, 16);
        if (here < lo || here >= hi) continue;
        path.erase(0, path.find_first_not_of(' '));
        if (path.empty() || path.front() != '/') return {};
        return path;
    }
    return {};
#elif defined(__unix__) || defined(__APPLE__)
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&engine_file), &info) == 0 || !info.dli_fname) return {};
    std::error_code ec;
    auto p = fs::absolute(info.dli_fname, ec);
    return ec ? fs::path() : p;
#else
    return {};
#endif
}

// BLAKE3 of the engine binary, once per process ("" when unknown: cache off).
const std::string& engine_digest() {
    static std::once_flag once;
    static std::string digest;
    std::call_once(once, [] {
        const auto p = engine_file();
        if (p.empty()) return;
        std::ifstream in(p, std::ios::binary);
        if (!in) return;
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.empty()) return;
        digest = prism::cache::key_of(bytes);
    });
    return digest;
}

// Every PRISM_* variable and value, sorted.
std::string prism_environment() {
    std::vector<std::string> vars;
#if defined(__unix__) || defined(__APPLE__)
    for (char** e = environ; e && *e; ++e)
        if (std::strncmp(*e, "PRISM_", 6) == 0) vars.emplace_back(*e);
#endif
    std::sort(vars.begin(), vars.end());
    std::ostringstream o;
    put_strs(o, vars);
    return o.str();
}

bool enabled() {
    const char* e = std::getenv("PRISM_PIR_FN_CACHE");
    return !(e && (std::strcmp(e, "0") == 0 || std::strcmp(e, "off") == 0));
}

fs::path cache_dir(const CheckOptions& opt) {
    const fs::path root = opt.cache_dir.empty() ? fs::path(solver::default_cache_dir()) : fs::path(opt.cache_dir);
    return root / "pir-fn";
}

}  // namespace

std::string fingerprint(const Function& fn) {
    std::ostringstream o;
    o << "fn ";
    put_str(o, fn.name);
    put_str(o, fn.ir_name);
    put_str(o, fn.file);
    o << " line " << fn.line << " ret " << fn.ret_width << " nondet " << fn.nondet << " mem " << fn.uses_memory
      << " mglob " << fn.mutable_globals << " rptr " << fn.returns_ptr << " ptr " << fn.ptr.w << ',' << fn.ptr.obj_shift
      << ',' << fn.ptr.max_obj_size << ',' << fn.ptr.max_objects << ',' << fn.ptr.fn_obj_base << '\n';
    o << "inlined ";
    put_strs(o, fn.inlined);
    o << " ptr_params ";
    put_strs(o, fn.ptr_params);
    o << " assumptions ";
    put_strs(o, fn.assumptions);
    o << " throws ";
    put_strs(o, fn.throws);
    o << " libm ";
    put_strs(o, fn.libm);
    o << "\nloops";
    for (const auto& [b, lc] : fn.loop_locs) o << ' ' << b << ':' << lc.first << ':' << lc.second;
    o << "\nvars " << fn.vars.size() << '\n';
    for (const auto& v : fn.vars) {
        put_str(o, v.name);
        o << ' ' << v.width << ' ' << v.fp << '\n';
    }
    o << "params";
    for (int p : fn.params) o << ' ' << p;
    o << "\nblocks " << fn.blocks.size() << '\n';
    for (const auto& b : fn.blocks) {
        o << "block ";
        put_str(o, b.name);
        o << '\n';
        for (const auto& p : b.phis) {
            o << " phi " << p.dst;
            for (const auto& [pred, a] : p.in) {
                o << " [" << pred << ' ';
                put_arg(o, a);
                o << ']';
            }
            o << '\n';
        }
        for (const auto& s : b.stmts) {
            o << " s " << static_cast<int>(s.kind) << ' ' << s.dst << ' ' << static_cast<int>(s.op) << ' ' << s.uninit
              << s.nondet << s.nondet_unsigned << ' ' << s.col << ' ' << s.line << ' ' << static_cast<int>(s.mkind)
              << ' ' << s.init << ' ' << s.align << ' ' << s.tag << ' ' << s.dst2 << ' ' << s.dst3 << ' ' << s.ptr_arith
              << ' ';
            put_str(o, s.nondet_fn);
            put_str(o, s.prop);
            put_str(o, s.cls);
            put_str(o, s.msg);
            o << " (";
            for (const auto& a : s.args) put_arg(o, a);
            o << ")\n";
        }
        const auto& t = b.term;
        o << " term " << static_cast<int>(t.kind) << ' ' << t.t << ' ' << t.f << ' ';
        put_arg(o, t.cond);
        if (t.val) put_arg(o, *t.val);
        else o << '-';
        o << '\n';
    }
    return o.str();
}

std::string key(const Function& fn, const CheckOptions& opt) {
    if (!enabled() || !opt.use_cache || opt.certified || !opt.only_props.empty() || opt.debug_before_query) return {};
    const auto& eng = engine_digest();
    if (eng.empty()) return {};
    std::ostringstream o;
    o << "prism-pir-fn " << kSchema << " engine " << eng << " z3 ";
#ifdef PRISM_HAS_Z3
    o << Z3_get_full_version();
#endif
    o << "\nunwind " << opt.unwind << " deepen " << opt.deepen_max << " timeout " << opt.timeout_s << " fnbudget "
      << opt.function_budget_s << " portfolio " << opt.portfolio << " mem " << static_cast<int>(opt.encode.memory)
      << " houdini_budget " << (opt.houdini_budget != nullptr) << "\nenv " << prism_environment() << '\n'
      << fingerprint(fn);
    return prism::cache::key_of(o.str());
}

bool storable(const Verdict& v) {
    static const std::string ok[] = {std::string(laws::PROVED),  std::string(laws::PROVED_UNBOUNDED),
                                     std::string(laws::PROVED_ASSUMING), std::string(laws::BOUNDED),
                                     std::string(laws::FAILED),  std::string(laws::NEEDS_HARNESS)};
    if (std::find(std::begin(ok), std::end(ok), v.status) == std::end(ok)) return false;
    // cut by a clock: a fresh check with a fresh budget may say more
    for (const auto& [k, val] : v.extra) {
        if (k.find("budget") != std::string::npos) return false;
        if (val.find("budget") != std::string::npos) return false;
        if (val.find("memory") != std::string::npos && k.find("note") != std::string::npos) return false;
    }
    if (v.message.find("budget") != std::string::npos) return false;
    return true;
}

std::optional<Verdict> get(const CheckOptions& opt, const std::string& k) {
    if (k.empty()) return std::nullopt;
    auto txt = prism::cache::get(cache_dir(opt), k);
    if (!txt) return std::nullopt;
    try {
        auto j = json::parse(*txt);
        if (j.value("schema", 0) != kSchema || j.value("key", "") != k) return std::nullopt;
        Verdict v;
        v.status = j.at("status").get<std::string>();
        v.message = j.at("message").get<std::string>();
        v.cls = j.at("cls").get<std::string>();
        v.prop = j.at("prop").get<std::string>();
        v.line = j.at("line").get<int>();
        v.cex = j.at("cex").get<std::map<std::string, uint64_t>>();
        v.cex_args = j.at("cex_args").get<std::vector<uint64_t>>();
        v.extra = j.at("extra").get<std::map<std::string, std::string>>();
        if (!storable(v)) return std::nullopt;  // never serve what would not have been stored
        v.extra["verdict_cache"] = "hit " + k.substr(0, 16);
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

void put(const CheckOptions& opt, const std::string& k, const Verdict& v) {
    if (k.empty() || !storable(v)) return;
    json j{{"schema", kSchema}, {"key", k},          {"status", v.status},     {"message", v.message},
           {"cls", v.cls},      {"prop", v.prop},    {"line", v.line},         {"cex", v.cex},
           {"cex_args", v.cex_args}, {"extra", v.extra}};
    (void)prism::cache::put(cache_dir(opt), k, j.dump());
}

}  // namespace prism::pir::fncache
