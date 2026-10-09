// prism-deps: supply-chain tool for third_party/MANIFEST.toml (roadmap Part 1).
// Standard library only; see include/prism/deps.hpp.
#include "prism/deps.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    try {
        return prism::deps::deps_main(args, argc > 0 ? argv[0] : "");
    } catch (const std::exception& e) {
        std::cerr << "prism-deps: " << e.what() << "\n";
        return 1;
    }
}
