// prism_docs_check: the documentation checks that need neither the engine
// nor Z3 (the docs CI job builds only this target).
//
//   prism_docs_check assurance-check [--docs DIR] [--repo DIR]
//   prism_docs_check anchors [--repo DIR]
//
// The CLI-flag check needs the engine's --help and runs in prism_tests
// (tests/cpp/test_qa.cpp) and as `prism-qa docs-check`.

#include "prism/exit.hpp"
#include "prism/log.hpp"
#include "docscan.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    prism::log::set_program("prism_docs_check");
    std::vector<std::string> args(argv + 1, argv + argc);
    const std::string cmd = args.empty() ? "" : args.front();
    if (!args.empty()) args.erase(args.begin());
    const auto repo = prism::qa::repo_root();
    if (cmd == "assurance-check") return prism::qa::assurance_main(args, repo);
    if (cmd == "anchors") return prism::qa::anchors_main(args, repo);
    prism::log::plain("usage: prism_docs_check assurance-check [--docs DIR] [--repo DIR]\n"
                 "       prism_docs_check anchors [--repo DIR]\n");
    return cmd == "-h" || cmd == "--help" ? prism::code(prism::Exit::Ok) : prism::code(prism::Exit::Usage);
}
