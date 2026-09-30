// prism-qa: PRISM's quality-assurance tools. Not part of the release archive:
// it grades an engine binary (any --prism / $PRISM_BIN), so it does not share
// a binary with the engine it grades.
//
//   prism-qa conformance [...]   conformance suite and release gate
//   prism-qa soundness [...]     random-program soundness campaign

#include "support/cli.hpp"
#include "support/proctree.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    prism::qa::install_interrupt_cleanup();
    std::vector<std::string> args(argv + 1, argv + argc);
    auto usage = [](std::ostream& os) {
        os << "usage: prism-qa {conformance,soundness} [options]\n"
              "  conformance  run the conformance suite and the release gate (0 wrong proofs)\n"
              "  soundness    random-program soundness campaign\n"
              "Run `prism-qa SUBCOMMAND --help` for the options.\n";
    };
    if (args.empty()) {
        usage(std::cerr);
        return 2;
    }
    const std::string sub = args.front();
    args.erase(args.begin());
    if (sub == "conformance") return prism::qa::conformance_main(args);
    if (sub == "soundness") return prism::qa::soundness_main(args);
    if (sub == "-h" || sub == "--help") {
        usage(std::cout);
        return 0;
    }
    std::cerr << "prism-qa: unknown subcommand '" << sub << "'\n";
    usage(std::cerr);
    return 2;
}
