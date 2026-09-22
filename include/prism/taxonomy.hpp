#pragma once

#include "prism/export.hpp"
#include "prism/models.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace prism {

struct TaxonomyClass {
    std::string id;
    std::string name;
    std::vector<int> cwe;
    std::vector<std::pair<std::string, std::string>> seen;  // instrument, strength
};

struct TaxonomyRow : TaxonomyClass {
    std::string best;
    std::string verdict;  // COVERED | PARTIAL | GAP
};

PRISM_API const std::vector<TaxonomyClass>& taxonomy_classes();
PRISM_API std::vector<TaxonomyRow> coverage_from_report(const RunReport& report);
// Python engine refuse_llm_cover: LLM cannot COVER. READS or only-LLM hits demote COVERED to PARTIAL.
PRISM_API std::string refuse_llm_cover(const RunReport& report, const std::string& cid,
                                       std::string verdict, const std::string& best);

}  // namespace prism
