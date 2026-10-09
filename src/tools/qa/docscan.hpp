#pragma once

// Documentation checks shared by prism_docs_check (the fast docs CI job: no
// Z3, no engine; this file and prism::Regex over PCRE2 only), prism-qa and
// the doctests (tests/cpp/test_qa.cpp).
//
//   assurance  every artefact docs/assurance/*.md cites exists (roadmap 6.5)
//   anchors    every Markdown anchor link in docs/, README.md and the report
//              writers (src/**/*.cpp, include/**/*.hpp) resolves (6.4)
//   flags      every flag `prism --help` prints is in docs/USER_GUIDE.md (6.4)

#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace prism::qa {

// GitHub's heading anchor: lower case, punctuation except - and _ dropped,
// spaces to hyphens (inline code and links reduced to their text). \w is
// Unicode-aware, as on GitHub.
std::string slug(std::string_view heading);
// Heading anchors (with GitHub's -1, -2 suffixes for repeats; fenced code
// ignored) and explicit <a id>/<a name> anchors of Markdown text.
std::set<std::string> anchors_of_text(const std::string& text);
std::set<std::string> anchors_of(const std::filesystem::path& md);

// ---- assurance package
//
// Backticked references in docs/assurance/*.md:
//   `path/to/file`        a repository path (a top-level directory or file of
//                         this repository), must exist
//   `path/to/file::name`  the file exists and contains `name` (a test class,
//                         function or doctest TEST_CASE name)
//   `docs/<doc>.md#<a>`  the Markdown file has that heading anchor or <a id>
//   `thm:Name`, `thm:Ns.Name`  `theorem Name` / `lemma Name` declared in
//                         proofs/**/*.lean (last dotted component matched; a
//                         namespace prefix must also occur in the declaring file)
struct AssuranceCounts {
    int files = 0;
    int paths = 0;
    int theorems = 0;
};
struct AssuranceResult {
    std::vector<std::string> errors;  // "<doc>:<line>: <what is missing>"
    AssuranceCounts counts;
};
bool is_path_ref(std::string_view tok);
AssuranceResult assurance_scan(const std::filesystem::path& docs, const std::filesystem::path& repo);
// `assurance-check [--docs DIR] [--repo DIR]`: exit 1 with one line per
// missing artefact, 2 when the documents directory does not exist.
int assurance_main(const std::vector<std::string>& args, const std::filesystem::path& repo);

// ---- anchors
// "<referring file>: <target>#<anchor>" for every anchor link that does not resolve.
std::vector<std::string> anchor_errors(const std::filesystem::path& repo);
int anchors_main(const std::vector<std::string>& args, const std::filesystem::path& repo);

// ---- CLI flags
// Double-dash flags anywhere; single-dash flags only where help text lists an
// option (usage brackets, line start, "--jobs JOBS, -j JOBS"), so prose such
// as "perl -c" is not mistaken for a flag.
std::set<std::string> flags_of(std::string help_text);
// The flag appears inside a backticked span of the guide.
bool documented(const std::string& flag, const std::string& guide);
// The stage list of USER_GUIDE.md's "The stages run in a fixed order
// (`--list-stages`): a, b, ... What" sentence; empty when the sentence is gone.
std::vector<std::string> stage_order_listed(const std::string& guide);

// The repository root: `explicit_root` when given, else the working
// directory when it is one, else the source tree this was built from.
std::filesystem::path repo_root(const std::string& explicit_root = {});
std::string read_text(const std::filesystem::path& p);

}  // namespace prism::qa
