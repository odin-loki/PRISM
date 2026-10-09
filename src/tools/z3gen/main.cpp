// PRISM z3gen: Python-free driver for vendored Z3's CMake codegen scripts.
// Invoked as Python3_EXECUTABLE with the upstream script path as argv[1].
// Copies pre-generated outputs from cmake/z3-generated/ (see cmake/z3.cmake).
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

[[noreturn]] void die(std::string_view msg) {
  std::cerr << "z3gen: " << msg << "\n";
  std::exit(1);
}

fs::path gen_root() {
  if (const char* env = std::getenv("PRISM_Z3_GENERATED_ROOT")) {
    if (env[0] != '\0') return fs::path(env);
  }
  die("PRISM_Z3_GENERATED_ROOT environment variable is not set");
}

std::string basename(const fs::path& p) { return p.filename().string(); }

bool copy_pregen(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    std::cerr << "z3gen: copy " << from << " -> " << to << ": " << ec.message() << "\n";
    return false;
  }
  return true;
}

fs::path pregen_src_rel(const fs::path& abs_under_z3_src) {
  const auto s = abs_under_z3_src.generic_string();
  const auto marker = std::string_view("/third_party/z3/src/");
  const auto pos = s.find(marker);
  if (pos == std::string_view::npos) {
    die("path is not under third_party/z3/src: " + s);
  }
  return fs::path("src") / s.substr(pos + marker.size());
}

int emulate_python_c(int argc, char** argv) {
  if (argc < 3) return 1;
  const std::string_view code = argv[2];
  if (code.find("sys.version_info[:3]") != std::string_view::npos ||
      code.find("sys.version_info[0:3]") != std::string_view::npos) {
    std::cout << "3.11.0";
    return 0;
  }
  if (code.find("sys.version_info[0]") != std::string_view::npos) {
    std::cout << "3";
    return 0;
  }
  if (code.find("sys.abiflags") != std::string_view::npos) {
    return 0;
  }
  std::cerr << "z3gen: unsupported -c snippet\n";
  return 1;
}

int do_pyg2hpp(const std::vector<std::string>& args) {
  if (args.size() < 3) die("pyg2hpp: need PYG DEST_DIR");
  const fs::path pyg = fs::absolute(args[1]);
  const fs::path dest = fs::absolute(args[2]);
  fs::path pregen_rel = pregen_src_rel(pyg);
  pregen_rel.replace_extension(".hpp");
  const fs::path from = gen_root() / pregen_rel;
  if (!fs::exists(from)) die("missing pre-generated " + from.string());
  if (!copy_pregen(from, dest / pregen_rel.filename())) return 1;
  return 0;
}

int do_update_api(const std::vector<std::string>& args) {
  fs::path out_dir;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--api_output_dir" && i + 1 < args.size()) out_dir = fs::absolute(args[i + 1]);
  }
  if (out_dir.empty()) die("update_api: missing --api_output_dir");
  const fs::path base = gen_root() / "src/api";
  for (const char* name : {"api_commands.cpp", "api_log_macros.cpp", "api_log_macros.h"}) {
    if (!copy_pregen(base / name, out_dir / name)) return 1;
  }
  return 0;
}

int do_install_tactic(const std::vector<std::string>& args) {
  if (args.size() < 3) die("mk_install_tactic_cpp: need BUILD_DIR DEPS");
  const fs::path build_dir = fs::absolute(args[1]);
  const fs::path from = gen_root() / "src/api/dll/install_tactic.cpp";
  return copy_pregen(from, build_dir / "install_tactic.cpp") ? 0 : 1;
}

int do_mem_initializer(const std::vector<std::string>& args) {
  if (args.size() < 3) die("mk_mem_initializer_cpp: need BUILD_DIR headers...");
  const fs::path build_dir = fs::absolute(args[1]);
  const fs::path from = gen_root() / "src/api/dll/mem_initializer.cpp";
  return copy_pregen(from, build_dir / "mem_initializer.cpp") ? 0 : 1;
}

int do_gparams_register(const std::vector<std::string>& args) {
  if (args.size() < 3) die("mk_gparams_register_modules_cpp: need BUILD_DIR headers...");
  const fs::path build_dir = fs::absolute(args[1]);
  const fs::path from = gen_root() / "src/api/dll/gparams_register_modules.cpp";
  return copy_pregen(from, build_dir / "gparams_register_modules.cpp") ? 0 : 1;
}

int do_pat_db(const std::vector<std::string>& args) {
  if (args.size() < 3) die("mk_pat_db: need SMT2 OUT_H");
  const fs::path out = fs::absolute(args[2]);
  const fs::path from = gen_root() / "src/ast/pattern/database.h";
  return copy_pregen(from, out) ? 0 : 1;
}

int do_def_file(const std::vector<std::string>& args) {
  if (args.size() < 4) die("mk_def_file: need OUT DLL HEADERS...");
  const fs::path out = fs::absolute(args[1]);
  const std::string dll = args[2];
  std::ofstream fout(out);
  if (!fout) die("cannot open " + out.string());
  fout << "LIBRARY \"" << dll << "\"\nEXPORTS\n";
  int num = 1;
  for (std::size_t i = 3; i < args.size(); ++i) {
    std::ifstream in(args[i]);
    if (!in) die("cannot read " + args[i]);
    std::string line;
    while (std::getline(in, line)) {
      if (line.find("Z3_API") == std::string::npos) continue;
      std::string word;
      std::vector<std::string> words;
      for (char c : line) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
          word.push_back(c);
        } else {
          if (!word.empty()) {
            words.push_back(word);
            word.clear();
          }
        }
      }
      if (!word.empty()) words.push_back(word);
      for (std::size_t w = 0; w + 1 < words.size(); ++w) {
        if (words[w] == "Z3_API") {
          fout << '\t' << words[w + 1] << " @" << num << '\n';
          ++num;
          break;
        }
      }
    }
  }
  return 0;
}

int dispatch_script(const std::vector<std::string>& args) {
  const std::string script = basename(fs::path(args[0]));
  if (script == "pyg2hpp.py") return do_pyg2hpp(args);
  if (script == "update_api.py") return do_update_api(args);
  if (script == "mk_install_tactic_cpp.py") return do_install_tactic(args);
  if (script == "mk_mem_initializer_cpp.py") return do_mem_initializer(args);
  if (script == "mk_gparams_register_modules_cpp.py") return do_gparams_register(args);
  if (script == "mk_pat_db.py") return do_pat_db(args);
  if (script == "mk_def_file.py") return do_def_file(args);
  die("unknown Z3 codegen script: " + script);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string_view(argv[1]) == "-c") return emulate_python_c(argc, argv);
  if (argc < 2) {
    std::cerr << "usage: z3gen [-c CODE | SCRIPT.py ...]\n";
    return 1;
  }
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  return dispatch_script(args);
}
