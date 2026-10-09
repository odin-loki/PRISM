#pragma once

// External suites kept out of git: NIST Juliet C/C++ 1.3 (downloaded,
// sha256-checked, a subset converted to tasks) and ESBMC's C++ regression
// tests (a pinned commit, converted; a curated subset is committed under
// tests/conformance/esbmc-cpp).

#include "replay.hpp"
#include "task.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace prism::qa {

// ---- NIST SARD Juliet C/C++ 1.3
extern const std::string JULIET_URL;
extern const std::string JULIET_SHA256;
extern const long long JULIET_SIZE;
extern const std::vector<std::string> JULIET_CWES;

struct JulietSource {
    std::string url = JULIET_URL;
    std::string sha256 = JULIET_SHA256;
    long long size = JULIET_SIZE;
};

// Download (curl) unless DEST/juliet-c-cplusplus-v1.3.zip has the pinned size,
// check the sha256, extract (unzip) and write DEST/juliet/<CWE>/*.{c,yml}.
// Throws std::runtime_error on a missing tool, a failed download or a hash
// mismatch. Returns DEST/juliet.
fs::path fetch_juliet(const fs::path& dest, const std::vector<std::string>& flows, const JulietSource& src = {});
// One extracted Juliet test case -> (task yml text) or nothing (no good/bad function).
std::optional<std::string> juliet_task(const std::string& base, const std::string& text, const std::string& cwe);
// latin-1 bytes -> UTF-8
std::string latin1_to_utf8(const std::string& s);

// ---- ESBMC C++ regression tests
extern const std::string ESBMC_REPO;
extern const std::string ESBMC_COMMIT;
extern const std::vector<std::string> ESBMC_DIRS;
extern const std::vector<std::string> ESBMC_SKIP_OPTIONS;
extern const std::vector<std::pair<std::string, std::string>> ESBMC_DISPUTED;
extern const std::set<std::string> ESBMC_CURATE_SKIP_CATEGORIES;
// Copyright/licence text of a foreign origin (case-insensitive pattern).
extern const std::string ESBMC_FOREIGN_TEXT;

struct EsbmcTask {
    fs::path source;
    std::string text;
    std::string upstream;  // path below regression/
    std::string level;
    std::string options;
    std::string std;
    bool expected = false;
    int label_bound = 0;
    bool deterministic = false;
    std::vector<std::string> reason;
};

// One ESBMC regression test directory -> (task, "") or (nothing, skip reason).
std::pair<std::optional<EsbmcTask>, std::string> esbmc_convert(const fs::path& test_dir, const fs::path& regression,
                                                               bool thorough = false);
// regression/<dir>/<sub>/.../<test> -> (category, file stem)
std::pair<std::string, std::string> esbmc_task_name(const std::string& upstream);
std::string esbmc_task_yaml(const EsbmcTask& t, const std::string& src_name);
fs::path fetch_esbmc(const fs::path& dest, bool thorough = false);
int curate_esbmc(const fs::path& fetched, const fs::path& dest, int per_verdict, int per_category, const fs::path& work);

}  // namespace prism::qa
