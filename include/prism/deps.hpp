// prism-deps: supply-chain fetcher, verifier, licence firewall and SBOM
// writer driven by third_party/MANIFEST.toml (roadmap Part 1).
//
// Standard library only (plus POSIX process calls and the git CLI at run
// time). It links no third_party/ code, because the Docker build and CI run
// it to verify third_party/ before anything from there is compiled.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace prism::deps {

namespace fs = std::filesystem;

// ------------------------------------------------------------------ errors

// Any failure that must stop the fetch (fail closed).
struct FetchError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct HashMismatch : FetchError {
    using FetchError::FetchError;
};
// The manifest text is not in the TOML subset this reader accepts.
struct TomlError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ----------------------------------------------------------------- hashing

class Sha256 {
public:
    void update(const void* data, std::size_t n);
    void update(std::string_view s) { update(s.data(), s.size()); }
    std::string hex();  // finalises; call once

private:
    void block(const unsigned char* p);
    std::uint32_t h_[8]{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char buf_[64]{};
    std::size_t used_ = 0;
    std::uint64_t bits_ = 0;
};

std::string sha256_hex(std::string_view data);
// Throws FetchError when the file cannot be read.
std::string sha256_file(const fs::path& p);
// SHA-1 (FIPS 180-4) digest bytes; only for RFC 4122 UUIDv5 names.
std::string sha1_bytes(std::string_view data);
// RFC 4122 version 5 UUID of name in namespace ns ("xxxxxxxx-xxxx-...").
std::string uuid5(std::string_view ns_uuid, std::string_view name);

// -------------------------------------------------------------------- TOML

// The subset of TOML used by MANIFEST.toml: top-level keys, [[array]]
// tables, basic and literal strings, integers, booleans and arrays of
// strings (multi-line, trailing comma, comments). Anything else is a
// TomlError: the reader fails closed instead of guessing.
using Value = std::variant<std::string, std::int64_t, bool, std::vector<std::string>>;

struct Table {
    std::vector<std::pair<std::string, Value>> items;  // insertion order

    const Value* find(std::string_view key) const;
    bool has(std::string_view key) const { return find(key) != nullptr; }
    // Python-style truthiness: a missing key, "" and [] are false.
    bool truthy(std::string_view key) const;
    // The string value, or "" when missing; an integer is printed.
    std::string str(std::string_view key) const;
    // The string list, or {} when missing or not a list.
    std::vector<std::string> list(std::string_view key) const;
    Table& set(std::string key, Value v);
    Table& erase(std::string_view key);
};

struct TomlDoc {
    Table root;
    std::vector<std::pair<std::string, std::vector<Table>>> arrays;  // [[name]]

    const std::vector<Table>& array(std::string_view name) const;
};

TomlDoc parse_toml(std::string_view text);

// ---------------------------------------------------------------- manifest

inline constexpr const char* kStamp = "PRISM-TOOL.json";

struct Manifest {
    fs::path path;
    fs::path root;  // the manifest's grandparent (the repository)
    std::string bytes;
    TomlDoc doc;
    const std::vector<Table>& components() const { return doc.array("component"); }
};

// Reads and validates every [[component]]; throws TomlError / FetchError.
Manifest load_manifest(const fs::path& path);
// Reads the TOML without validating the components (licence-check, sbom).
Manifest read_manifest(const fs::path& path);
void validate_component(const Table& c);
std::vector<const Table*> components(const Manifest& m, std::string_view kind = {});
// By component name or by one of its stages.
const Table& find_component(const Manifest& m, std::string_view name);
// Walk up from start for third_party/MANIFEST.toml.
std::optional<fs::path> find_manifest(const fs::path& start);

// $PRISM_TOOLS_DIR (with ~ expanded) or ~/.prism/tools.
fs::path tools_dir();
fs::path install_dir(const Table& comp, const fs::path& base);

// ---------------------------------------------------------- linked checks

// Files under root, sorted by POSIX relative path, skipping __pycache__/.git
// directories and *.gguf / *.pyc files.
std::vector<fs::path> tree_files(const fs::path& root);
// SHA-256 over sorted "<relpath>\0<sha256>\n" lines of every file under root.
std::string tree_digest(const fs::path& root);
// Python-`re` pattern (with re.S) turned into an ECMAScript one; throws
// FetchError on syntax std::regex would read differently.
std::string py_regex_to_ecma(std::string_view py);
// nullopt when the component has no version marker; "" when it does not match.
std::optional<std::string> tree_version(const Table& comp, const fs::path& root);
std::vector<std::string> check_linked(const Table& comp, const fs::path& root);
std::vector<std::string> diff_against_upstream(const Table& comp, const fs::path& upstream,
                                               const fs::path& root);
// Sort key of drift lines: by path, then by the D/M/A letter.
void sort_drift(std::vector<std::string>& lines);

// --------------------------------------------------------------------- tar

// Extracts a (git-archive) tar into dest with the safety rules of Python's
// tarfile "data" filter: no absolute paths, no "..", no links leaving dest,
// no device files; group/other write bits cleared. Returns the single
// top-level directory. Throws FetchError.
fs::path extract_tar(const fs::path& tar, const fs::path& dest);

// ----------------------------------------------------------------- process

struct ProcResult {
    int code = -1;
    std::string out;
    std::string err;
};
// Runs argv (PATH lookup) in cwd. capture: stdout/stderr collected;
// otherwise inherited. stdout_file (when set) receives stdout.
ProcResult run_process(const std::vector<std::string>& argv, const fs::path& cwd,
                       bool capture = true, const fs::path* stdout_file = nullptr);
std::optional<fs::path> which(std::string_view name);

// -------------------------------------------------------------------- fetch

// git fetch the pinned commit and write its git-archive tar to dest_tar.
// FetchError on any failure, HashMismatch when the tar does not hash to
// archive_sha256; dest_tar is removed on failure.
void fetch_archive(const Table& comp, const fs::path& dest_tar);
// Fetch + verify + (optionally) build one external tool; returns its dir.
// Staged in a temporary sibling, renamed into place only when every check
// and build step passed.
fs::path fetch_tool(const Table& comp, const fs::path& base, bool build = true);
int run_linked(const Manifest& m, bool refetch);

// ------------------------------------------------------------------ licence

// "permissive" | "weak" | "strong" for one SPDX id (optionally "X WITH exc").
std::string classify(std::string_view spdx_id);
// Licence ids of an SPDX expression, WITH-exception kept attached.
std::vector<std::string> spdx_ids(std::string_view expr);
std::vector<std::string> licence_problems(const std::vector<Table>& comps, const fs::path& root);

// --------------------------------------------------------------------- sbom

// CycloneDX 1.5 JSON (Python json.dumps(indent=2) layout) plus a newline.
std::string build_sbom(const std::string& manifest_bytes, const std::string& prism_version);

// --------------------------------------------------------------------- cli

// prism-deps list | linked [--refetch] | tool NAME... [--no-build]
//   [--tools-dir D] | tree-digest DIR | sbom [--version V] [-o F]
//   | licence-check | pins -o FILE ; every command takes --manifest P.
// Exit codes: 0 ok, 1 check failed, 2 usage or bad manifest, 3 hash mismatch.
int deps_main(const std::vector<std::string>& args, const fs::path& argv0 = {});

}  // namespace prism::deps
