#pragma once

// Hardware/device model and driver-module layer (docs/DEVICE_MODEL.md).
//
// A device model (.pdm) is a small text format: registers with offset, width,
// access type (RW/RO/WO/W1C), reset value, read semantics, write effects,
// spontaneous events, interrupt lines. A module description (.pmod) names a
// driver source, its entry points and its environment. prism::module turns the
// two into one C translation unit (driver + device model + kernel stubs +
// harness `main`) which the existing pir stage checks; it then reads the
// verdict back as module properties with the device model named in the
// assumption list. Nothing here proves anything by itself: every verdict is
// the pir stage's, and a result that rests on a device model is L4 only with
// the model named, and never above BOUNDED/PROVED-ASSUMING language.

#include "prism/config.hpp"
#include "prism/export.hpp"
#include "prism/models.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace prism::module {

struct ParseError : std::runtime_error {
    int line;
    ParseError(const std::string& file, int line_, const std::string& what)
        : std::runtime_error(file + ":" + std::to_string(line_) + ": " + what), line(line_) {}
};

// ---------------------------------------------------------------------------
// Device model
// ---------------------------------------------------------------------------

enum class Access { RW, RO, WO, W1C };

struct Register {
    std::string name;
    std::uint64_t offset = 0;
    unsigned width = 32;        // bits: 8, 16, 32, 64
    Access access = Access::RW;
    std::uint64_t reset = 0;
    std::uint64_t mask = ~std::uint64_t{0};   // writable bits (RW/WO/W1C defaults)
    // Read: exactly one of read_expr (a function of device state), read_in
    // (any value satisfying a constraint on `result`), read_unconstrained, or
    // none of them (the stored value). Expressions are C-syntax text with
    // every identifier already resolved (validated) at parse time.
    std::string read_expr;
    std::string read_in;
    bool read_unconstrained = false;
    std::string on_read;        // statements run before the read value is formed
    std::string write_body;     // statements; empty = the access type's default effect
    bool has_write_body = false;
    // group (an array of identical registers, e.g. identification bytes)
    bool group = false;
    std::uint64_t group_end = 0;  // exclusive end offset
    int line = 0;
};

struct StateVar {
    std::string name;
    unsigned width = 32;
    std::uint64_t init = 0;
    bool init_nondet = false;
};

struct Event {
    std::string name;
    std::string body;           // C statements
    int line = 0;
};

struct IrqLine {
    std::string name;
    std::string level_expr;     // C expression, nonzero = asserted
};

enum class Policy { Zero, Unconstrained, Violation, Ignore };

struct DevModel {
    std::string name;
    std::string file;
    std::vector<std::string> doc;
    std::vector<std::string> sources;  // provenance lines ("QEMU hw/rtc/pl031.c@...")
    std::string validation = "unvalidated";  // unvalidated | datasheet | differential
    std::string validation_how;
    std::uint64_t window = 0x1000;
    std::vector<StateVar> state;
    std::vector<Register> regs;
    std::string init_body;
    std::vector<Event> events;
    std::vector<IrqLine> irqs;
    Policy unmapped_read = Policy::Violation;
    Policy unmapped_write = Policy::Violation;
    Policy wo_read = Policy::Violation;
    Policy ro_write = Policy::Violation;
    bool strict_width = true;
    std::vector<std::string> dma_notes;  // dma rules are parsed and listed, not yet enforced
};

PRISM_API DevModel parse_device_model(std::string_view text, const std::string& file = "<device model>");
// The C translation of the model: prism_dev_read/write/irq_level/event/init.
PRISM_API std::string emit_device_c(const DevModel& m);
// One paragraph per register for reports and the assumption list.
PRISM_API std::string describe_device(const DevModel& m);

// ---------------------------------------------------------------------------
// Module description and harness
// ---------------------------------------------------------------------------

struct Op {
    std::string name;
    std::string body;           // C statements, a function body returning void
};

struct Spec {
    std::string name;
    std::string file;
    std::string kernel = "linux";           // linux | toy
    std::string driver;                     // path under the kernel root, or absolute
    std::vector<std::string> extra_sources; // more .c files included before the harness
    std::string device;                     // .pdm path (relative to the spec) or "builtin:<name>"; empty = no device model
    std::vector<std::string> stubs;         // kernel stub groups
    int steps = 2;                          // operations between probe and remove
    bool irq_at_api = true;                 // interrupts may fire at kernel API calls inside probe/remove
    bool irq_at_mmio = false;               // ... and at every MMIO access (larger)
    bool irq_at_steps = true;               // ... and between steps
    std::string irq_spurious = "auto";      // auto (shared lines only) | yes | no
    bool teardown_ops = true;               // one operation after remove returns, before devres release
    std::vector<std::string> properties;    // empty = all
    std::string env;                        // file-scope C declarations
    std::string setup;                      // C statements run before probe
    std::string probe;                      // int prism_probe(void) body
    std::string remove;                     // void prism_remove(void) body
    std::vector<Op> ops;
    std::string includes_pre;               // extra #include lines before the driver
    std::vector<std::string> defines;       // `define NAME [VALUE];` -> #define before the driver (variants of a fixture)
    std::vector<std::string> assumptions;   // free text, copied to the report
};

PRISM_API Spec parse_module_spec(std::string_view text, const std::string& file = "<module spec>");

// Entry points found in the driver's registration tables: `.field = symbol`
// designated initialisers whose symbol is a function defined in the file.
struct Registered {
    std::string field, symbol;
    int line = 0;
    bool exercised = false;     // appears in the module description's entry blocks
};
PRISM_API std::vector<Registered> discover_entry_points(std::string_view driver_source, const Spec& spec);

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------

struct RunOptions {
    std::filesystem::path spec;
    std::filesystem::path kernel_root;      // the Linux tree the driver path is relative to
    std::filesystem::path cc_like;          // compile_commands.json with an entry to clone for the harness unit
    std::string cc_like_file;               // suffix of that entry's file ("drivers/rtc/rtc-cmos.c"); empty = first
    std::filesystem::path out{"prism-out/module"};
    std::vector<std::string> properties;    // override the spec (empty = spec)
    int steps = -1;                         // override (-1 = spec)
    std::string device_override;            // another model file ("none" = no device model, unconstrained reads)
    bool generate_only = false;             // write the unit and stop (no pir run)
    std::vector<std::string> rows;          // run only these rows (empty = all): all, all-sequential, memory, or a property
    std::vector<std::string> defines;       // extra `define`s on top of the spec's
};

struct PropertyRow {
    std::string property;
    std::string verdict;        // exactly as docs/VERDICTS.md names it, or NOTRUN
    std::string level;          // "L4" only with a device model, else "L1"/"L4-" text; see check.cpp
    std::string detail;
};

struct Report {
    std::string module, device, device_validation, unit_path, cdb_path;
    std::string verdict;        // the pir verdict of the harness function
    std::string cls, message, counterexample, line;
    std::string level;
    std::vector<std::string> assumptions;
    std::vector<std::string> properties_checked;
    std::vector<Registered> entry_points;
    std::map<std::string, std::string> extra;
    double seconds = 0;
    std::string notes;
    std::string json() const;
    std::string text() const;
};

// Writes <out>/<module>/harness.c (+ headers, compile_commands.json) and,
// unless generate_only, runs the pir stage on it.
PRISM_API Report check_module(const RunOptions& opt, const Config& cfg);

// Embedded kernel-stub and preamble files: name -> text (device/<name>).
PRISM_API const std::vector<std::pair<std::string, std::string>>& embedded_device_files();

}  // namespace prism::module
