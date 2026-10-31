// Correctness-witness invariants (`prism svcomp`).
//
// Invariant conjuncts the witness may carry. The engine proves its
// invariants in C semantics over bit-vectors, where a signed `+`, `-` or `*`
// in the invariant text wraps; in C that is an overflow (UB), so a conjunct
// with arithmetic is exported only when the other exported conjuncts bound
// every variable in it so tightly that no subterm leaves the range of int
// (then wrapping and C agree, whatever the variables' integer types). A
// plain comparison of identifiers and constants is always exported. Leaving
// out a conjunct keeps the witness valid (each proved conjunct holds by
// itself); it only gives the validator less to work with.
#include "internal.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace prism::svcomp {

using namespace detail;
namespace fs = std::filesystem;

// Only invariants the engine proved are exported: the Houdini-filtered loop
// invariants of a pir PROVED-UNBOUNDED: its structured conjuncts rendered
// in C (debug_ir.cpp), with the loops' source positions in
// extra["invariant_loops"]. A loop whose
// position is unknown (inlined body), that is a `do` loop (its invariant is
// proved at the top of the body, not where the condition is evaluated), or
// whose keyword is not at that position in the task text gets none. Every
// other proof (pir PROVED within the unwind, k-induction without an
// invariant) exports nothing, and the witness is the empty invariant_set:
// trivially valid, the validator has to find the proof itself.
// SVCOMP-ILP32: PROVED* plus invariant_loops is not enough. The conjuncts
// are exported only when this finding is a proof and k_induction is
// closed-invariants (the engine proved those conjuncts).
std::pair<std::vector<Invariant>, std::string> correctness_invariants(const fs::path& task, const json& finding) {
    const json& extra = extra_of(finding);
    const std::string st = get_str(finding, "status", "");
    const bool proved = st == "PROVED-UNBOUNDED" || st == "PROVED" || st == "PROVED-CERTIFIED";
    if (!proved || get_str(extra, "k_induction", "None") != "closed-invariants" || !has(extra, "invariant_conjuncts"))
        return {{}, "no loop invariant exported by the proving stage (empty invariant set)"};
    std::vector<std::vector<std::string>> invs;
    json loops;
    try {
        // structured conjuncts over IR values, rendered in C here
        auto texts = pir_invariant_texts(task, finding);
        if (!texts) return {{}, "pir invariants not mapped to C (no debug information; empty invariant set)"};
        invs = std::move(*texts);
        loops = json::parse(has(extra, "invariant_loops") ? pystr(extra.at("invariant_loops")) : "[]");
    } catch (const std::exception&) {
        return {{}, "unreadable invariants (empty invariant set)"};
    }
    const auto lines = split_nl(read_text(task));
    std::vector<Invariant> out;
    static const Regex kw_re(R"((for|while)\b)");
    static const Regex word_char(R"(\w)");
    static const Regex for_decl(R"(for\s*\(\s*(?:[A-Za-z_]\w*\s+)+\**\s*([A-Za-z_]\w*)\s*=)");
    std::optional<std::string> function;
    if (has(finding, "function") && finding.at("function").is_string() && truthy(finding.at("function")))
        function = finding.at("function").get<std::string>();
    if (loops.is_array()) {
        for (std::size_t j = 0; j < loops.size(); ++j) {
            const json& loop = loops[j];
            if (j >= invs.size() || !loop.is_object()) continue;
            std::string kind = has(loop, "kind") ? pystr(loop.at("kind")) : "None";
            if (kind != "for" && kind != "while" && kind != "") continue;
            long line = has(loop, "line") && truthy(loop.at("line")) ? static_cast<long>(to_int(loop.at("line")).value_or(0)) : 0;
            long col = has(loop, "column") && truthy(loop.at("column")) ? static_cast<long>(to_int(loop.at("column")).value_or(0)) : 0;
            if (!(1 <= line && line <= static_cast<long>(lines.size())) || col < 1) continue;
            const std::string& ltext = lines[static_cast<std::size_t>(line - 1)];
            const std::string at = static_cast<std::size_t>(col - 1) < ltext.size() ? ltext.substr(static_cast<std::size_t>(col - 1)) : "";
            std::string kw = kind;
            if (kw.empty()) {  // pir: the keyword at the loop's start position (a `do` loop gets none)
                auto km = kw_re.match_prefix(at);
                if (!km) continue;
                kw = km->group(1);
            }
            Regex kwb(re_escape(kw) + "\\b");
            if (!kwb.match_prefix(at)) continue;
            if (col > 1 && word_char.match_prefix(std::string_view(ltext).substr(static_cast<std::size_t>(col - 2), 1)))
                continue;
            // a name declared in the for-init is not in scope at the keyword
            auto decl = for_decl.match_prefix(at);
            std::optional<Regex> declared;
            if (decl) declared.emplace("\\b" + re_escape(decl->group(1)) + "\\b");
            std::vector<std::string> mine;
            for (const auto& e : invs[j])
                if (!(declared.has_value() && declared->search(e))) mine.push_back(strip(e));
            // conjuncts are rendered with their C types
            const std::vector<std::string>& kept = mine;
            if (!kept.empty()) {
                std::string value;
                for (std::size_t k = 0; k < kept.size(); ++k) value += (k ? " && " : "") + ("(" + kept.at(k) + ")");
                out.push_back(Invariant{"loop_invariant", Location{task.filename().string(), line, col, function}, value});
            }
        }
    }
    std::size_t n = 0;
    for (const auto& inv : out) {
        std::size_t k = 1, at = 0;
        while ((at = inv.value.find(" && ", at)) != std::string::npos) {
            ++k;
            at += 4;
        }
        n += k;
    }
    std::string stage = has(finding, "stage") ? pystr(finding.at("stage")) : "pir";
    return {out, std::to_string(n) + " Houdini loop invariant conjunct(s) from " + stage};
}

}  // namespace prism::svcomp
