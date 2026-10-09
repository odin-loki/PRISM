// docs-check: the user documentation is complete (roadmap 6.4).
//
// - every flag `prism --help` and `prism prove --help` print is documented
//   in docs/USER_GUIDE.md (inside a backticked span);
// - the guide's stage-order sentence lists include/prism/pipeline.hpp's
//   STAGE_ORDER exactly;
// - every Markdown anchor link in docs/, README.md and the report writers
//   resolves (docscan.hpp anchor_errors);
// - every verdict has the docs/VERDICTS.md anchor the reports link to.

#include "qa.hpp"

#include "../../prism/proc.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"
#include "prism/shipdocs.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>

namespace fs = std::filesystem;

namespace prism::qa {

std::vector<std::string> undocumented_flags(const std::string& help, const std::string& guide) {
    std::vector<std::string> out;
    for (const auto& f : flags_of(help))
        if (!documented(f, guide)) out.push_back(f);
    return out;  // flags_of is a sorted set
}

int docs_check_main(const Args& args) {
    const auto repo = repo_root();
    std::string bin;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--bin" && i + 1 < args.size()) bin = args[++i];
        else if (args[i] == "-h" || args[i] == "--help") {
            std::cout << "prism-qa docs-check [--bin PRISM]\n"
                         "Every --help flag is in docs/USER_GUIDE.md, the stage order is listed, every\n"
                         "Markdown anchor link resolves and every verdict has its VERDICTS anchor.\n";
            return 0;
        } else {
            std::cerr << "docs-check: unknown option " << args[i] << "\n";
            return 2;
        }
    }
    int bad = 0;
    const auto guide = read_text(repo / "docs" / "USER_GUIDE.md");
    auto exe = find_prism(bin, repo);
    if (!exe) {
        std::cout << "docs-check: NOTRUN: no PRISM binary: the --help flags were not checked\n";
        ++bad;
    } else {
        for (const auto& sub : std::vector<std::vector<std::string>>{{"--help"}, {"prove", "--help"}}) {
            std::vector<std::string> argv{exe->string()};
            argv.insert(argv.end(), sub.begin(), sub.end());
            detail::SessionOptions so;
            so.timeout_s = 60;
            auto r = detail::run_session(argv, so);
            const std::string who = sub.size() == 1 ? "prism" : "prism prove";
            if (r.rc != 0 || flags_of(r.out).empty()) {
                std::cout << who << " --help: exit " << r.rc << ", no flags parsed\n";
                ++bad;
                continue;
            }
            for (const auto& f : undocumented_flags(r.out, guide)) {
                std::cout << who << " --help flag missing from docs/USER_GUIDE.md: " << f << "\n";
                ++bad;
            }
        }
    }
    std::vector<std::string> want;
    for (auto* s : STAGE_ORDER)
        if (s) want.emplace_back(s);
    if (stage_order_listed(guide) != want) {
        std::cout << "docs/USER_GUIDE.md: the stage-order sentence does not list STAGE_ORDER\n";
        ++bad;
    }
    for (const auto& e : anchor_errors(repo)) {
        std::cout << "broken anchor: " << e << "\n";
        ++bad;
    }
    const auto anchors = anchors_of(repo / "docs" / "VERDICTS.md");
    for (auto v : {laws::PROVED_CERTIFIED, laws::PROVED_UNBOUNDED, laws::PROVED, laws::PROVED_ASSUMING,
                   laws::BOUNDED, laws::FAILED, laws::UNKNOWN, laws::TIMEOUT, laws::ERROR, laws::NOFUNC,
                   laws::NOTRUN, laws::NEEDS_HARNESS, laws::CRASH, laws::CLEAN, laws::NOSEED, laws::SANFAIL,
                   laws::HYPOTHESIS, laws::READS}) {
        std::string a = "verdict-";
        for (char c : v) a += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (prism::verdict_anchor(v) != a || !anchors.contains(a)) {
            std::cout << "docs/VERDICTS.md needs #" << a << " (reports link to it)\n";
            ++bad;
        }
    }
    std::cout << "docs-check: " << bad << " problem" << (bad == 1 ? "" : "s") << "\n";
    return bad ? 1 : 0;
}

}  // namespace prism::qa
