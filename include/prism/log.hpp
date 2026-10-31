#pragma once

// PRISM's one logging interface. Deliberately thin: a level, a component
// name and a message go to one sink. The default sink writes stderr; a host
// (later FORGE's logger) replaces it with set_sink(). Nothing else in PRISM
// should write diagnostics to stderr directly.
//
//   error / warn  user-facing problems: "prism <component>: error: <msg>"
//   trace         diagnostics, printed only when PRISM_MEM_TRACE (memory) or
//                 the caller's own switch enables them: "[<component>] <msg>"

#include "prism/export.hpp"

#include <string_view>

namespace prism::log {

enum class Level { Trace, Info, Warn, Error };

using Sink = void (*)(Level, std::string_view component, std::string_view message);

// Replace the sink (nullptr restores the stderr default). Returns the old one.
PRISM_API Sink set_sink(Sink s);

PRISM_API void write(Level level, std::string_view component, std::string_view message);

inline void error(std::string_view component, std::string_view message) { write(Level::Error, component, message); }
inline void warn(std::string_view component, std::string_view message) { write(Level::Warn, component, message); }
inline void info(std::string_view component, std::string_view message) { write(Level::Info, component, message); }
inline void trace(std::string_view component, std::string_view message) { write(Level::Trace, component, message); }

}  // namespace prism::log
