#pragma once

// SV-COMP witnesses (format 2.0, YAML) from PRISM findings (roadmap 6.3).
//
// A PRISM FAILED finding of main carries the values returned by the
// __VERIFIER_nondet_* calls on the violating path, in call order (bmc and
// pir report them in extra["nondet"]). A violation witness is one
// violation_sequence entry:
//
//     - entry_type: violation_sequence
//       metadata: {format_version: "2.0", uuid, creation_time, producer, task}
//       content:
//         - segment: [ {waypoint: {type: function_return, action: follow,
//                                  location: {...}, constraint: {value: "\result == 5",
//                                                                format: acsl_expression}}} ]
//         - ...
//         - segment: [ {waypoint: {type: target, action: follow, location: {...}}} ]
//
// A correctness witness is one invariant_set entry whose invariants the
// engine proved. Format reference: https://gitlab.com/sosy-lab/benchmarking/sv-witnesses
// (format/schemas/archive/v2.0). Nothing here validates a witness: the
// witnesses of the pinned subset were checked with CPAchecker 4.2.2 and
// UAutomizer 0.3.1 outside this repository (docs/SVCOMP.md, "Witness
// validation").

#include "prism/export.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace prism::svcomp {

using ojson = nlohmann::ordered_json;

inline constexpr const char* WITNESS_FORMAT_VERSION = "2.0";
inline constexpr const char* WITNESS_PRODUCER = "PRISM";

// A value a __VERIFIER_nondet_* call returned: an integer (kept exactly,
// 128-bit) or a floating-point value.
struct Num {
    bool is_float = false;
    __int128 i = 0;
    double f = 0.0;
    static Num of_int(__int128 v) { return Num{false, v, 0.0}; }
    static Num of_float(double v) { return Num{true, 0, v}; }
    double as_double() const { return is_float ? f : static_cast<double>(i); }
    bool operator==(const Num& o) const {
        return is_float == o.is_float && (is_float ? f == o.f : i == o.i);
    }
};

// Decimal text of a 128-bit integer.
PRISM_API std::string int_text(__int128 v);
// The shortest text that reads back as v, spelled like Python's repr(float):
// "0.5", "2147483647.0", "1e+16", "1.5e-05", "nan", "inf".
PRISM_API std::string float_repr(double v);
// A constant for `\result == <constant>` (ACSL): integers in decimal, floats
// as float_repr.
PRISM_API std::string acsl_literal(const Num& v);

struct Location {
    std::string file_name;
    long line = 0;
    std::optional<long> column = 1;  // nullopt: no column (the line's first statement)
    std::optional<std::string> function;
    PRISM_API ojson as_dict() const;
};

// One value returned by a __VERIFIER_nondet_* call on the violating path;
// location is the call's closing parenthesis (format 2.0 function_return).
struct NondetValue {
    Location location;
    Num value;
};

struct Counterexample {
    std::string function;
    Location target;
    std::vector<NondetValue> nondet;
};

// One entry of a correctness witness (format 2.0 invariant_set). kind is
// "loop_invariant" (location: the first character of the for/while/do
// keyword) or "location_invariant".
struct Invariant {
    std::string kind;
    Location location;
    std::string value;
};

struct WitnessMeta {
    std::filesystem::path input_file;
    std::string input_file_name;
    std::string specification;
    std::string data_model = "LP64";
    std::string language = "C";
    std::string producer_version = "unknown";
    std::optional<std::string> creation_time;  // default: now, UTC, "%Y-%m-%dT%H:%M:%SZ"
    std::optional<std::string> uuid;           // default: a random UUID version 4
};

PRISM_API ojson build_violation_witness(const Counterexample& cex, const WitnessMeta& meta);
PRISM_API ojson build_correctness_witness(const std::vector<Invariant>& invariants, const WitnessMeta& meta);

// Block-style YAML of a witness document (maps keep their insertion order).
PRISM_API std::string to_yaml(const ojson& doc);
PRISM_API bool write_witness(const ojson& doc, const std::filesystem::path& path);

}  // namespace prism::svcomp
