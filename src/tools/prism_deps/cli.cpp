// prism-deps command line (see deps.hpp for the commands and exit codes).
#include "prism/deps.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

namespace prism::deps {

namespace {

constexpr const char* kUsage =
    "usage: prism-deps COMMAND [--manifest PATH] ...\n"
    "\n"
    "Fetch and verify PRISM's pinned dependencies (third_party/MANIFEST.toml).\n"
    "Fails closed: any hash, commit or version mismatch exits non-zero and\n"
    "leaves nothing installed.\n"
    "\n"
    "  list                          print the manifest\n"
    "  linked [--refetch]            in-tree linked libraries match the manifest\n"
    "                                (--refetch: and the pinned upstream archives)\n"
    "  tool NAME... [--no-build] [--tools-dir DIR]\n"
    "                                fetch, verify and build external tools into\n"
    "                                DIR (default $PRISM_TOOLS_DIR or ~/.prism/tools)\n"
    "  tree-digest DIR               print the tree_sha256 of a directory\n"
    "  licence-check                 fail when a linked component is copyleft\n"
    "  sbom [--version V] [-o FILE]  CycloneDX 1.5 SBOM\n"
    "  pins -o FILE                  C++ header of the external tool pins (build)\n"
    "\n"
    "Exit codes: 0 ok, 1 check failed, 2 usage or bad manifest, 3 hash mismatch.\n";

int usage(const std::string& why) {
    if (!why.empty()) std::cerr << "prism-deps: " << why << "\n";
    std::cerr << kUsage;
    return 2;
}

std::string pad(std::string s, std::size_t w) {
    if (s.size() < w) s.append(w - s.size(), ' ');
    return s;
}

bool write_if_changed(const fs::path& p, const std::string& text) {
    {
        std::ifstream in(p, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            if (ss.str() == text) return true;
        }
    }
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o << text;
    o.close();
    return static_cast<bool>(o);
}

}  // namespace

int deps_main(const std::vector<std::string>& args, const fs::path& argv0) {
    if (args.empty()) return usage("");
    const std::string cmd = args[0];
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        std::cout << kUsage;
        return 0;
    }
    std::optional<fs::path> manifest, tools, output;
    std::optional<std::string> version;
    bool refetch = false, no_build = false;
    std::vector<std::string> pos;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& a = args[i];
        auto value = [&](const char* flag) -> std::optional<std::string> {
            if (i + 1 >= args.size()) return std::nullopt;
            (void)flag;
            return args[++i];
        };
        if (a == "--manifest") {
            auto v = value("--manifest");
            if (!v) return usage("--manifest needs a path");
            manifest = *v;
        } else if (a == "--tools-dir") {
            auto v = value("--tools-dir");
            if (!v) return usage("--tools-dir needs a path");
            tools = *v;
        } else if (a == "--version") {
            auto v = value("--version");
            if (!v) return usage("--version needs a value");
            version = *v;
        } else if (a == "-o" || a == "--output") {
            auto v = value("-o");
            if (!v) return usage("-o needs a path");
            output = *v;
        } else if (a == "--refetch") {
            refetch = true;
        } else if (a == "--no-build") {
            no_build = true;
        } else if (a.size() > 1 && a[0] == '-') {
            return usage("unknown option " + a);
        } else {
            pos.push_back(a);
        }
    }
    auto allow = [&](bool ok, const char* what) { return ok ? std::string() : std::string(what); };
    std::string bad;
    if (cmd == "tree-digest") bad = allow(pos.size() == 1, "tree-digest takes one DIR");
    else if (cmd == "tool") bad = allow(!pos.empty(), "tool needs at least one NAME");
    else if (cmd == "list" || cmd == "linked" || cmd == "licence-check" || cmd == "sbom" || cmd == "pins")
        bad = allow(pos.empty(), "unexpected argument");
    else return usage("unknown command " + cmd);
    if (bad.empty() && refetch && cmd != "linked") bad = "--refetch is for linked";
    if (bad.empty() && (no_build || tools) && cmd != "tool") bad = "--no-build/--tools-dir are for tool";
    if (bad.empty() && version && cmd != "sbom") bad = "--version is for sbom";
    if (bad.empty() && output && cmd != "sbom" && cmd != "pins") bad = "-o is for sbom and pins";
    if (bad.empty() && cmd == "pins" && !output) bad = "pins needs -o FILE";
    if (!bad.empty()) return usage(bad);

    if (cmd == "tree-digest") {
        std::error_code ec;
        if (!fs::is_directory(pos[0], ec)) {
            std::cerr << "prism-deps: " << pos[0] << " is not a directory\n";
            return 2;
        }
        try {
            std::cout << tree_digest(pos[0]) << "\n";
        } catch (const FetchError& e) {
            std::cerr << "prism-deps: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    if (!manifest) {
        std::error_code ec;
        manifest = find_manifest(fs::current_path(ec));
        if (!manifest && !argv0.empty()) manifest = find_manifest(fs::absolute(argv0, ec).parent_path());
        if (!manifest) {
            std::cerr << "prism-deps: third_party/MANIFEST.toml not found above the current directory; "
                         "pass --manifest PATH\n";
            return 2;
        }
    }

    Manifest m;
    try {
        m = (cmd == "licence-check" || cmd == "sbom") ? read_manifest(*manifest) : load_manifest(*manifest);
    } catch (const std::exception& e) {
        std::cerr << "prism-deps: bad manifest: " << e.what() << "\n";
        return 2;
    }

    if (cmd == "list") {
        for (const Table* c : components(m)) {
            auto commit = c->has("commit") ? c->str("commit").substr(0, 12) : std::string("-");
            std::cout << pad(c->str("kind"), 8) << " " << pad(c->str("name"), 14) << " "
                      << pad(c->str("version"), 10) << " " << pad(commit, 12) << " " << c->str("spdx") << "\n";
        }
        return 0;
    }
    if (cmd == "linked") return run_linked(m, refetch);
    if (cmd == "licence-check") {
        auto problems = licence_problems(m.components(), m.root);
        int n = 0;
        for (const auto& c : m.components())
            if (c.str("kind") == "linked") {
                std::cout << "linked   " << pad(c.str("name"), 14) << " " << c.str("spdx") << "\n";
                ++n;
            }
        if (!problems.empty()) {
            std::cout.flush();
            for (const auto& p : problems) std::cerr << "FAIL " << p << "\n";
            return 1;
        }
        std::cout << "licence check OK: " << n << " linked components, none copyleft\n";
        return 0;
    }
    if (cmd == "sbom") {
        std::string v = version ? *version : [] {
            const char* e = std::getenv("PRISM_VERSION");
            return std::string(e ? e : "0.0.0-dev");
        }();
        std::string text;
        try {
            text = build_sbom(m.bytes, v);
        } catch (const std::exception& e) {
            std::cerr << "prism-deps: " << e.what() << "\n";
            return 2;
        }
        if (output) {
            std::ofstream o(*output, std::ios::binary | std::ios::trunc);
            o << text;
            o.close();
            if (!o) {
                std::cerr << "prism-deps: cannot write " << output->string() << "\n";
                return 1;
            }
            std::cout << "wrote " << output->string() << " (" << m.components().size() << " components)\n";
        } else {
            std::cout << text;
        }
        return 0;
    }
    if (cmd == "pins") {
        std::string text =
            "// Generated by prism-deps from third_party/MANIFEST.toml. Do not edit.\n"
            "#pragma once\n\nnamespace prism {\n"
            "struct ManifestPin {\n    const char* name;\n    const char* commit;\n};\n"
            "inline constexpr ManifestPin kManifestPins[] = {\n";
        for (const Table* c : components(m, "external"))
            text += "    {\"" + c->str("name") + "\", \"" + c->str("commit") + "\"},\n";
        text += "};\n}  // namespace prism\n";
        if (!write_if_changed(*output, text)) {
            std::cerr << "prism-deps: cannot write " << output->string() << "\n";
            return 1;
        }
        return 0;
    }
    // tool NAME...
    int rc = 0;
    const fs::path base = tools ? *tools : fs::path();
    for (const auto& name : pos) {
        try {
            fetch_tool(find_component(m, name), base, !no_build);
        } catch (const HashMismatch& e) {
            std::cerr << "prism-deps: " << e.what() << "\n";
            return 3;
        } catch (const FetchError& e) {
            std::cerr << "prism-deps: " << e.what() << "\n";
            rc = 1;
        } catch (const fs::filesystem_error& e) {
            std::cerr << "prism-deps: " << e.what() << "\n";
            rc = 1;
        }
    }
    return rc;
}

}  // namespace prism::deps
