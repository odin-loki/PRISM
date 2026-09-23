// Library operational models (roadmap 2.6, docs/PIR.md "Library models").
//
// The models are C files (src/prism/pir/models/libc/*.c) embedded in the
// binary at build time. Once per run they are lowered by the same clang ->
// opt pipeline as scanned code (plus -fno-builtin -ffreestanding so clang
// does not replace a model by the builtin it models), and every function a
// unit declares but does not define is linked into that unit's module
// (transitively, with the globals it uses). Linking happens on the parsed
// ir::Module (the models' metadata references are renamed so they cannot
// collide with the unit's), so no llvm-link run is needed.

#include "prism/pir.hpp"

#include "../proc.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

namespace prism::pir {

// generated at configure time from src/prism/pir/models (CMakeLists.txt)
const std::vector<std::pair<std::string, std::string>>& embedded_models();

const std::vector<std::pair<std::string, std::string>>& model_sources() { return embedded_models(); }

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
        out.push_back(text[i]);
    }
    return out;
}

void refs_of(const ir::Value& v, std::set<std::string>& globals) {
    if (v.kind == ir::Value::Global) globals.insert(v.name);
    for (auto& e : v.elems) refs_of(e.v, globals);
}

}  // namespace

ModelLibrary build_models(const Frontend& fe, double timeout_s) {
    ModelLibrary lib;
    if (!fe.clang || !fe.opt) {
        lib.error = "clang/opt not found";
        return lib;
    }
    static std::atomic<unsigned> seq{0};
    std::random_device rd;
    auto dir = fs::temp_directory_path() / ("prism_models_" + std::to_string(rd()) + "_" + std::to_string(seq++));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        lib.error = "cannot create a temporary directory";
        return lib;
    }
    for (auto& [name, text] : model_sources()) {
        auto dst = dir / (name.ends_with(".h") ? name : "prism_model_" + name);
        std::ofstream(dst, std::ios::binary) << text;
    }
    for (auto& [name, text] : model_sources()) {
        if (!name.ends_with(".c")) continue;
        auto src = dir / ("prism_model_" + name);
        auto o0 = dir / (name + ".o0.ll"), o1 = dir / (name + ".ll");
        auto r = detail::run_process({fe.clang->string(), "-S", "-emit-llvm", "-O0", "-Xclang", "-disable-O0-optnone",
                                      "-fno-builtin", "-ffreestanding", "-fno-discard-value-names",
                                      "-gline-tables-only", "-std=c17", "-w", "-o", o0.string(), src.string()},
                                     timeout_s);
        if (r.failed || r.timed_out || r.rc != 0) {
            lib.error = "model " + name + ": " + first_error_line(r.text);
            break;
        }
        auto ro = detail::run_process({fe.opt->string(), "-passes=mem2reg,lowerswitch,loop-simplify,lcssa,instnamer",
                                       "-S", "-o", o1.string(), o0.string()},
                                      timeout_s);
        if (ro.failed || ro.timed_out || ro.rc != 0) {
            lib.error = "model " + name + " (opt): " + first_error_line(ro.text);
            break;
        }
        auto tag = name.substr(0, name.size() - 2);
        lib.units.push_back(ir::parse_module(rename_privates(slurp(o1), tag)));
    }
    fs::remove_all(dir, ec);
    if (!lib.error.empty()) lib.units.clear();
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
