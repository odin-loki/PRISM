// Library operational models (roadmap 2.6, docs/PIR.md "Library models").
//
// The models are C++ sources (src/prism/pir/models/libc/*.cpp, extern "C"
// libc API) embedded in the
// binary at build time. Once per run they are lowered by the same clang ->
// opt pipeline as scanned code (plus -fno-builtin -ffreestanding so clang
// does not replace a model by the builtin it models), and every function a
// unit declares but does not define is linked into that unit's module
// (transitively, with the globals it uses). Linking happens on the parsed
// ir::Module (the models' metadata references are renamed so they cannot
// collide with the unit's), so no llvm-link run is needed.

#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include "../proc.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>

namespace prism::pir {

// generated at configure time from src/prism/pir/models (CMakeLists.txt)
const std::vector<std::pair<std::string, std::string>>& embedded_models();

const std::vector<std::pair<std::string, std::string>>& model_sources() { return embedded_models(); }

bool write_cxx_models(const std::filesystem::path& dir) {
    std::error_code ec;
    for (auto& [name, text] : model_sources()) {
        if (!name.starts_with("cxx/")) continue;
        auto dst = dir / name.substr(4);
        std::filesystem::create_directories(dst.parent_path(), ec);
        std::ofstream o(dst, std::ios::binary);
        o << text;
        if (!o) return false;
    }
    return true;
}

bool source_imports_std(const std::string& t) {
    // Skip comments and string/char literals; match `import std` followed by
    // optional whitespace and ';' or '.compat'.
    std::size_t i = 0;
    const std::size_t n = t.size();
    bool line_start = true;
    while (i < n) {
        const char c = t[i];
        if (c == '/' && i + 1 < n && t[i + 1] == '/') { while (i < n && t[i] != '\n') ++i; continue; }
        if (c == '/' && i + 1 < n && t[i + 1] == '*') {
            const auto e = t.find("*/", i + 2);
            i = e == std::string::npos ? n : e + 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            ++i;
            while (i < n && t[i] != c) { if (t[i] == '\\') ++i; ++i; }
            ++i;
            line_start = false;
            continue;
        }
        if (c == '\n') { line_start = true; ++i; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        if (line_start) {
            std::size_t j = i;
            if (t.compare(j, 7, "export ") == 0) j += 7;
            while (j < n && (t[j] == ' ' || t[j] == '\t')) ++j;
            if (t.compare(j, 6, "import") == 0) {
                j += 6;
                const std::size_t ws = j;
                while (j < n && (t[j] == ' ' || t[j] == '\t')) ++j;
                if (j > ws && t.compare(j, 3, "std") == 0) {
                    j += 3;
                    if (t.compare(j, 7, ".compat") == 0) j += 7;
                    while (j < n && (t[j] == ' ' || t[j] == '\t')) ++j;
                    if (j < n && t[j] == ';') return true;
                }
            }
        }
        line_start = false;
        ++i;
    }
    return false;
}

bool build_cxx_std_module(const std::filesystem::path& clangxx, const std::filesystem::path& models_dir,
                          const std::string& cxx_std, const std::vector<std::string>& extra_flags,
                          const std::filesystem::path& out_pcm, double timeout_s, std::string& err) {
    const auto src = models_dir / "std.cppm";
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) { err = "std.cppm model missing in " + models_dir.string(); return false; }
    std::vector<std::string> argv{clangxx.string(), "-std=" + (cxx_std.empty() ? std::string("c++23") : cxx_std),
                                  "-isystem", models_dir.string()};
    argv.insert(argv.end(), extra_flags.begin(), extra_flags.end());
    // Lowering runs units in parallel: build to a private name, then rename
    // (atomic on one file system), so no unit reads a half-written module.
    static std::atomic<unsigned> seq{0};
    const auto tmp = out_pcm.string() + ".tmp" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
                     "." + std::to_string(seq.fetch_add(1));
    argv.insert(argv.end(), {"-w", "--precompile", "-o", tmp, src.string()});
    auto r = detail::run_process(argv, timeout_s);
    if (!r.failed && !r.timed_out && r.rc == 0) {
        std::filesystem::rename(tmp, out_pcm, ec);
        if (ec) { err = "std module rename failed: " + ec.message(); return false; }
        return true;
    }
    std::filesystem::remove(tmp, ec);
    {
        err = r.timed_out ? "std module precompile timed out" : "std module precompile failed: " + r.text.substr(0, 400);
        return false;
    }
    return true;
}

namespace {

namespace fs = std::filesystem;

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string first_error_line(const std::string& text) {
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
        if (line.find("error") != std::string::npos) return line;
    return text.substr(0, 200);
}

// Rename the model's private globals (@.str...) so they cannot clash with the unit's.
std::string rename_privates(std::string text, const std::string& tag) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text.compare(i, 5, "@.str") == 0) {
            out += "@.str.prism_" + tag;
            i += 4;
            continue;
        }
        out.push_back(text.at(i));
    }
    return out;
}

void refs_of(const ir::Value& v, std::set<std::string>& globals) {
    if (v.kind == ir::Value::Global) globals.insert(v.name);
    for (auto& e : v.elems) refs_of(e.v, globals);
}

bool is_lowered_libc_model(const std::string& name) {
    return name.ends_with(".cpp") && !name.starts_with("cxx/");
}

std::string model_unit_tag(const std::string& name) {
    const auto dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

}  // namespace

namespace {

// Where lowered models are kept across runs: $XDG_CACHE_HOME/prism/pir-models
// or ~/.cache/prism/pir-models ("" when neither is set).
fs::path model_cache_root() {
#ifdef _WIN32
    if (const char* l = std::getenv("LOCALAPPDATA"); l && *l) return fs::path(l) / "prism" / "pir-models";
#endif
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return fs::path(x) / "prism" / "pir-models";
    if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h) / ".cache" / "prism" / "pir-models";
    return {};
}

// One model file through clang -> opt; "" on success, else why not.
std::string lower_model(const Frontend& fe, const fs::path& dir, const std::string& name, double timeout_s,
                        std::string& ll) {
    auto src = dir / ("prism_model_" + name);
    auto o0 = dir / (name + ".o0.ll"), o1 = dir / (name + ".ll");
    std::vector<std::string> argv{fe.clang->string(), "-S"};
    for (auto& t : lp64_target_flags()) argv.push_back(t);
    argv.insert(argv.end(), {"-emit-llvm", "-O0", "-Xclang", "-disable-O0-optnone", "-fno-builtin", "-ffreestanding",
                             "-fno-discard-value-names", "-gline-tables-only", "-std=c++23", "-w", "-o", o0.string(),
                             src.string()});
    auto r = detail::run_process(argv, timeout_s);
    if (r.timed_out) return "model " + name + ": clang timed out after " + std::to_string(int(timeout_s)) + " s";
    if (r.failed || r.rc != 0) return "model " + name + ": " + first_error_line(r.text);
    auto ro = detail::run_process({fe.opt->string(), "-passes=mem2reg,lower-switch,loop-simplify,lcssa,instnamer", "-S",
                                   "-o", o1.string(), o0.string()},
                                  timeout_s);
    if (ro.timed_out) return "model " + name + " (opt): timed out after " + std::to_string(int(timeout_s)) + " s";
    if (ro.failed || ro.rc != 0) return "model " + name + " (opt): " + first_error_line(ro.text);
    ll = slurp(o1);
    if (ll.empty()) return "model " + name + ": opt wrote no IR";
    return {};
}

std::size_t model_source_lowered_count() {
    std::size_t n = 0;
    for (auto& [name, _] : model_sources())
        if (is_lowered_libc_model(name)) ++n;
    return n;
}

bool cached_model_ir_ok(const std::string& ll) {
    if (ll.empty()) return false;
#ifdef _WIN32
    const std::string triple_line = lp64_llvm_triple_metadata_line();
    if (ll.find(triple_line) == std::string::npos) return false;
    if (ll.find("target triple = \"x86_64-pc-windows") != std::string::npos) return false;
    if (ll.find("target datalayout = ") == std::string::npos) return false;
    // Native Windows clang IR uses e-m:w- (LLP64); reject pre-LP64 model caches.
    if (ll.find("e-m:w-") != std::string::npos) return false;
#endif
    return true;
}

// Every embedded libc .cpp model must be present and pass cached_model_ir_ok.
bool try_load_model_cache(const fs::path& cache, std::vector<std::pair<std::string, std::string>>& out) {
    out.clear();
    std::error_code ec;
    if (!fs::is_directory(cache, ec)) return false;
    for (auto& [name, _] : model_sources()) {
        if (!is_lowered_libc_model(name)) continue;
        auto ll = slurp(cache / (name + ".ll"));
        if (!cached_model_ir_ok(ll)) return false;
        out.emplace_back(name, std::move(ll));
    }
    return out.size() == model_source_lowered_count() && !out.empty();
}

}  // namespace

// Lowered once per run and reused by every unit. The lowered IR is also kept
// on disk, keyed by the model sources and the front end, so later runs (and
// runs on a loaded machine) do not lower the models again. A clang/opt that
// times out is retried once with twice the time; if the models still cannot
// be built, lib.error says why and the pir stage records it (never silent).
ModelLibrary build_models(const Frontend& fe, double timeout_s) {
    ModelLibrary lib;
    if (!fe.clang || !fe.opt) {
        lib.error = "clang/opt not found";
        return lib;
    }
#ifdef _WIN32
    std::string key_text = "prism-pir-models-v5\n" + fe.version + "\n" + fe.clang->string() + "\n" +
                           fe.opt->string() + "\n" + lp64_target_triple() + "\n";
#else
    std::string key_text = "prism-pir-models-v5\n" + fe.version + "\n" + fe.clang->string() + "\n" + fe.opt->string();
#endif
    for (auto& [name, text] : model_sources())
        if (!name.starts_with("cxx/")) key_text += "\n--" + name + "\n" + text;
    std::error_code ec;
    fs::path cache;
    if (auto root = model_cache_root(); !root.empty()) {
        cache = root / sha256_hex(key_text).substr(0, 24);
#ifdef _WIN32
        // Drop pre-LP64 or partial trees (v1/v2 keys, bad triples) without manual cache delete.
        if (fs::is_directory(root, ec)) {
            for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); ++it) {
                if (!it->is_directory(ec)) continue;
                if (it->path() == cache) continue;
                std::vector<std::pair<std::string, std::string>> probe;
                if (!try_load_model_cache(it->path(), probe)) fs::remove_all(it->path(), ec);
            }
        }
#endif
    }
    std::vector<std::pair<std::string, std::string>> lowered;  // model file -> IR
    if (!cache.empty() && fs::is_directory(cache, ec)) {
        if (!try_load_model_cache(cache, lowered)) {
            fs::remove_all(cache, ec);
            lowered.clear();
        }
    }
    if (lowered.empty()) {
        static std::atomic<unsigned> seq{0};
        std::random_device rd;
        auto dir = fs::temp_directory_path() / ("prism_models_" + std::to_string(rd()) + "_" + std::to_string(seq++));
        fs::create_directories(dir, ec);
        if (ec) {
            lib.error = "cannot create a temporary directory";
            return lib;
        }
        for (auto& [name, text] : model_sources()) {
            if (name.starts_with("cxx/")) continue;  // C++ headers: write_cxx_models
            auto dst = dir / (name.ends_with(".h") ? name : "prism_model_" + name);
            std::ofstream(dst, std::ios::binary) << text;
        }
        for (auto& [name, text] : model_sources()) {
            if (!is_lowered_libc_model(name)) continue;
            std::string ll;
            auto err = lower_model(fe, dir, name, timeout_s, ll);
            if (!err.empty() && err.find("timed out") != std::string::npos)
                err = lower_model(fe, dir, name, 2 * timeout_s, ll);  // a loaded machine: once more
            if (!err.empty()) {
                lib.error = err;
                break;
            }
            lowered.emplace_back(name, std::move(ll));
        }
        fs::remove_all(dir, ec);
        if (!lib.error.empty()) return lib;
        if (!cache.empty()) {
            // write-then-rename: concurrent runs never read a partial file
            fs::create_directories(cache, ec);
            for (auto& [name, ll] : lowered) {
                auto tmp = cache / (name + ".ll.tmp" + std::to_string(rd()));
                {
                    std::ofstream o(tmp, std::ios::binary);
                    o << ll;
                    if (!o) continue;
                }
                fs::rename(tmp, cache / (name + ".ll"), ec);
                if (ec) fs::remove(tmp, ec);
            }
        }
    }
    for (auto& [name, ll] : lowered) {
        auto tag = model_unit_tag(name);
        lib.units.push_back(ir::parse_module(rename_privates(ll, tag)));
    }
    return lib;
}

std::vector<std::string> link_models(ir::Module& m, const ModelLibrary& lib) {
    std::vector<std::string> linked;
    if (lib.units.empty()) return linked;
    std::set<std::string> defined;
    for (auto& f : m.functions) defined.insert(f.name);
    std::deque<std::string> want(m.declarations.begin(), m.declarations.end());
    // the printf-family model checks %s arguments with the strlen model
    for (auto* f : {"printf", "fprintf", "dprintf", "sprintf", "snprintf"})
        if (std::find(m.declarations.begin(), m.declarations.end(), f) != m.declarations.end()) {
            want.push_back("strlen");
            break;
        }
    int uniq = 0;
    while (!want.empty()) {
        auto name = want.front();
        want.pop_front();
        if (defined.count(name)) continue;
        const ir::Function* src = nullptr;
        const ir::Module* unit = nullptr;
        for (auto& u : lib.units)
            if (auto* f = u.find(name)) {
                src = f;
                unit = &u;
                break;
            }
        if (!src) continue;
        defined.insert(name);
        ir::Function f = *src;
        f.is_model = true;
        // metadata references are per module: give them fresh keys
        const std::string pre = "!prism.model." + std::to_string(uniq++) + ".";
        auto remap = [&](std::string& ref) {
            if (ref.empty()) return;
            auto key = pre + ref.substr(1);
            if (auto it = unit->locs.find(ref); it != unit->locs.end()) m.locs[key] = it->second;
            if (auto it = unit->subprograms.find(ref); it != unit->subprograms.end()) m.subprograms[key] = it->second;
            ref = key;
        };
        remap(f.dbg);
        std::set<std::string> globals;
        for (auto& bl : f.blocks)
            for (auto& in : bl.insts) {
                remap(in.dbg);
                if (!in.callee.empty() && !defined.count(in.callee)) want.push_back(in.callee);
                for (auto& o : in.ops) refs_of(o.v, globals);
            }
        for (auto& g : globals) {
            if (m.find_global(g)) continue;
            if (auto* gg = unit->find_global(g)) m.globals.push_back(*gg);
            else if (unit->find(g) && !defined.count(g)) want.push_back(g);
        }
        linked.push_back(name);
        m.functions.push_back(std::move(f));
    }
    return linked;
}

}  // namespace prism::pir
