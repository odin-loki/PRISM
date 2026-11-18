#pragma once

// `prism sim ...`: opt-in similarity search over SMT-LIB corpora (docs/SIM_SEARCH.md).
//   prism sim index PATH... [-o FILE] [--labels TSV] [--threads N] [--alpha A]
//   prism sim query FILE.smt2 [-i FILE] [-k N] [--exact] [--ted]
//   prism sim hint  FILE.smt2 [-i FILE] [-k N]
// argv[0] is the subcommand (index, query, hint).

#include "prism/export.hpp"

namespace prism::sim {
PRISM_API int sim_main(int argc, char** argv);
}
