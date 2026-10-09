#pragma once

// Random C program generators for `prism-qa soundness` (roadmap 5.5 / 6.2).
// Draws come from PyRandom in the same order as the original generators, so
// a seed gives the same program it always gave.

#include <string>

namespace prism::qa {

// scalar expressions, guards, bounded loops, compound assignment
std::string gen_inhouse(long long seed, int nfuncs);
// stack/heap int arrays, masked or reduced indices, pointer walks, memcpy, free
std::string gen_inhouse_ptr(long long seed, int nfuncs);
// symbolic-bound loops over heap strings and counters
std::string gen_inhouse_loop(long long seed, int nfuncs);

}  // namespace prism::qa
