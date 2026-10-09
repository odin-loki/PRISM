#pragma once

// The YAML subset the conformance task files use (tests/conformance/**.yml):
// block mappings, block sequences (also of mappings: SV-COMP `properties:`),
// flow sequences of scalars (`witness: f: [2147483647, 1]`), plain, 'single'
// and "double" quoted scalars, and `#` comments. Anything else (block
// scalars `|` / `>`, anchors, tags, multi-document streams, flow mappings
// with content) is rejected with an error naming the file and line, never
// read approximately.
//
// Scalars keep their text; the accessors type them the way PyYAML's
// safe_load (YAML 1.1) does, which the task files were written against.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace prism::qa::yaml {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Entry;

struct Node {
    enum class Kind { Null, Scalar, Map, Seq };
    Kind kind = Kind::Null;
    std::string text;  // scalar text (quotes removed, escapes resolved)
    bool quoted = false;
    std::vector<Entry> map;  // insertion order; a repeated key replaces the value
    std::vector<Node> seq;

    bool is_map() const { return kind == Kind::Map; }
    bool is_seq() const { return kind == Kind::Seq; }
    bool is_scalar() const { return kind == Kind::Scalar; }
    // null: absent value, `~`, `null` (plain)
    bool is_null() const;
    // key lookup in a mapping (nullptr when absent or not a mapping)
    const Node* get(std::string_view key) const;
    bool has(std::string_view key) const { return get(key) != nullptr; }

    // YAML 1.1 plain-scalar typing (PyYAML safe_load)
    std::optional<bool> as_bool() const;          // true/false/yes/no/on/off (plain only)
    std::optional<__int128> as_int() const;       // decimal, 0x, 0o/0 octal, 0b, `_` separators (plain only)
    bool is_float() const;                        // plain float literal
    // Python str() of the loaded value: None, True/False, canonical ints, text
    std::string py_str() const;
    // Python `_as_bool` of the scorer: a bool as is, else str(v).strip().lower() == "true"
    bool truthy_label() const;
};

struct Entry {
    std::string first;
    Node second;
};

Node parse(std::string_view text, const std::string& where = "<yaml>");
Node load_file(const std::filesystem::path& p);

}  // namespace prism::qa::yaml
