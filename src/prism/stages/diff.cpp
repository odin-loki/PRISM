// Stage diff: differential testing of same-named function pairs built with gcc/clang.
#include "common.hpp"

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
int c_type_nbytes_key(std::string typ) {
    typ = strip(typ);
    auto it = kCTypeSize.find(typ);
    return it == kCTypeSize.end() ? 4 : it->second;
}

std::string stem_of(const FunctionInfo& fn) {
    return fs::path(fn.file).stem().string();
}

std::vector<std::pair<FunctionInfo, FunctionInfo>> diff_pairs(const std::vector<FunctionInfo>& functions) {
    std::map<std::string, std::vector<FunctionInfo>> by_name;
    for (auto& fn : functions) by_name[fn.name].push_back(fn);
    std::set<const FunctionInfo*> used;
    std::vector<std::pair<FunctionInfo, FunctionInfo>> pairs;
    auto mark = [&](const FunctionInfo& a, const FunctionInfo& b) {
        used.insert(&a);
        used.insert(&b);
        pairs.emplace_back(a, b);
    };
    std::map<std::string, const FunctionInfo*> names;
    for (auto& fn : functions) names[fn.name] = &fn;
    for (auto& fn : functions) {
        if (used.contains(&fn)) continue;
        if (fn.name.ends_with("_a")) {
            auto other = names.find(fn.name.substr(0, fn.name.size() - 2) + "_b");
            if (other != names.end() && !used.contains(other->second)) mark(fn, *other->second);
        }
    }
    for (auto& fn : functions) {
        if (used.contains(&fn)) continue;
        auto d = parse_comments(fn).diff;
        if (!d) continue;
        auto other = names.find(*d);
        if (other != names.end() && !used.contains(other->second)) mark(fn, *other->second);
    }
    for (auto& [nm, group] : by_name) {
        std::vector<const FunctionInfo*> a_fns, b_fns;
        for (auto& f : group) {
            if (used.contains(&f)) continue;
            if (stem_of(f).ends_with("_a")) a_fns.push_back(&f);
            if (stem_of(f).ends_with("_b")) b_fns.push_back(&f);
        }
        for (std::size_t i = 0; i < a_fns.size() && i < b_fns.size(); ++i) mark(*a_fns[i], *b_fns[i]);
    }
    return pairs;
}

std::string emit_diff_program(const FunctionInfo& a, const FunctionInfo& b) {
    std::string args, call;
    for (std::size_t i = 0; i < a.params.size(); ++i) {
        if (i) {
            args += ", ";
            call += ", ";
        }
        auto t = a.params[i].first;
        auto n = a.params[i].second;
        args += (t.empty() ? "int " : t + " ") + n;
        call += n;
    }
    std::string decls, reads;
    int off = 0;
    for (auto& [typ, name] : a.params) {
        auto key = strip(typ);
        if (key.empty()) key = "int";
        int sz = c_type_nbytes_key(key);
        decls += "    " + key + " " + name + ";\n";
        reads += "    memcpy(&" + name + ", buf + " + std::to_string(off) + ", " + std::to_string(sz) + ");\n";
        off += sz;
    }
    int nbytes = off ? off : 1;
    auto strip_star = [](std::string s) {
        s.erase(std::remove(s.begin(), s.end(), '*'), s.end());
        s = strip(s);
        return s.empty() ? "int" : s;
    };
    auto ret_a = strip_star(a.return_type);
    auto ret_b = strip_star(b.return_type);
    return "#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n\nstatic " + ret_a + " impl_a(" +
           args + ") {\n" + a.body + "\n}\nstatic " + ret_b + " impl_b(" + args + ") {\n" + b.body +
           "\n}\n\nint main(void) {\n    unsigned char buf[" + std::to_string(nbytes) +
           "];\n    if (fread(buf, 1, " + std::to_string(nbytes) + ", stdin) != " + std::to_string(nbytes) +
           ") return 0;\n" + decls + reads + "    " + ret_a + " ra = impl_a(" + call + ");\n    " + ret_b +
           " rb = impl_b(" + call +
           ");\n    if (ra != rb) {\n        fprintf(stderr, \"DIFF %d %d\\n\", (int)ra, (int)rb);\n        "
           "return 2;\n    }\n    return 0;\n}\n";
}

std::vector<std::vector<uint8_t>> diff_inputs(int nbytes) {
    std::vector<int> interesting{0,  1,  -1, 2,         3,          42, 99, 100,
                                 127, 128, 255, 0x7FFFFFFF, static_cast<int>(0x80000000), 123456, -99};
    std::vector<std::vector<uint8_t>> out;
    for (int v : interesting) {
        std::vector<uint8_t> raw;
        if (nbytes >= 4) {
            uint32_t u = static_cast<uint32_t>(v);
            raw = {static_cast<uint8_t>(u), static_cast<uint8_t>(u >> 8), static_cast<uint8_t>(u >> 16),
                   static_cast<uint8_t>(u >> 24)};
        } else {
            raw = {static_cast<uint8_t>(v & 0xFF)};
        }
        if (static_cast<int>(raw.size()) > nbytes) raw.resize(static_cast<std::size_t>(nbytes));
        while (static_cast<int>(raw.size()) < nbytes) raw.push_back(0);
        out.push_back(std::move(raw));
    }
    std::vector<uint8_t> rnd(static_cast<std::size_t>(nbytes));
    std::mt19937 rng{std::random_device{}()};
    for (auto& b : rnd) b = static_cast<uint8_t>(rng());
    out.push_back(std::move(rnd));
    return out;
}

Finding diff_pair(const FunctionInfo& a, const FunctionInfo& b, const fs::path&) {
    auto base = make_find("diff", laws::CLEAN, a, "", "", laws::STRENGTH_FINDS);
    base.function = a.name + "/" + b.name;
    if (a.kind != "SCALAR" || b.kind != "SCALAR") {
        base.status = std::string(laws::NEEDS_HARNESS);
        base.message = "differential testing needs two SCALAR functions, got " + a.kind + "/" + b.kind +
                       "; POINTER/OTHER would invent a buffer or object";
        return base;
    }
    if (a.params != b.params) {
        base.status = std::string(laws::ERROR);
        base.message = "parameter lists differ; same bytes would not mean the same arguments";
        return base;
    }
    if (!sandbox::allowed()) {
        // Law 9: the harness compiles and runs both scanned functions.
        auto f = sandbox::exec_notrun("diff", "diff " + a.name + "/" + b.name);
        f.file = a.file;
        f.function = a.name + "/" + b.name;
        f.line = a.line;
        return f;
    }
    auto cc = Config{}.which({"gcc", "clang"});
    if (!cc) {
        base.status = std::string(laws::NOTRUN);
        base.message = "no gcc/clang on PATH";
        base.extra["install"] = "install gcc or clang";
        return base;
    }
    auto src = emit_diff_program(a, b);
    auto td = fs::temp_directory_path() / ("prism_diff_" + std::to_string(std::random_device{}()));
    fs::create_directories(td);
    auto harness = td / "diff.c";
    auto exe = td / "diff.exe";
    {
        std::ofstream out(harness);
        out << src;
    }
    auto cr = run_argv({cc->string(), "-O0", "-g", "-std=c11", harness.string(), "-o", exe.string()}, {}, 30.0);
    if (cr.timeout || cr.rc != 0) {
        fs::remove_all(td);
        base.status = std::string(laws::ERROR);
        auto err = cr.err.empty() ? cr.out : cr.err;
        if (err.size() > 400) err.resize(400);
        base.message = "diff compile: " + err;
        return base;
    }
    int nbytes = param_nbytes(a.params);
    bool timed_out = false;
    for (auto& data : diff_inputs(nbytes)) {
        std::string in(data.begin(), data.end());
        auto rr = run_argv(sandbox::wrap_argv({exe.string()}, td), in, 1.0, sandbox::limits_for(1.0));
        bool disagree = rr.rc == 2 || rr.err.starts_with("DIFF");
        if (disagree) {
            fs::remove_all(td);
            base.status = std::string(laws::FAILED);
            base.cls = "FUNC-CONTRACT";
            base.message = a.name + " and " + b.name + " disagree";
            base.evidence = rr.err.substr(0, 800);
            std::string hex;
            for (auto b : data) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "%02x", b);
                hex += buf;
            }
            base.counterexample = hex;
            base.extra["a"] = a.file;
            base.extra["b"] = b.file;
            return base;
        }
        if (rr.crashed || rr.rc < 0) {
            fs::remove_all(td);
            base.status = std::string(laws::CRASH);
            base.cls = "FUZZ-CRASH";
            std::string hex;
            for (std::size_t i = 0; i < data.size() && i < 16; ++i) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "%02x", data[i]);
                hex += buf;
            }
            base.message = "diff harness crashed on " + hex;
            base.evidence = rr.err.substr(0, 800);
            std::string full;
            for (auto b : data) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "%02x", b);
                full += buf;
            }
            base.counterexample = full;
            return base;
        }
        if (rr.timeout) timed_out = true;
    }
    fs::remove_all(td);
    if (timed_out) {
        base.status = std::string(laws::TIMEOUT);
        base.message = "diff harness timed out (not agreement, not a proof)";
        base.extra["a"] = a.file;
        base.extra["b"] = b.file;
        return base;
    }
    base.status = std::string(laws::CLEAN);
    base.message = "no disagreement on sampled inputs (not a proof)";
    base.extra["a"] = a.file;
    base.extra["b"] = b.file;
    return base;
}

}  // namespace

std::vector<Finding> run_diff(const std::vector<FunctionInfo>& functions, const fs::path& root) {
    auto pairs = diff_pairs(functions);
    if (pairs.empty()) return {};
    std::vector<Finding> out;
    for (auto& [a, b] : pairs) out.push_back(diff_pair(a, b, root));
    return out;
}

}  // namespace prism
