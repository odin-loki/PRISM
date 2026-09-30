// prism_ai command dispatch (the library part; main.cpp only forwards to it).

#include "prism_ai.hpp"

#include <ostream>
#include <string>

namespace prism_ai {

int main_dispatch(int argc, char** argv, std::ostream& out, std::ostream& err) {
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "predict") return predict::predict_main(argc - 2, argv + 2, out, err);
    if (cmd == "replay") return sched::replay_main(argc - 2, argv + 2, out, err);
    if (cmd == "e2e") return e2e::e2e_main(argc - 2, argv + 2, out, err);
    if (cmd == "measure") return measure::measure_main(argc - 2, argv + 2, out, err);
    if (cmd == "sample-pairs") return measure::sample_pairs_main(argc - 2, argv + 2, out, err);
    err << "usage: prism_ai COMMAND ...\n"
           "  predict       train, measure and export the solver / bound model (--out MODEL.json)\n"
           "  replay        replay the portfolio scheduler on alone-times (--solve-log FILE)\n"
           "  e2e           end-to-end A/B of the learned scheduler (--prism BIN --solve-log FILE --out DIR)\n"
           "  measure       {triage,ask,regress,draft} OUT: per-feature metrics for docs/AI.md\n"
           "  sample-pairs  OUT: seeded sample of the finding pairs triage merges\n";
    return cmd.empty() || cmd == "--help" || cmd == "-h" ? 0 : 2;
}

}  // namespace prism_ai
