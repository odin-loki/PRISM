// prism-deps: supply-chain tool for third_party/MANIFEST.toml (roadmap Part 1).
// Standard library only; see include/prism/deps.hpp.
#include "prism/exit.hpp"
#include "prism/log.hpp"
#include "prism/deps.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    prism::log::set_program("prism-deps");
    std::vector<std::string> args(argv + 1, argv + argc);
    try {
        return prism::deps::deps_main(args, argc > 0 ? argv[0] : "");
    } catch (const std::exception& e) {
        prism::log::error("", e.what());
        return prism::code(prism::Exit::Failed);
    }
}
