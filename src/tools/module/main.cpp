// prism-module: check a driver module against a device model
// (docs/DEVICE_MODEL.md). One command:
//
//   prism-module run SPEC.pmod --kernel-root LINUX --cc-like DB.json [--cc-like-file SUFFIX]
//                [--out DIR] [--steps N] [--ops a,b] [--teardown-ops yes/no] [--mem-mb N] [--row-jobs N] [--in-process] [--rows a,b] [--define NAME[ VALUE]] [--properties a,b] [--device FILE|none] [--timeout S] [--json]
//   prism-module device FILE.pdm            parse a device model and print its description
//   prism-module device-c FILE.pdm          print the C the model is translated to
//   prism-module entries SPEC.pmod --kernel-root LINUX
#include "prism/config.hpp"
#include "prism/module.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

namespace {

std::string slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + p);
    std::ostringstream o;
    o << in.rdbuf();
    return o.str();
}

std::vector<std::string> csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

int usage() {
    std::cerr << "usage: prism-module run SPEC.pmod --kernel-root DIR [--cc-like DB [--cc-like-file SUFFIX]] [--out DIR]\n"
                 "                    [--steps N] [--ops a,b] [--teardown-ops yes/no] [--mem-mb N] [--row-jobs N] [--in-process] [--rows a,b] [--define NAME[ VALUE]] [--properties a,b] [--device FILE|none] [--timeout S] [--generate-only] [--json]\n"
                 "       prism-module device FILE.pdm | device-c FILE.pdm | entries SPEC.pmod --kernel-root DIR\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    namespace pm = prism::module;
    if (argc < 3) return usage();
    const std::string cmd = argv[1];
    try {
        if (cmd == "device" || cmd == "device-c") {
            auto m = pm::parse_device_model(slurp(argv[2]), argv[2]);
            std::cout << (cmd == "device" ? pm::describe_device(m) : pm::emit_device_c(m));
            return 0;
        }
        if (cmd == "row") {   // one row of a module check, in this process: the child of `run` (docs/DEVICE_MODEL.md section 6)
            prism::Config rc = prism::default_config();
            rc.llm = false;
            std::filesystem::path cdb;
            bool model_only = false;
            for (int i = 3; i < argc; ++i) {
                std::string a = argv[i];
                auto next = [&]() -> std::string {
                    if (i + 1 >= argc) throw std::runtime_error("missing value after " + a);
                    return argv[++i];
                };
                if (a == "--root") rc.root = next();
                else if (a == "--cdb") cdb = next();
                else if (a == "--model-only") model_only = true;
                else if (a == "--timeout") rc.timeout = std::stod(next());
                else if (a == "--unwind") rc.unwind = std::stoi(next());
                else return usage();
            }
            std::cout << pm::run_row_json(argv[2], cdb, model_only, rc) << "\n";
            return 0;
        }
        pm::RunOptions o;
        prism::Config cfg = prism::default_config();
        bool json = false;
        o.spec = argv[2];
        {
            std::error_code ec;
            auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
            if (!ec) o.self_exe = self;
        }
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value after " + a);
                return argv[++i];
            };
            if (a == "--kernel-root") o.kernel_root = next();
            else if (a == "--cc-like") o.cc_like = next();
            else if (a == "--cc-like-file") o.cc_like_file = next();
            else if (a == "--out") o.out = next();
            else if (a == "--steps") o.steps = std::stoi(next());
            else if (a == "--teardown-ops") o.teardown_ops = next() == "yes" ? 1 : 0;
            else if (a == "--ops") o.ops = csv(next());
            else if (a == "--mem-mb") o.mem_mb = std::stoull(next());
            else if (a == "--row-jobs") o.row_jobs = std::stoi(next());
            else if (a == "--in-process") o.self_exe.clear();
            else if (a == "--properties") o.properties = csv(next());
            else if (a == "--device") o.device_override = next();
            else if (a == "--rows") o.rows = csv(next());
            else if (a == "--define") o.defines.push_back(next());
            else if (a == "--timeout") cfg.timeout = std::stod(next());
            else if (a == "--unwind") cfg.unwind = std::stoi(next());
            else if (a == "--jobs") cfg.jobs = std::stoi(next());
            else if (a == "--generate-only") o.generate_only = true;
            else if (a == "--json") json = true;
            else return usage();
        }
        cfg.llm = false;
        if (cmd == "entries") {
            auto sp = pm::parse_module_spec(slurp(argv[2]), argv[2]);
            std::filesystem::path d = std::filesystem::path(sp.driver).is_absolute() ? std::filesystem::path(sp.driver) : o.kernel_root / sp.driver;
            for (auto& e : pm::discover_entry_points(slurp(d.string()), sp))
                std::cout << e.field << " = " << e.symbol << "  (line " << e.line << ")  " << (e.exercised ? "exercised" : "NOT exercised") << "\n";
            return 0;
        }
        if (cmd != "run") return usage();
        auto rep = pm::check_module(o, cfg);
        std::cout << (json ? rep.json() : rep.text()) << "\n";
        return rep.verdict == "FAILED" ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "prism-module: " << e.what() << "\n";
        return 3;
    }
}
